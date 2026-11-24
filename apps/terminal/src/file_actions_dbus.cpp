#include "file_actions.h"

#include "core/log.h"
#include "protocols/uri.h"
#include "dbus/session_bus.h"
#include "files/file_ops_client.h"

#include <glib.h>
#include <sdbus-c++/sdbus-c++.h>

namespace term {

  namespace {
    constexpr Logger kLog("browse");

    FileOpsClient::Started started(FileActions::Done done) {
      return [done = std::move(done)](const std::string& /*jobPath*/, const std::string& error) {
        if (done) {
          done(error);
        }
      };
    }
  } // namespace

  struct FileActions::Impl {
    SessionBus* bus = nullptr;
    std::unique_ptr<FileOpsClient> ops;
    std::unique_ptr<sdbus::IProxy> fileManager;

    // Null without a session bus: every operation then reports that.
    FileOpsClient* client() {
      if (ops == nullptr && bus != nullptr) {
        ops = std::make_unique<FileOpsClient>(*bus);
      }
      return ops.get();
    }
  };

  FileActions::FileActions(SessionBus* bus) : m_impl(std::make_unique<Impl>()) { m_impl->bus = bus; }

  FileActions::~FileActions() = default;

  void FileActions::copy(const Paths& paths, const std::filesystem::path& dest, Done done) {
    if (auto* c = m_impl->client()) {
      c->copy(paths, dest, started(std::move(done)));
    } else if (done) {
      done("no session bus");
    }
  }

  void FileActions::move(const Paths& paths, const std::filesystem::path& dest, Done done) {
    if (auto* c = m_impl->client()) {
      c->move(paths, dest, started(std::move(done)));
    } else if (done) {
      done("no session bus");
    }
  }

  void FileActions::trash(const Paths& paths, Done done) {
    if (auto* c = m_impl->client()) {
      c->trash(paths, started(std::move(done)));
    } else if (done) {
      done("no session bus");
    }
  }

  void FileActions::rename(const std::filesystem::path& path, const std::string& name, Done done) {
    if (auto* c = m_impl->client()) {
      c->rename(path, name, started(std::move(done)));
    } else if (done) {
      done("no session bus");
    }
  }

  void FileActions::renameMany(const std::vector<std::pair<std::filesystem::path, std::string>>& items, Done done) {
    if (auto* c = m_impl->client()) {
      c->renameMany(items, started(std::move(done)));
    } else if (done) {
      done("no session bus");
    }
  }

  void FileActions::showInFiles(const Paths& paths) {
    if (m_impl->bus == nullptr || paths.empty()) {
      return;
    }
    std::vector<std::string> uris;
    for (const auto& path : paths) {
      if (path.is_absolute()) {
        uris.push_back(uri::fileUri(path.string()));
      }
    }
    try {
      if (m_impl->fileManager == nullptr) {
        m_impl->fileManager =
            sdbus::createProxy(m_impl->bus->connection(), sdbus::ServiceName{"org.freedesktop.FileManager1"},
                               sdbus::ObjectPath{"/org/freedesktop/FileManager1"});
      }
      m_impl->fileManager->callMethodAsync("ShowItems")
          .onInterface("org.freedesktop.FileManager1")
          .withArguments(uris, std::string())
          .uponReplyInvoke([](std::optional<sdbus::Error> error) {
            if (error) {
              kLog.warn("ShowItems: {}", error->getMessage());
            }
          });
    } catch (const sdbus::Error& error) {
      kLog.warn("ShowItems: {}", error.getMessage());
    }
  }

} // namespace term
