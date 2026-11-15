#include "git_branch.h"

#include <fstream>
#include <string_view>
#include <system_error>

namespace term {

namespace {

std::string firstLine(const std::filesystem::path& file) {
  std::ifstream in(file);
  std::string line;
  std::getline(in, line);
  return line;
}

} // namespace

std::optional<std::string> gitBranch(const std::filesystem::path& dir) {
  std::error_code ec;
  for (std::filesystem::path at = dir; !at.empty(); at = at.parent_path()) {
    const std::filesystem::path dotGit = at / ".git";
    std::filesystem::path gitDir;
    if (std::filesystem::is_directory(dotGit, ec)) {
      gitDir = dotGit;
    } else if (std::filesystem::is_regular_file(dotGit, ec)) {
      // A worktree or submodule: ".git" names the real git dir.
      const std::string line = firstLine(dotGit);
      constexpr std::string_view kPrefix = "gitdir: ";
      if (!line.starts_with(kPrefix)) return std::nullopt;
      gitDir = std::filesystem::path(line.substr(kPrefix.size()));
      if (gitDir.is_relative()) gitDir = at / gitDir;
    }
    if (!gitDir.empty()) {
      const std::string head = firstLine(gitDir / "HEAD");
      constexpr std::string_view kRef = "ref: refs/heads/";
      if (head.starts_with(kRef)) return head.substr(kRef.size());
      if (head.size() >= 7) return head.substr(0, 7);
      return std::nullopt;
    }
    if (at == at.parent_path()) break;
  }
  return std::nullopt;
}

} // namespace term
