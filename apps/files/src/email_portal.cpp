#include "email_portal.h"

#include "core/log.h"
#include "dbus/caller.h"
#include "dbus/session_bus.h"

#include <glib.h>
#include <sdbus-c++/sdbus-c++.h>

#include <optional>
#include <string>
#include <vector>

namespace {
  constexpr Logger kLog("email-portal");
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.Email";
  using Options = std::map<std::string, sdbus::Variant>;

  template <typename T> std::optional<T> option(const Options& options, const char* key) {
    const auto it = options.find(key);
    if (it == options.end()) {
      return std::nullopt;
    }
    try {
      return it->second.get<T>();
    } catch (const sdbus::Error& e) {
      kLog.warn("option {} has the wrong type: {}", key, e.what());
      return std::nullopt;
    }
  }

  // Addresses go to xdg-email as positional words: one that starts with '-' would be read as an option.
  bool plainAddress(const std::string& address) {
    return !address.empty() && address.front() != '-' && address.find('@') != std::string::npos
        && address.find_first_of(" \t\r\n") == std::string::npos;
  }

  // Empty when an address is unsafe to pass on.
  std::optional<std::vector<std::string>> xdgEmailArgs(const Options& options) {
    std::vector<std::string> args{"xdg-email", "--utf8"};
    std::vector<std::string> to = option<std::vector<std::string>>(options, "addresses").value_or({});
    if (const auto address = option<std::string>(options, "address")) {
      to.insert(to.begin(), *address);
    }
    for (const auto& [key, flag] : {std::pair{"cc", "--cc"}, std::pair{"bcc", "--bcc"}}) {
      for (const std::string& address : option<std::vector<std::string>>(options, key).value_or({})) {
        if (!plainAddress(address)) {
          return std::nullopt;
        }
        args.insert(args.end(), {flag, address});
      }
    }
    if (const auto subject = option<std::string>(options, "subject")) {
      args.insert(args.end(), {"--subject", *subject});
    }
    if (const auto body = option<std::string>(options, "body")) {
      args.insert(args.end(), {"--body", *body});
    }
    for (const std::string& path : option<std::vector<std::string>>(options, "attachments").value_or({})) {
      args.insert(args.end(), {"--attach", path});
    }
    for (const std::string& address : to) {
      if (!plainAddress(address)) {
        return std::nullopt;
      }
      args.push_back(address);
    }
    return args;
  }
} // namespace

struct EmailPortal::Pending {
  sdbus::Result<std::uint32_t, Options> result;
};

EmailPortal::EmailPortal(SessionBus& bus) : m_bus(bus) {
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(sdbus::registerMethod("ComposeEmail")
                      .withInputParamNames("handle", "app_id", "parent_window", "options")
                      .withOutputParamNames("response", "results")
                      .implementedAs([this](sdbus::Result<std::uint32_t, Options>&& result, sdbus::ObjectPath /*handle*/,
                                            std::string appId, std::string /*parent*/, Options options) {
                        requirePortalFrontend(m_bus.connection());
                        const auto args = xdgEmailArgs(options);
                        if (!args) {
                          kLog.warn("{}: refused an address that is not a plain e-mail address", appId);
                          result.returnResults(2, Options{});
                          return;
                        }
                        std::vector<char*> argv;
                        for (const std::string& arg : *args) {
                          argv.push_back(const_cast<char*>(arg.c_str()));
                        }
                        argv.push_back(nullptr);
                        GPid pid = 0;
                        GError* error = nullptr;
                        const auto flags = static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD);
                        if (!g_spawn_async(nullptr, argv.data(), nullptr, flags, nullptr, nullptr, &pid, &error)) {
                          kLog.warn("xdg-email: {}", error->message);
                          g_error_free(error);
                          result.returnResults(2, Options{});
                          return;
                        }
                        const std::uint64_t id = m_nextId++;
                        m_pending.emplace(id, std::make_unique<Pending>(Pending{std::move(result)}));
                        struct Watch {
                          EmailPortal* portal;
                          std::weak_ptr<bool> alive;
                          std::uint64_t id;
                        };
                        g_child_watch_add_full(
                            G_PRIORITY_DEFAULT, pid,
                            [](GPid child, gint status, gpointer data) {
                              g_spawn_close_pid(child);
                              const auto* watch = static_cast<Watch*>(data);
                              if (watch->alive.expired()) {
                                return;
                              }
                              auto node = watch->portal->m_pending.extract(watch->id);
                              if (node.empty()) {
                                return;
                              }
                              const bool ok = g_spawn_check_wait_status(status, nullptr);
                              if (!ok) {
                                kLog.warn("xdg-email failed (no mail app?)");
                              }
                              node.mapped()->result.returnResults(ok ? 0U : 2U, Options{});
                            },
                            new Watch{this, m_alive, id}, [](gpointer data) { delete static_cast<Watch*>(data); });
                      }))
      .forInterface(kInterface);
}

EmailPortal::~EmailPortal() { *m_alive = false; }
