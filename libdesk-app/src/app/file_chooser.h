#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class SessionBus;

namespace desk {

  // Opens the system file dialog: the FileChooser portal on Linux (file_chooser.cpp), NSOpenPanel and NSSavePanel on
  // macOS (file_chooser_macos.mm; bus is unused).
  class FileChooser {
  public:
    explicit FileChooser(SessionBus* bus);
    ~FileChooser();
    FileChooser(const FileChooser&) = delete;
    FileChooser& operator=(const FileChooser&) = delete;

    // Asks for one folder. `picked` runs on the loop thread with the folder, or nullopt on cancel or error; a new
    // request makes an open one's answer stale, so only the newest `picked` runs. parentWindow: AppWindow::exportHandle()'s handle or "".
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

    struct Impl;

  private:
    std::unique_ptr<Impl> m_impl;
  };

} // namespace desk
