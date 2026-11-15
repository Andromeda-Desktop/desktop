#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace term {

// The checked-out branch of the git work tree that holds `dir` (a 7-char commit when detached), or nullopt
// outside a work tree. Reads .git/HEAD only; runs no git process.
std::optional<std::string> gitBranch(const std::filesystem::path& dir);

} // namespace term
