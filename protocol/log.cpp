#include "log.h"
#include "os.h"
#include "xdg_dirs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace {

  LogLevel gMinLevel = LogLevel::Info;
  FILE* gLogFile = nullptr;
  // gLogMutex: the level, console switch and repeat state (callers). gFileMutex: the log file (writer thread, init).
  // Callers never take gFileMutex, so a writer blocked in I/O cannot block them.
  std::mutex gLogMutex;
  std::mutex gFileMutex;
  std::atomic<bool> gFileEnabled{false};
  std::atomic<LogSink> gLogSink{nullptr};
  std::string gLogPath;
  std::string gBackupLogPath;
  std::uintmax_t gLogSizeBytes = 0;
  std::size_t gBufferedFileLogLines = 0;
  std::chrono::steady_clock::time_point gLastFileFlushAt = std::chrono::steady_clock::now();
  bool gRegisteredExitFlush = false;
  bool gConsoleEnabled = true;

  constexpr std::size_t kMaxLogBytes = 1 * 1024 * 1024; // 1 MB
  constexpr std::size_t kMaxLogLineBytes = 8 * 1024;    // 8 KiB
  constexpr std::size_t kBufferedFileLogFlushLines = 64;
  constexpr auto kBufferedFileLogFlushInterval = std::chrono::milliseconds(500);
  constexpr const char* kLogLevelEnv = "ANDROMEDA_LOG_LEVEL";

  // A run of one message collapses into a count, reported when the run ends; one copy passes per window, so a
  // persistent fault stays visible. A per-frame failure can log thousands of lines a second.
  constexpr auto kRepeatWindow = std::chrono::seconds(1);
  std::string gLastMessage;
  const char* gLastSection = nullptr;
  LogLevel gLastLevel = LogLevel::Debug;
  std::size_t gSuppressedRepeats = 0;
  std::chrono::steady_clock::time_point gRepeatWindowStart{};

  // Every write runs on one writer thread: a write to the journal or the log file can block for seconds under disk
  // writeback throttling or a busy journald, and on the main thread that froze the UI. Callers only queue lines;
  // past this budget they drop them and the writer reports the count.
  constexpr std::size_t kMaxQueuedBytes = 4 * 1024 * 1024;

  struct QueuedLine {
    std::string console;
    std::string file;
    bool flush = false;
  };

  std::mutex gQueueMutex;
  std::condition_variable gQueueReady;
  std::condition_variable gQueueDrained;
  bool gWriterBusy = false;
  std::deque<QueuedLine> gQueue;
  std::size_t gQueuedBytes = 0;
  std::size_t gDroppedLines = 0;
  std::size_t gSkippedConsoleLines = 0;
  bool gWriterStop = false;
  bool gWriterRunning = false;
  // Set by the exit join: later lines (static destructors, other threads) are written in place.
  bool gWriterShutDown = false;
  // A forked child has no writer thread; it writes in place.
  bool gInForkedChild = false;
  std::thread gWriter;

  struct CappedLogMessage {
    std::string storage;
    std::string_view original;
    bool capped = false;

    [[nodiscard]] std::string_view text() const noexcept { return capped ? std::string_view(storage) : original; }
  };

  const char* levelTagAnsi(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
      return "\033[36mDBG\033[0m";
    case LogLevel::Info:
      return "\033[32mINF\033[0m";
    case LogLevel::Warn:
      return "\033[33mWRN\033[0m";
    case LogLevel::Error:
      return "\033[31mERR\033[0m";
    }
    return "???";
  }

  const char* levelTagPlain(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
      return "DBG";
    case LogLevel::Info:
      return "INF";
    case LogLevel::Warn:
      return "WRN";
    case LogLevel::Error:
      return "ERR";
    }
    return "???";
  }

  std::size_t utf8PrefixBoundary(std::string_view text, std::size_t maxBytes) {
    if (maxBytes >= text.size()) {
      return text.size();
    }

    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
      --end;
    }
    return end;
  }

  std::string truncationSuffix(std::size_t originalBytes) {
    return std::string(" ... [truncated, original=") + std::to_string(originalBytes) + " bytes]";
  }

  CappedLogMessage capMessageForLine(std::string_view msg, std::size_t prefixBytes) {
    CappedLogMessage result;
    result.original = msg;

    if (prefixBytes + 1 >= kMaxLogLineBytes) {
      result.capped = true;
      return result;
    }

    const std::size_t maxMessageBytes = kMaxLogLineBytes - prefixBytes - 1;
    if (msg.size() <= maxMessageBytes) {
      return result;
    }

    result.capped = true;

    std::string suffix = truncationSuffix(msg.size());
    if (suffix.size() > maxMessageBytes) {
      constexpr std::string_view kShortSuffix = " ... [truncated]";
      result.storage = std::string(kShortSuffix.substr(0, std::min(kShortSuffix.size(), maxMessageBytes)));
      return result;
    }

    const std::size_t bodyBytes = utf8PrefixBoundary(msg, maxMessageBytes - suffix.size());
    result.storage.reserve(bodyBytes + suffix.size());
    result.storage.append(msg.substr(0, bodyBytes));
    result.storage.append(suffix);
    return result;
  }

  std::size_t formattedPrefixLength(int length, std::size_t bufferSize) {
    if (length <= 0 || bufferSize == 0) {
      return 0;
    }
    return std::min(static_cast<std::size_t>(length), bufferSize - 1);
  }

  // The sd-daemon level prefix when stderr is the journal stream ($JOURNAL_STREAM is its "dev:inode"), so journald
  // stores the line's real priority instead of 6.
  const char* journalLevelPrefix(LogLevel level) {
    static const bool toJournal = andromeda::os::stderrIsJournal();
    if (!toJournal) {
      return "";
    }
    switch (level) {
    case LogLevel::Debug:
      return "<7>";
    case LogLevel::Info:
      return "<6>";
    case LogLevel::Warn:
      return "<4>";
    case LogLevel::Error:
      return "<3>";
    }
    return "";
  }

  std::string consolePrefix(const std::tm& tm, const std::timespec& ts, LogLevel level, const char* section) {
    char buffer[96];
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%02d:%02d:%02d.%03ld [%s]", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1'000'000,
        levelTagAnsi(level)
    );

    std::string prefix(buffer, formattedPrefixLength(length, sizeof(buffer)));
    if (section != nullptr && section[0] != '\0') {
      prefix += " [\033[34m";
      prefix += section;
      prefix += "\033[0m]";
    }
    prefix += ' ';
    return prefix;
  }

  std::string filePrefix(const std::tm& tm, const std::timespec& ts, LogLevel level, const char* section) {
    char buffer[128];
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s]", tm.tm_year + 1900, tm.tm_mon + 1,
        tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1'000'000, levelTagPlain(level)
    );

    std::string prefix(buffer, formattedPrefixLength(length, sizeof(buffer)));
    if (section != nullptr && section[0] != '\0') {
      prefix += " [";
      prefix += section;
      prefix += ']';
    }
    prefix += ' ';
    return prefix;
  }

  void writeRaw(FILE* stream, const std::string& line) {
    if (stream != nullptr && !line.empty()) {
      std::fwrite(line.data(), 1, line.size(), stream);
    }
  }

  void flushLogFileUnlocked() {
    if (gLogFile == nullptr) {
      return;
    }
    std::fflush(gLogFile);
    gBufferedFileLogLines = 0;
    gLastFileFlushAt = std::chrono::steady_clock::now();
  }

  void closeLogFileUnlocked() {
    if (gLogFile == nullptr) {
      return;
    }

    std::fflush(gLogFile);
    std::fclose(gLogFile);
    gLogFile = nullptr;
  }

  std::uintmax_t currentLogFileSizeUnlocked() {
    if (gLogPath.empty()) {
      return 0;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(gLogPath, ec);
    return ec ? 0 : size;
  }

  void openLogFileUnlocked() {
    if (gLogPath.empty()) {
      return;
    }

    gLogFile = andromeda::os::openAppend(gLogPath);
    gLogSizeBytes = gLogFile == nullptr ? 0 : currentLogFileSizeUnlocked();
    gBufferedFileLogLines = 0;
    gLastFileFlushAt = std::chrono::steady_clock::now();
  }

  void rotateLogFileUnlocked() {
    closeLogFileUnlocked();

    if (gLogPath.empty() || gBackupLogPath.empty()) {
      return;
    }

    std::error_code ec;
    std::filesystem::remove(gBackupLogPath, ec);
    ec.clear();
    std::filesystem::rename(gLogPath, gBackupLogPath, ec);
    openLogFileUnlocked();
  }

  // The console is the journal stream or a pipe: when it is full, skip the line rather than wait, so the log file
  // (written first) keeps going. A child shares stderr's file description, so it cannot be set O_NONBLOCK.
  void writeConsole(const std::string& line) {
    if (line.empty()) {
      return;
    }
    if (!andromeda::os::stderrWritable()) {
      ++gSkippedConsoleLines;
      return;
    }
    if (gSkippedConsoleLines > 0) {
      const std::string note = std::format("[log] {} console lines skipped: stderr was full\n", gSkippedConsoleLines);
      gSkippedConsoleLines = 0;
      andromeda::os::writeStderr(note);
    }
    andromeda::os::writeStderr(line);
  }

  // Runs on the writer thread, or in place in a forked child.
  void writeQueuedLine(const QueuedLine& line) {
    if (!line.file.empty()) {
      std::scoped_lock lock(gFileMutex);
      if (gLogFile != nullptr) {
        if (gLogSizeBytes > 0 && gLogSizeBytes + line.file.size() > kMaxLogBytes) {
          rotateLogFileUnlocked();
        }
        writeRaw(gLogFile, line.file);
        gLogSizeBytes += line.file.size();
        if (line.flush) {
          flushLogFileUnlocked();
        } else {
          ++gBufferedFileLogLines;
          if (gBufferedFileLogLines >= kBufferedFileLogFlushLines
              || std::chrono::steady_clock::now() - gLastFileFlushAt >= kBufferedFileLogFlushInterval) {
            flushLogFileUnlocked();
          }
        }
      }
    }
    writeConsole(line.console);
  }

  void writerMain() {
    log_platform::writerThreadStarted();
    std::unique_lock lock(gQueueMutex);
    while (true) {
      gQueueReady.wait(lock, [] { return gWriterStop || !gQueue.empty(); });
      if (gQueue.empty()) {
        return;
      }
      QueuedLine line = std::move(gQueue.front());
      gQueue.pop_front();
      gQueuedBytes -= line.console.size() + line.file.size();
      const std::size_t dropped = std::exchange(gDroppedLines, 0);
      gWriterBusy = true;
      lock.unlock();
      if (dropped > 0) {
        const std::string note = std::format("[log] {} lines dropped: the log writer fell behind\n", dropped);
        writeQueuedLine({.console = note, .file = note, .flush = true});
      }
      writeQueuedLine(line);
      lock.lock();
      gWriterBusy = false;
      if (gQueue.empty()) {
        gQueueDrained.notify_all();
      }
    }
  }

  void stopWriter() {
    {
      std::scoped_lock lock(gQueueMutex);
      if (!gWriterRunning) {
        return;
      }
      gWriterStop = true;
    }
    gQueueReady.notify_one();
    gWriter.join();
    std::scoped_lock lock(gQueueMutex);
    gWriterRunning = false;
    gWriterStop = false;
    gWriterShutDown = true;
  }

  void queueLine(QueuedLine line) {
    if (gInForkedChild) {
      writeQueuedLine(line);
      return;
    }
    std::unique_lock lock(gQueueMutex);
    if (gWriterShutDown) {
      lock.unlock();
      writeQueuedLine(line);
      return;
    }
    if (!gWriterRunning) {
      // The first line can come before main blocks SIGINT/SIGTERM for its signalfd; a writer that inherits them
      // unblocked would take the signal's default action and kill the process.
      gWriter = andromeda::os::startThreadSignalsBlocked(writerMain);
      gWriterRunning = true;
      // Joins before static destruction: a joinable std::thread destructor terminates the process.
      static const bool registered = std::atexit(stopWriter) == 0;
      (void)registered;
    }
    const std::size_t bytes = line.console.size() + line.file.size();
    if (gQueuedBytes + bytes > kMaxQueuedBytes) {
      ++gDroppedLines;
      return;
    }
    gQueuedBytes += bytes;
    gQueue.push_back(std::move(line));
    gQueueReady.notify_one();
  }

  void flushLogFileAtExit() {
    stopWriter();
    std::scoped_lock lock(gFileMutex);
    flushLogFileUnlocked();
  }

  // gFileMutex is not taken: the writer holds it during disk I/O, and fork() must not wait for that. The child may
  // inherit it locked, so it never touches the file: it logs to the console only.
  void prepareFork() {
    gLogMutex.lock();
    gQueueMutex.lock();
  }

  void afterForkParent() {
    gQueueMutex.unlock();
    gLogMutex.unlock();
  }

  void afterForkChild() {
    gQueueMutex.unlock();
    gLogMutex.unlock();
    gInForkedChild = true;
    gFileEnabled.store(false, std::memory_order_relaxed);
  }

  [[maybe_unused]] const bool gForkHandlers = (andromeda::os::atFork(prepareFork, afterForkParent, afterForkChild), true);

  void emit(LogLevel level, const char* section, std::string_view msg, const std::tm& tm, const std::timespec& ts,
            LogLevel minLevel, bool console) {
    log_platform::breadcrumb("DIWE"[static_cast<int>(level)], section, msg);
    if (LogSink sink = gLogSink.load(std::memory_order_acquire); sink != nullptr) {
      if (level >= minLevel) {
        sink(level, section, msg);
      }
      return;
    }
    QueuedLine line;
    // Console: respects the level, ANSI colours, time only
    if (console && level >= minLevel) {
      const std::string prefix = consolePrefix(tm, ts, level, section);
      const CappedLogMessage capped = capMessageForLine(msg, prefix.size());
      line.console = journalLevelPrefix(level);
      line.console += prefix;
      line.console += capped.text();
      line.console += '\n';
    }
    // File: every level, no ANSI, full date
    if (gFileEnabled.load(std::memory_order_acquire)) {
      const std::string prefix = filePrefix(tm, ts, level, section);
      const CappedLogMessage capped = capMessageForLine(msg, prefix.size());
      line.file = prefix;
      line.file += capped.text();
      line.file += '\n';
      line.flush = level >= LogLevel::Warn;
    }
    if (!line.console.empty() || !line.file.empty()) {
      queueLine(std::move(line));
    }
  }

} // namespace

