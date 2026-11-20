#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class SessionBus;
namespace sdbus {
  class IProxy;
  class Variant;
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
    // Asks for one file to open; same contract as chooseFolder.
    void openFile(const std::string& parentWindow, const std::string& title,
                  std::function<void(std::optional<std::filesystem::path>)> picked);
    // Asks for one or more files to open; `picked` gets an empty list on cancel or error, else as chooseFolder.
    void openFiles(const std::string& parentWindow, const std::string& title,
                   std::function<void(std::vector<std::filesystem::path>)> picked);
    // Asks where to save a new file, suggesting currentName; same contract as chooseFolder.
    void saveFile(const std::string& parentWindow, const std::string& title, const std::string& currentName,
                  std::function<void(std::optional<std::filesystem::path>)> picked);

  private:
    void request(const char* method, const std::string& parentWindow, const std::string& title,
                 std::map<std::string, sdbus::Variant> options,
                 std::function<void(std::vector<std::filesystem::path>)> picked);

    SessionBus& m_bus;
    std::unique_ptr<sdbus::IProxy> m_portal;
    std::unique_ptr<sdbus::IProxy> m_request;
    std::uint64_t m_seq = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace desk
