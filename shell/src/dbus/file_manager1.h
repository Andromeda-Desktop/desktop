#pragma once

#include <filesystem>
#include <memory>
#include <string>

class SessionBus;

namespace sdbus {
  class IProxy;
}

// org.freedesktop.FileManager1: shows a file selected in its folder.
class FileManager1 {
public:
  explicit FileManager1(SessionBus* bus);
  ~FileManager1();

  FileManager1(const FileManager1&) = delete;
  FileManager1& operator=(const FileManager1&) = delete;

  // Without a bus or a file manager on it, opens the file's folder instead.
  void showItem(const std::filesystem::path& file, const std::string& activationToken = {});

private:
  SessionBus* m_bus;
  std::unique_ptr<sdbus::IProxy> m_proxy; // kept alive for pending ShowItems replies
};