std::string_view logLevelName(LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "debug";
  case LogLevel::Info:
    return "info";
  case LogLevel::Warn:
    return "warn";
  case LogLevel::Error:
    return "error";
  }
  return "unknown";
}

std::optional<LogLevel> parseLogLevel(std::string_view value) {
  if (value == "debug") {
    return LogLevel::Debug;
  }
  if (value == "info") {
    return LogLevel::Info;
  }
  if (value == "warn") {
    return LogLevel::Warn;
  }
  if (value == "error") {
    return LogLevel::Error;
  }
  return std::nullopt;
}

void initLogFile(std::string_view name, TtyStdio stdio) {
  const std::string cacheHome = xdg::cacheHome();
  std::filesystem::path logPath;
  if (!cacheHome.empty()) {
    logPath = std::filesystem::path(cacheHome) / "andromeda" / name;
  }
  std::error_code ec;
  if (!logPath.empty()) {
    std::filesystem::create_directories(logPath.parent_path(), ec);
  }
  const bool haveDir = !logPath.empty() && !ec;
  if (stdio == TtyStdio::Redirect) {
    andromeda::os::redirectStdioIfTty(haveDir ? logPath.parent_path() : std::filesystem::path());
  }
  if (!haveDir) {
    return;
  }
  const std::string backupPath = logPath.string() + ".1";

  std::scoped_lock lock(gFileMutex);
  closeLogFileUnlocked();
  gLogPath = logPath.string();
  gBackupLogPath = backupPath;

  const auto size = std::filesystem::file_size(gLogPath, ec);
  if (!ec && size > kMaxLogBytes) {
    rotateLogFileUnlocked();
  } else {
    openLogFileUnlocked();
  }

  if (gLogFile != nullptr && !gRegisteredExitFlush) {
    (void)std::atexit(flushLogFileAtExit);
    gRegisteredExitFlush = true;
  }
  gFileEnabled.store(gLogFile != nullptr, std::memory_order_release);
}

