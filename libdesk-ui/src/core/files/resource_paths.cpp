#include "core/files/resource_paths.h"

#include "core/log.h"
#include "core/sys/sys.h"
#include "protocols/i18n_catalog.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

namespace paths {

  namespace {

    constexpr Logger kLog("paths");

    std::filesystem::path installedAssetsRoot() {
      const std::filesystem::path datadir(NOCTALIA_INSTALL_DATADIR);
      if (datadir.is_absolute()) {
        return datadir / "noctalia" / "assets";
      }
      return std::filesystem::path(NOCTALIA_INSTALL_PREFIX) / datadir / "noctalia" / "assets";
    }

    bool isAssetRoot(const std::filesystem::path& root) {
      if (root.empty()) {
        return false;
      }

      std::error_code ec;
      return std::filesystem::exists(root / "emoji.json", ec)
          && std::filesystem::exists(root / "fonts" / "noctalia-tabler.ttf", ec)
          && std::filesystem::exists(root / "translations" / "en.json", ec);
    }

    void appendUnique(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& candidate) {
      if (candidate.empty()) {
        return;
      }
      if (!std::ranges::contains(candidates, candidate)) {
        candidates.push_back(candidate);
      }
    }

    std::vector<std::filesystem::path> assetCandidates() {
      std::vector<std::filesystem::path> candidates;

      if (const char* env = (std::getenv("ANDROMEDA_ASSETS_DIR") ? std::getenv("ANDROMEDA_ASSETS_DIR") : std::getenv("NOCTALIA_ASSETS_DIR")); env != nullptr && env[0] != '\0') {
        const std::filesystem::path overridePath(env);
        if (isAssetRoot(overridePath)) {
          candidates.push_back(overridePath);
          return candidates;
        }
        kLog.warn("ANDROMEDA_ASSETS_DIR is not a valid asset bundle: {}", overridePath.string());
      }

      if (const std::filesystem::path exe = sys::executablePath(); !exe.empty()) {
        const std::filesystem::path exeDir = exe.parent_path();
        appendUnique(candidates, exeDir / "assets");
        appendUnique(candidates, exeDir.parent_path() / "Resources" / "assets");
        appendUnique(candidates, exeDir.parent_path() / "assets");

        const std::filesystem::path datadir(NOCTALIA_INSTALL_DATADIR);
        if (!datadir.empty() && !datadir.is_absolute()) {
          appendUnique(candidates, exeDir.parent_path() / datadir / "noctalia" / "assets");
        }
        appendUnique(candidates, exeDir.parent_path() / "share" / "noctalia" / "assets");
      }

      appendUnique(candidates, installedAssetsRoot());
#ifdef NOCTALIA_SOURCE_ASSETS_DIR
      // Uninstalled builds of other apps on the toolkit (the greeter) use the shell's source-tree bundle.
      appendUnique(candidates, std::filesystem::path(NOCTALIA_SOURCE_ASSETS_DIR));
#endif
      return candidates;
    }

    std::filesystem::path resolveAssetsRoot() {
      for (const auto& candidate : assetCandidates()) {
        if (isAssetRoot(candidate)) {
          kLog.debug("using assets from {}", candidate.string());
          return candidate;
        }
      }

      const std::filesystem::path fallback = installedAssetsRoot();
      kLog.warn("could not locate a valid asset bundle; defaulting to {}", fallback.string());
      return fallback;
    }

  } // namespace

  const std::filesystem::path& assetsRoot() {
    static const std::filesystem::path s_root = resolveAssetsRoot();
    return s_root;
  }

  std::filesystem::path assetPath(std::string_view relativePath) {
    return assetsRoot() / std::filesystem::path(std::string(relativePath));
  }

  std::filesystem::path translationsDir(std::string_view domain, const std::filesystem::path& sourceDir) {
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::path("translations") / std::string(domain);
    const std::filesystem::path datadir(NOCTALIA_INSTALL_DATADIR);
    const std::filesystem::path installed =
        (datadir.is_absolute() ? datadir : std::filesystem::path(NOCTALIA_INSTALL_PREFIX) / datadir) / "andromeda" / relative;
    // An app built on the static libdesk (Link's window) keeps its catalogs beside the executable, as it keeps assets.
    if (const std::filesystem::path exe = sys::executablePath(); !exe.empty() && !std::filesystem::exists(installed, ec)) {
      for (const auto& bundled : {exe.parent_path() / relative, exe.parent_path().parent_path() / "Resources" / relative}) {
        if (std::filesystem::exists(bundled, ec)) {
          return i18n::catalogDir(sourceDir, bundled);
        }
      }
    }
    return i18n::catalogDir(sourceDir, installed);
  }

} // namespace paths
