#pragma once

#include "files/file_ops_client.h"

#include <format>
#include <string>

inline std::string noun(std::size_t count) { return count == 1 ? "1 item" : std::format("{} items", count); }

// What a finished job did, for its toast or desktop notification: "Copied 3 items", or the failure.
inline std::string jobResultText(const FileOpsClient::Job& job) {
  if (job.state == "failed") {
    return job.error.empty() ? std::string("The operation failed") : job.error;
  }
  const std::string what = job.kind == "trash" ? "Moved to Trash"
      : job.kind == "copy"                    ? "Copied"
      : job.kind == "move"                    ? "Moved"
      : job.kind == "restore"                 ? "Restored"
      : job.kind == "undo"                    ? "Undone"
      : job.kind == "rename"                  ? "Renamed"
      : job.kind == "new_folder"              ? "Created"
      : job.kind == "link"                    ? "Linked"
      : job.kind == "extract"                 ? "Extracted"
      : job.kind == "compress"                ? "Compressed"
      : job.kind == "tags"                    ? "Tagged"
                                              : "Done";
  return std::format("{} {}", what, noun(job.doneFiles > 0 ? job.doneFiles : 1));
}
