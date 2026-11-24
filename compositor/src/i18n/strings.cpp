#include "i18n/strings.h"

#include "andromeda_data_dir.h"
#include "config/config.h"
#include "core/log.h"

#include <filesystem>
#include <format>

namespace strings {

  namespace {
    constexpr Logger kLog("i18n");

    struct Loaded {
      std::string language;
      i18n::Catalog active;
      i18n::Catalog fallback;
      bool ready = false;
    };

    void read(const std::filesystem::path& file, i18n::Catalog& out) {
      std::string error;
      if (i18n::readCatalogFile(file, out, "compositor", &error) == i18n::CatalogRead::Invalid) {
        kLog.warn("{}: {}", file.string(), error);
      }
    }

    const Loaded& loaded() {
      static Loaded s;
      const std::string& language = andromeda::config().appearance.language;
      if (s.ready && s.language == language) {
        return s;
      }
      const std::filesystem::path dir = i18n::catalogDir(
          andromeda::kTranslationsSourceDir,
          std::filesystem::path(andromeda::kDataDir) / "andromeda" / "translations" / "compositor"
      );
      s = Loaded{.language = language, .active = {}, .fallback = {}, .ready = true};
      read(dir / "en.json", s.fallback);
      if (!language.empty() && language != "en") {
        read(dir / (language + ".json"), s.active);
      }
      return s;
    }
  } // namespace

  std::string lookup(std::string_view key, std::span<const i18n::detail::Pair> args) {
    const Loaded& s = loaded();
    auto it = s.active.find(key);
    if (it == s.active.end()) {
      it = s.fallback.find(key);
      if (it == s.fallback.end()) {
        return std::format("!!{}!!", key);
      }
    }
    return args.empty() ? it->second : i18n::detail::interpolate(it->second, args);
  }

} // namespace strings
