#pragma once

#include "protocols/async_file_writer.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct AtomicWriteTarget {
  std::filesystem::path path;
  bool throughSymlink = false;
};

[[nodiscard]] std::optional<AtomicWriteTarget> resolveAtomicWriteTarget(const std::filesystem::path& path);
// Durable fsyncs the file and its directory, so a crash keeps the old or the new content; it blocks on disk, so
// only worker-thread callers use it.
enum class AtomicWrite : bool { Fast, Durable };

[[nodiscard]] bool writeTextFileAtomic(
    const std::filesystem::path& path, std::string_view content,
    std::optional<std::filesystem::perms> mode = std::nullopt, AtomicWrite durability = AtomicWrite::Fast
);

// writeTextFileAtomic on the shared background writer (protocols/async_file_writer.h): the caller never waits on the
// disk and pendingFileText(path) shows the content until it lands. `done` gets the result on the main loop.
void writeTextFileAtomicAsync(
    std::filesystem::path path, std::string content, std::optional<std::filesystem::perms> mode = std::nullopt,
    andromeda::WriteDone done = {}
);
