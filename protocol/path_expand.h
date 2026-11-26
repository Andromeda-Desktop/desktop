#pragma once

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

// Path text in config files: $VAR / ${VAR} and a leading ~ expand the same way in every module.
namespace paths {

  // $NAME and ${NAME} become the variable's value (unset = empty); a lone '$' or an unclosed "${" stays verbatim.
  // Never wordexp() or a shell: config text must not run command substitution.
  [[nodiscard]] inline std::string expandEnvironment(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto isNameStart = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    const auto isNameChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    for (std::size_t i = 0; i < in.size();) {
      if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
        if (const std::size_t close = in.find('}', i + 2); close != std::string_view::npos) {
          if (const char* value = std::getenv(std::string(in.substr(i + 2, close - i - 2)).c_str())) {
            out.append(value);
          }
          i = close + 1;
          continue;
        }
      } else if (in[i] == '$' && i + 1 < in.size() && isNameStart(in[i + 1])) {
        std::size_t end = i + 2;
        while (end < in.size() && isNameChar(in[end])) {
          ++end;
        }
        if (const char* value = std::getenv(std::string(in.substr(i + 1, end - i - 1)).c_str())) {
          out.append(value);
        }
        i = end;
        continue;
      }
      out.push_back(in[i++]);
    }
    return out;
  }

  // "~" and "~/rest" start at $HOME; anything else, or an unset HOME, stays as written.
  [[nodiscard]] inline std::filesystem::path expandHome(const std::string& path) {
    const char* home = std::getenv("HOME");
    if (path.empty() || path[0] != '~' || home == nullptr || home[0] == '\0') {
      return path;
    }
    if (path.size() == 1) {
      return home;
    }
    return path[1] == '/' ? std::filesystem::path(home) / path.substr(2) : std::filesystem::path(path);
  }

  // One key per file for include cycle and duplicate checks: symlinks resolved where the path exists.
  [[nodiscard]] inline std::filesystem::path canonicalKey(const std::filesystem::path& path) {
    std::error_code error;
    auto key = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : key;
  }

} // namespace paths
