#pragma once

#include "admin_process.h"
#include "ui/files/file_browser.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The administrator view of one Files window: folders listed by the root helper, shown as a FileBrowser listing
// with a red bar, and changes sent to the same helper. One helper (one password) serves the whole window.
class AdminView {
public:
  struct Host {
    std::function<void(FileBrowser::Listing)> show;              // show this listing in the current tab
    std::function<void(const std::string& text, bool ok)> toast; // a result or an error
    std::function<void(const std::filesystem::path& dir)> leave; // the bar's Close: the folder as the user
  };
  explicit AdminView(Host host);

  [[nodiscard]] static bool isAdminListing(const std::string& listingId) { return listingId.starts_with(kPrefix); }
  [[nodiscard]] const std::filesystem::path& dir() const noexcept { return m_dir; }

  void open(const std::filesystem::path& dir);
  void refresh() { open(m_dir); }
  void newFolder(const std::string& name);
  void rename(const std::filesystem::path& path, const std::string& name);
  void remove(const std::vector<std::filesystem::path>& paths);
  void transfer(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& dest, bool move);
  void chmod(const std::vector<std::filesystem::path>& paths, std::uint32_t mode, std::uint32_t mask, bool recursive);
  void chown(const std::vector<std::filesystem::path>& paths, std::optional<std::uint32_t> uid,
             std::optional<std::uint32_t> gid, bool recursive);

private:
  static constexpr const char* kPrefix = "admin:";
  void start();
  void change(nlohmann::json op, std::string done);
  void show();
  [[nodiscard]] std::unique_ptr<Node> banner(float scale);
  [[nodiscard]] std::unique_ptr<Node> statusPage(float scale);

  Host m_host;
  std::unique_ptr<AdminProcess> m_process;
  std::filesystem::path m_dir;
  std::vector<std::shared_ptr<const FileInfo>> m_rows;
  std::string m_error; // the last listing error ("Permission denied"); empty after a good listing
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
