#pragma once

#include "files/file_ops_client.h"
#include "i18n/i18n.h"

#include <format>
#include <string>

inline std::string noun(std::size_t count) { return i18n::trp("files.job.items", static_cast<long>(count)); }

// What a finished job did, for its toast or desktop notification: "Copied 3 items", or the failure.
inline std::string jobResultText(const FileOpsClient::Job& job) {
  if (job.state == "failed") {
    return job.error.empty() ? i18n::tr("files.job.operation-failed") : job.error;
  }
  const char* key = job.kind == "trash" ? "files.job.done-trash"
      : job.kind == "copy"              ? "files.job.done-copy"
      : job.kind == "move"              ? "files.job.done-move"
      : job.kind == "restore"           ? "files.job.done-restore"
      : job.kind == "undo"              ? "files.job.done-undo"
      : job.kind == "rename"            ? "files.job.done-rename"
      : job.kind == "new_folder"        ? "files.job.done-new-folder"
      : job.kind == "link"              ? "files.job.done-link"
      : job.kind == "extract"           ? "files.job.done-extract"
      : job.kind == "compress"          ? "files.job.done-compress"
      : job.kind == "tags"              ? "files.job.done-tags"
                                        : "files.job.done";
  return i18n::tr(key, "items", noun(job.doneFiles > 0 ? job.doneFiles : 1));
}
