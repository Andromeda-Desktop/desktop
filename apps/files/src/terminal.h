#pragma once

#include <filesystem>
#include <string>

// Opens andromeda-terminal in `dir` (Open Terminal Here). Returns the command used, or "" when it is missing or failed to start.
std::string openTerminalIn(const std::filesystem::path& dir);
