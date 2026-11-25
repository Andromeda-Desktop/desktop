#include "app/file_chooser.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "protocols/bus_names.h"
#include "protocols/uri.h"

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <format>
#include <map>
#include <vector>

namespace desk {

  namespace {
    constexpr Logger kLog("file-chooser");
    const sdbus::ServiceName kPortal{busname::kPortalFrontend};
    const sdbus::ObjectPath kPath{"/org/freedesktop/portal/desktop"};
    constexpr auto kFileChooser = "org.freedesktop.portal.FileChooser";
    constexpr auto kRequest = "org.freedesktop.portal.Request";
    using Options = std::map<std::string, sdbus::Variant>;

    std::vector<std::filesystem::path> pickedPaths(std::uint32_t response, const Options& results) {
      std::vector<std::filesystem::path> paths;
      const auto it = results.find("uris");
      if (response != 0 || it == results.end() || !it->second.containsValueOfType<std::vector<std::string>>()) {
        return paths;
      }
      for (const auto& item : it->second.get<std::vector<std::string>>()) {
        if (auto path = uri::pathFromFileUri(item)) {
          paths.emplace_back(std::move(*path));
        }
      }
      return paths;
    }

    std::function<void(std::vector<std::filesystem::path>)>
    takeFront(std::function<void(std::optional<std::filesystem::path>)> picked) {
      return [picked = std::move(picked)](std::vector<std::filesystem::path> paths) {
        picked(paths.empty() ? std::nullopt : std::optional(std::move(paths.front())));
      };
    }
  } // namespace

  struct FileChooser::Impl {
    void request(const char* method, const std::string& parentWindow, const std::string& title, Options options,
                 std::function<void(std::vector<std::filesystem::path>)> picked);

    SessionBus* m_bus = nullptr;
    std::unique_ptr<sdbus::IProxy> m_portal;
    std::unique_ptr<sdbus::IProxy> m_request;
    std::uint64_t m_seq = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

  FileChooser::FileChooser(SessionBus* bus) : m_impl(std::make_unique<Impl>()) { m_impl->m_bus = bus; }

  FileChooser::~FileChooser() = default;

  void FileChooser::chooseFolder(const std::string& parentWindow, const std::string& title,
                                 std::function<void(std::optional<std::filesystem::path>)> picked) {
    m_impl->request("OpenFile", parentWindow, title, {{"directory", sdbus::Variant{true}}}, takeFront(std::move(picked)));
  }

  void FileChooser::openFile(const std::string& parentWindow, const std::string& title,
                             std::function<void(std::optional<std::filesystem::path>)> picked) {
    m_impl->request("OpenFile", parentWindow, title, {}, takeFront(std::move(picked)));
  }

  void FileChooser::openFiles(const std::string& parentWindow, const std::string& title,
                              std::function<void(std::vector<std::filesystem::path>)> picked) {
    m_impl->request("OpenFile", parentWindow, title, {{"multiple", sdbus::Variant{true}}}, std::move(picked));
  }

  void FileChooser::saveFile(const std::string& parentWindow, const std::string& title, const std::string& currentName,
                             std::function<void(std::optional<std::filesystem::path>)> picked) {
    m_impl->request("SaveFile", parentWindow, title, {{"current_name", sdbus::Variant{currentName}}}, takeFront(std::move(picked)));
  }

  void FileChooser::Impl::request(const char* method, const std::string& parentWindow, const std::string& title,
                                  Options options, std::function<void(std::vector<std::filesystem::path>)> picked) {
    const std::uint64_t seq = ++m_seq;
    if (m_bus == nullptr) {
      kLog.warn("file chooser portal: no session bus");
      picked({});
      return;
    }
    const std::string token = std::format("desk_choose{}", seq);
    try {
      if (m_portal == nullptr) {
        m_portal = sdbus::createProxy(m_bus->connection(), kPortal, kPath);
      }
      std::string sender = m_bus->connection().getUniqueName().substr(1);
      std::ranges::replace(sender, '.', '_');
      // Subscribed before the call, so a fast Response is never missed.
      m_request = sdbus::createProxy(
          m_bus->connection(), kPortal,
          sdbus::ObjectPath{std::format("/org/freedesktop/portal/desktop/request/{}/{}", sender, token)});
      m_request->uponSignal("Response").onInterface(kRequest).call(
          [this, seq, picked, alive = std::weak_ptr<bool>(m_alive)](std::uint32_t response, const Options& results) {
            // The request proxy runs this handler; the next request replaces it only after the handler returns.
            DeferredCall::callLater([this, seq, picked, alive, paths = pickedPaths(response, results)] {
              if (!alive.expired() && seq == m_seq) {
                picked(paths);
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
              picked({});
            }
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("file chooser portal: {}", e.getMessage());
      picked({});
    }
  }

} // namespace desk
