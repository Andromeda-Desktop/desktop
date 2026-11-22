#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class SessionBus;

namespace term {

  // File operations for browse mode: the Files job engine (org.andromeda.FileOps1) and FileManager1 on Linux
  // (file_actions_dbus.cpp), NSWorkspace and NSFileManager on macOS (file_actions_macos.mm; bus is unused).
  // done gets an empty error once the operation started, else why it could not.
  class FileActions {
  public:
    using Paths = std::vector<std::filesystem::path>;
    using Done = std::function<void(const std::string& error)>;

    explicit FileActions(SessionBus* bus);
    ~FileActions();
    FileActions(const FileActions&) = delete;
    FileActions& operator=(const FileActions&) = delete;

    void copy(const Paths& paths, const std::filesystem::path& dest, Done done);
    void move(const Paths& paths, const std::filesystem::path& dest, Done done);
    void trash(const Paths& paths, Done done);
    void rename(const std::filesystem::path& path, const std::string& name, Done done);
    void renameMany(const std::vector<std::pair<std::filesystem::path, std::string>>& items, Done done);
    void showInFiles(const Paths& paths);

    struct Impl;

  private:
    std::unique_ptr<Impl> m_impl;
  };

} // namespace term
