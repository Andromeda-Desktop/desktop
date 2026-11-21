#include "dbus/file_manager1.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "net/url_open.h"

#include <glib.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>

#include <optional>
#include <vector>

namespace {
  constexpr Logger kLog("file-manager1");

  std::string fileUri(const std::filesystem::path& path) {
    gchar* raw = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (raw == nullptr) {
      return "file://" + path.string();
    }
    std::string uri(raw);
    g_free(raw);
    return uri;
  }
} // namespace

FileManager1::FileManager1(SessionBus* bus) : m_bus(bus) {}

FileManager1::~FileManager1() = default;

void FileManager1::showItem(const std::filesystem::path& file, const std::string& activationToken) {
  const std::filesystem::path folder = file.parent_path();
  if (m_bus == nullptr) {
    (void)net::openInBrowser(fileUri(folder), activationToken);
    return;
  }
  if (m_proxy == nullptr) {
    m_proxy = sdbus::createProxy(
        m_bus->connection(), sdbus::ServiceName{"org.freedesktop.FileManager1"},
        sdbus::ObjectPath{"/org/freedesktop/FileManager1"}
    );
  }
  m_proxy->callMethodAsync("ShowItems")
      .onInterface("org.freedesktop.FileManager1")
      .withArguments(std::vector<std::string>{fileUri(file)}, activationToken)
      .uponReplyInvoke([folder, activationToken](std::optional<sdbus::Error> error) {
        // No file manager owns FileManager1: open the folder itself.
        if (error.has_value() && !net::openInBrowser(fileUri(folder), activationToken)) {
          kLog.warn("showing {} failed: {}", folder.string(), error->what());
        }
      });
}
