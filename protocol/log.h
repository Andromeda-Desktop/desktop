#pragma once

#include <format>
#include <optional>
#include <string_view>
#include <utility>

enum class LogLevel { Debug, Info, Warn, Error };

namespace detail {
  void logMessage(LogLevel level, const char* section, std::string_view msg);
} // namespace detail

template <typename... Args> void logDebug(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Debug, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logInfo(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Info, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logWarn(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Warn, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logError(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Error, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

class Logger {
public:
  explicit constexpr Logger(const char* section) : m_section(section) {}

  template <typename... Args> void debug(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Debug, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args> void info(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Info, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args> void warn(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Warn, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args> void error(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Error, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

private:
  const char* m_section;
};

[[nodiscard]] std::string_view logLevelName(LogLevel level);
[[nodiscard]] std::optional<LogLevel> parseLogLevel(std::string_view value);
// The console (journal) threshold; the log file takes every level.
[[nodiscard]] LogLevel currentLogLevel();
void setLogLevel(LogLevel level);
// Reads ANDROMEDA_LOG_LEVEL: debug, info, warn or error.
void initLogLevelFromEnvironment();

// Redirect: fd 1/2 that point at a TTY (greetd wires them to /dev/tty1) go to andromeda-stderr.log in the same
// directory (or /dev/null), since synchronous VT writes on a main thread can turn a per-frame error into a livelock.
// Libraries that write to stderr themselves (glibc asserts, mesa, dbus) and spawned children inherit them.
enum class TtyStdio { Keep, Redirect };
// Opens (or rotates) $XDG_CACHE_HOME/andromeda/<name>, e.g. "shell/noctalia.log".
void initLogFile(std::string_view name, TtyStdio stdio = TtyStdio::Keep);
// Blocks until every queued line is written and the file flushed. Tests and shutdown only, never a frame path.
void flushLogs();
void setConsoleLogging(bool enabled);

// Replaces the console and log-file output when set (the greeter logs to syslog and its own file).
using LogSink = void (*)(LogLevel level, const char* section, std::string_view message);
void setLogSink(LogSink sink);

// Defined by each binary's platform layer (libdesk-ui core/sys, the compositor's core/log_wlr.cpp).
namespace log_platform {
  // Runs first on the writer thread: name it, give it the crash handler's stack.
  void writerThreadStarted();
  void breadcrumb(char level, const char* section, std::string_view message) noexcept;
} // namespace log_platform
