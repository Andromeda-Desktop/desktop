#include "core/freedesktop_thumbnails.h"

// Platforms without the freedesktop cache and Thumbnailer1: the pipeline makes every thumbnail itself.
namespace freedesktop_thumbnails {

  std::optional<std::filesystem::path> lookup(const std::filesystem::path& /*path*/, int /*targetPx*/,
                                              bool /*allowSmaller*/) {
    return std::nullopt;
  }

  bool knownFailure(const std::filesystem::path& /*path*/) { return false; }

  bool generate(const std::filesystem::path& /*path*/, int /*targetPx*/, std::chrono::milliseconds /*timeout*/) {
    return false;
  }

} // namespace freedesktop_thumbnails
