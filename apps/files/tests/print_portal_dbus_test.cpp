// Print portal contract over a private session bus (meson runs it under dbus-run-session). No printer is needed:
// the sheet is stood in by the open/closed handlers.
#include "core/deferred_call.h"
#include "dbus/session_bus.h"
#include "print_portal.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "print_portal_dbus_test: {}", what);
      ++g_failures;
    }
  }

  using Options = std::map<std::string, sdbus::Variant>;
  const sdbus::ServiceName kBusName{"org.freedesktop.impl.portal.desktop.andromeda.Files"};
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.Print";

  template <typename Predicate> bool pumpUntil(SessionBus& bus, Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      for (auto& callback : DeferredCall::takePending()) {
        callback();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready();
  }

  struct Reply {
    std::atomic<bool> done = false;
    std::uint32_t response = 99;
    Options results;
  };

  template <typename T> T get(const Options& o, const char* key, T fallback) {
    const auto it = o.find(key);
    return it == o.end() ? fallback : it->second.get<T>();
  }
} // namespace

int main() {
  const std::filesystem::path state = std::filesystem::temp_directory_path() / ("print-test-" + std::to_string(getpid()));
  {
    std::ofstream(state) << "[org.example.App]\nprinter=Office\n\n[org.example.App/Office]\nmedia=na_letter_8.5x11in\n"
                            "copies=3\nnumber-up=2\n";
  }
  SessionBus bus;
  std::vector<std::pair<std::uint64_t, PrintPortal::PrintRequest>> opened;
  std::vector<std::uint64_t> closed;
  PrintPortal portal(
      bus,
      {.open = [&](std::uint64_t id, const PrintPortal::PrintRequest& r) { opened.emplace_back(id, r); },
       .closed = [&](std::uint64_t id) { closed.push_back(id); }},
      state);
  bus.connection().requestName(kBusName);

  auto client = sdbus::createSessionBusConnection();
  auto proxy = sdbus::createProxy(*client, kBusName, kObjectPath);
  client->enterEventLoopAsync();

  // PreparePrint answers at once: the remembered paper for the app, and settings that leave copies, ranges and
  // pages per sheet to the sheet.
  {
    Reply reply;
    proxy->callMethodAsync("PreparePrint")
        .onInterface(kInterface)
        .withArguments(sdbus::ObjectPath{"/org/freedesktop/portal/desktop/request/1_1/p1"},
                       std::string{"org.example.App"}, std::string{"wayland:abc"}, std::string{"Trip plan"},
                       Options{}, Options{}, Options{})
        .uponReplyInvoke([&reply](std::optional<sdbus::Error> error, std::uint32_t response, Options results) {
          if (!error) {
            reply.response = response;
            reply.results = std::move(results);
          }
          reply.done = true;
        });
    expect(pumpUntil(bus, [&] { return reply.done.load(); }), "PreparePrint answers without a sheet");
    expect(reply.response == 0 && opened.empty(), "PreparePrint returns 0 and opens no sheet");
    const auto setup = get(reply.results, "page-setup", Options{});
    const auto settings = get(reply.results, "settings", Options{});
    expect(get(setup, "Width", 0.0) > 215.8 && get(setup, "Width", 0.0) < 216.0, "page-setup width is Letter");
    expect(get(settings, "printer", std::string{}) == "Office", "settings name the remembered printer");
    expect(get(settings, "n-copies", std::string{}) == "1" && get(settings, "number-up", std::string{}) == "1",
           "the app renders one copy, one page per sheet");
    expect(reply.results.contains("token"), "PreparePrint returns a token");
  }

  const auto print = [&](const std::string& handle, Reply& reply) {
    const int fd = memfd_create("doc", 0);
    expect(write(fd, "%PDF-1.4\n", 9) == 9, "write the document");
    proxy->callMethodAsync("Print")
        .onInterface(kInterface)
        .withArguments(sdbus::ObjectPath{handle}, std::string{"org.example.App"}, std::string{"wayland:abc"},
                       std::string{"Trip plan"}, sdbus::UnixFd{fd, sdbus::adopt_fd}, Options{})
        .uponReplyInvoke([&reply](std::optional<sdbus::Error> error, std::uint32_t response, Options) {
          if (!error) {
            reply.response = response;
          }
          reply.done = true;
        });
  };

  // Print opens the sheet with the document and the remembered options; Cancel answers 1.
  {
    Reply reply;
    print("/org/freedesktop/portal/desktop/request/1_1/p2", reply);
    expect(pumpUntil(bus, [&] { return opened.size() == 1; }), "Print opens the sheet");
    if (opened.size() == 1) {
      const auto& r = opened[0].second;
      expect(r.title == "Trip plan" && r.parentWindow == "wayland:abc", "the sheet gets title and parent");
      expect(r.remembered.copies == 3 && r.remembered.printer == "Office", "the sheet gets the remembered options");
      char head[5] = {};
      expect(pread(r.fd, head, 4, 0) == 4 && std::string(head) == "%PDF", "the sheet can read the document");
      portal.cancel(opened[0].first);
    }
    expect(pumpUntil(bus, [&] { return reply.done.load(); }) && reply.response == 1, "Cancel answers 1");
    expect(portal.idle(), "no request is left after Cancel");
  }

  // The caller's Close answers 2 and closes the sheet.
  {
    Reply reply;
    const std::string handle = "/org/freedesktop/portal/desktop/request/1_1/p3";
    print(handle, reply);
    expect(pumpUntil(bus, [&] { return opened.size() == 2; }), "second Print opens the sheet");
    auto request = sdbus::createProxy(*client, kBusName, sdbus::ObjectPath{handle});
    request->callMethodAsync("Close").onInterface("org.freedesktop.impl.portal.Request").uponReplyInvoke(
        [](std::optional<sdbus::Error>) {});
    expect(pumpUntil(bus, [&] { return reply.done.load() && closed.size() == 1; }), "Close ended the call");
    expect(reply.response == 2, "Close returns response 2");
    expect(portal.idle(), "no request is left after Close");
  }

  std::filesystem::remove(state);
  return g_failures == 0 ? 0 : 1;
}