void flushLogs() {
  {
    std::unique_lock lock(gQueueMutex);
    gQueueDrained.wait(lock, [] { return !gWriterRunning || (gQueue.empty() && !gWriterBusy); });
  }
  std::scoped_lock lock(gFileMutex);
  flushLogFileUnlocked();
}

namespace detail {

  void logMessage(LogLevel level, const char* section, std::string_view msg) {
    std::timespec ts{};
    std::timespec_get(&ts, TIME_UTC);
    const std::tm tm = andromeda::os::localTime(ts.tv_sec);

    struct Repeats {
      LogLevel level;
      const char* section;
      std::size_t count;
    };
    std::optional<Repeats> repeats;
    LogLevel minLevel = LogLevel::Info;
    bool console = true;
    {
      std::scoped_lock lock(gLogMutex);
      minLevel = gMinLevel;
      console = gConsoleEnabled;
      const auto now = std::chrono::steady_clock::now();
      const bool same = msg == gLastMessage && section == gLastSection && level == gLastLevel;
      if (same && now - gRepeatWindowStart < kRepeatWindow) {
        ++gSuppressedRepeats;
        return;
      }
      if (gSuppressedRepeats > 0) {
        repeats = Repeats{gLastLevel, gLastSection, std::exchange(gSuppressedRepeats, 0)};
      }
      if (!same) {
        gLastMessage.assign(msg);
        gLastSection = section;
        gLastLevel = level;
      }
      gRepeatWindowStart = now;
    }
    if (repeats) {
      emit(repeats->level, repeats->section, std::format("last message repeated {} times", repeats->count), tm, ts,
           minLevel, console);
    }
    emit(level, section, msg, tm, ts, minLevel, console);
  }

} // namespace detail

void setLogSink(LogSink sink) { gLogSink.store(sink, std::memory_order_release); }

void setConsoleLogging(bool enabled) {
  std::scoped_lock lock(gLogMutex);
  gConsoleEnabled = enabled;
}

LogLevel currentLogLevel() {
  std::scoped_lock lock(gLogMutex);
  return gMinLevel;
}

void setLogLevel(LogLevel level) {
  std::scoped_lock lock(gLogMutex);
  gMinLevel = level;
}

void initLogLevelFromEnvironment() {
  const char* value = std::getenv(kLogLevelEnv);
  if (value == nullptr) {
    return;
  }

  const auto level = parseLogLevel(value);
  if (!level.has_value()) {
    logWarn("invalid {} '{}'; expected debug, info, warn, or error", kLogLevelEnv, value);
    return;
  }

  setLogLevel(*level);
  logInfo("log level set to {}", logLevelName(*level));
}
