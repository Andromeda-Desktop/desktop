#include "app/file_chooser.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"

#include <glib.h>
#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <format>
#include <map>
#include <vector>

namespace desk {

  namespace {
    constexpr Logger kLog("file-chooser");
    const sdbus::ServiceName kPortal{"org.freedesktop.portal.Desktop"};
    const sdbus::ObjectPath kPath{"/org/freedesktop/portal/desktop"};
    constexpr auto kFileChooser = "org.freedesktop.portal.FileChooser";
    constexpr auto kRequest = "org.freedesktop.portal.Request";
    using Options = std::map<std::string, sdbus::Variant>;

    std::optional<std::filesystem::path> firstPath(std::uint32_t response, const Options& results) {
      const auto it = results.find("uris");
      if (response != 0 || it == results.end() || !it->second.containsValueOfType<std::vector<std::string>>()) {
        return std::nullopt;
      }
      const auto uris = it->second.get<std::vector<std::string>>();
      if (uris.empty()) {
        return std::nullopt;
      }
      gchar* file = g_filename_from_uri(uris.front().c_str(), nullptr, nullptr);
      if (file == nullptr) {
        return std::nullopt;
      }
      std::filesystem::path path(file);
      g_free(file);
      return path;
    }
  } // namespace

  FileChooser::FileChooser(SessionBus& bus) : m_bus(bus) {}

  FileChooser::~FileChooser() = default;

  void FileChooser::chooseFolder(const std::string& parentWindow, const std::string& title,
                                 std::function<void(std::optional<std::filesystem::path>)> picked) {
    request("OpenFile", parentWindow, title, {{"directory", sdbus::Variant{true}}}, std::move(picked));
  }

  void FileChooser::saveFile(const std::string& parentWindow, const std::string& title, const std::string& currentName,
                             std::function<void(std::optional<std::filesystem::path>)> picked) {
    request("SaveFile", parentWindow, title, {{"current_name", sdbus::Variant{currentName}}}, std::move(picked));
  }

  void FileChooser::request(const char* method, const std::string& parentWindow, const std::string& title,
                            Options options, std::function<void(std::optional<std::filesystem::path>)> picked) {
    const std::uint64_t seq = ++m_seq;
    const std::string token = std::format("desk_choose{}", seq);
    try {
      if (m_portal == nullptr) {
        m_portal = sdbus::createProxy(m_bus.connection(), kPortal, kPath);
      }
      std::string sender = m_bus.connection().getUniqueName().substr(1);
      std::ranges::replace(sender, '.', '_');
      // Subscribed before the call, so a fast Response is never missed.
      m_request = sdbus::createProxy(
          m_bus.connection(), kPortal,
          sdbus::ObjectPath{std::format("/org/freedesktop/portal/desktop/request/{}/{}", sender, token)});
      m_request->uponSignal("Response").onInterface(kRequest).call(
          [this, seq, picked, alive = std::weak_ptr<bool>(m_alive)](std::uint32_t response, const Options& results) {
            // The request proxy runs this handler; the next request replaces it only after the handler returns.
            DeferredCall::callLater([this, seq, picked, alive, path = firstPath(response, results)] {
              if (!alive.expired() && seq == m_seq) {
                picked(path);
              }
            });
          });
      options.emplace("handle_token", sdbus::Variant{token});
      options.emplace("modal", sdbus::Variant{true});
      m_portal->callMethodAsync(method)
          .onInterface(kFileChooser)
          .withArguments(parentWindow, title, options)
          .uponReplyInvoke([this, seq, picked, method, alive = std::weak_ptr<bool>(m_alive)](
                               std::optional<sdbus::Error> error, sdbus::ObjectPath /*request*/) {
            if (error.has_value() && !alive.expired() && seq == m_seq) {
              kLog.warn("{}: {}", method, error->getMessage());
              picked(std::nullopt);
            }
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("file chooser portal: {}", e.getMessage());
      picked(std::nullopt);
    }
  }

} // namespace desk
