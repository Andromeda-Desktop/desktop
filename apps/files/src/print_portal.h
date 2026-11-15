#pragma once

#include "print_settings.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

class SessionBus;

namespace sdbus {
  class IObject;
}

// org.freedesktop.impl.portal.Print, served by `andromeda-files --picker`. PreparePrint answers at once with the
// settings remembered for the app, so the app lays out its pages for the remembered paper. Print(fd) opens the sheet
// with the rendered document; the sheet answers with cancel() or print().
class PrintPortal {
public:
  struct PrintRequest {
    std::string appId;
    std::string parentWindow; // "wayland:<handle>" or ""
    std::string title;
    int fd = -1; // the document (PDF); owned by the portal until the request ends
    PrintOptions remembered;
  };

  struct Handlers {
    std::function<void(std::uint64_t id, const PrintRequest& request)> open;
    std::function<void(std::uint64_t id)> closed; // the caller gave up: close the sheet
  };

  PrintPortal(SessionBus& bus, Handlers handlers, std::filesystem::path stateFile);
  ~PrintPortal();
  PrintPortal(const PrintPortal&) = delete;
  PrintPortal& operator=(const PrintPortal&) = delete;

  void cancel(std::uint64_t id);
  // Sends the job; `done` gets the reason when it fails. Remembers the options for the app and printer.
  void print(std::uint64_t id, const PrintOptions& options,
             std::function<void(std::optional<std::string> error)> done);
  [[nodiscard]] bool idle() const noexcept { return m_pending.empty(); }

private:
  struct Pending;
  void finish(std::uint64_t id, std::uint32_t response);

  SessionBus& m_bus;
  Handlers m_handlers;
  std::filesystem::path m_stateFile;
  std::unique_ptr<sdbus::IObject> m_object;
  std::map<std::uint64_t, std::unique_ptr<Pending>> m_pending;
  std::uint64_t m_nextId = 1;
  std::uint32_t m_nextToken = 1;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
