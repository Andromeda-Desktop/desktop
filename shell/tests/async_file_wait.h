#pragma once

#include "protocols/async_file_writer.h"

#include <chrono>
#include <filesystem>
#include <thread>

// Waits until no write to `path` is queued or running on the background file writer; false on timeout.
inline bool waitFileWritten(const std::filesystem::path& path, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (andromeda::pendingFileText(path)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}
