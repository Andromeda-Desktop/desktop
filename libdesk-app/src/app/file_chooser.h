#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class SessionBus;
namespace sdbus {
  class IProxy;
}

namespace desk {

  // Opens the FileChooser portal dialog.
  class FileChooser {
  public:
    explicit FileChooser(SessionBus& bus);
    ~FileChooser();
    FileChooser(const FileChooser&) = delete;
    FileChooser& operator=(const FileChooser&) = delete;

    // Asks for one folder. `picked` runs on the loop thread with the folder, or nullopt on cancel or error; a new
    // request makes an open one's answer stale, so only the newest `picked` runs. parentWindow: "wayland:<handle>" or "".
    void chooseFolder(const std::string& parentWindow, const std::string& title,
                      std::function<void(std::optional<std::filesystem::path>)> picked);

  private:
    SessionBus& m_bus;
    std::unique_ptr<sdbus::IProxy> m_portal;
    std::unique_ptr<sdbus::IProxy> m_request;
    std::uint64_t m_seq = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace desk
