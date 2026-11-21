#include "launcher/emoji_provider.h"

#include "core/files/resource_paths.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "util/string_utils.h"
#include "clipboard/clipboard_service.h"
#include "config/config_service.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace {
  constexpr Logger kLog("emoji");
  constexpr std::size_t kMaxResults = 100;

  // Resolve an emoji category id (as stored in emoji.json) to its localized label.
  std::string emojiCategoryLabel(std::string_view id) {
    return i18n::tr("launcher.categories.emoji." + std::string(id));
  }

  // "u+2014" (1-6 hex digits) or a bare "2014" (4-6 hex digits) in a lowercased query; 0 otherwise.
  std::uint32_t parseCodepointQuery(std::string_view query) {
    const bool prefixed = query.starts_with("u+");
    if (prefixed) {
      query.remove_prefix(2);
    }
    const std::size_t minDigits = prefixed ? 1 : 4;
    if (query.size() < minDigits || query.size() > 6) {
      return 0;
    }
    std::uint32_t codepoint = 0;
    const auto [ptr, ec] = std::from_chars(query.data(), query.data() + query.size(), codepoint, 16);
    if (ec != std::errc{} || ptr != query.data() + query.size()) {
      return 0;
    }
    return codepoint;
  }
} // namespace

void EmojiProvider::initialize() {
  // Emoji load first: on equal scores the stable sort in query() keeps them above Unicode entries.
  loadEntries("emoji.json", false);
  loadEntries("unicode.json", true);
}

void EmojiProvider::loadEntries(std::string_view assetName, bool unicode) {
  const std::filesystem::path path = paths::assetPath(assetName);
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }

  // A bundled asset: a damaged file is a packaging bug, logged; the provider then lists nothing.
  const auto json = nlohmann::json::parse(file, nullptr, false);
  if (json.is_discarded() || !json.is_array()) {
    kLog.warn("{} is not a JSON array", path.string());
    return;
  }
  const auto text = [](const nlohmann::json& item, const char* key) {
    const auto it = item.find(key);
    return it != item.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  {
    m_entries.reserve(m_entries.size() + json.size());
    for (const auto& item : json) {
      if (!item.is_object()) {
        continue;
      }
      EmojiEntry entry;
      entry.emoji = text(item, unicode ? "char" : "emoji");
      entry.name = text(item, "name");
      entry.nameLower = StringUtils::toLower(entry.name);
      entry.category = unicode ? "unicode" : text(item, "category");
      if (const auto it = item.find("codepoint"); unicode && it != item.end() && it->is_number_unsigned()) {
        entry.codepoint = it->get<std::uint32_t>();
      }

      if (item.contains("keywords") && item["keywords"].is_array()) {
        for (const auto& kw : item["keywords"]) {
          if (kw.is_string()) {
            entry.keywords.push_back(StringUtils::toLower(kw.get<std::string>()));
          }
        }
      }

      if (item.contains("tones") && item["tones"].is_array()) {
        for (const auto& tone : item["tones"]) {
          if (tone.is_string()) {
            entry.tones.push_back(tone.get<std::string>());
          }
        }
      }

      if (!entry.emoji.empty() && !entry.name.empty()) {
        m_entries.push_back(std::move(entry));
      }
    }
  }
}

std::string EmojiProvider::displayName() const { return i18n::tr("launcher.providers.emoji.title"); }

std::vector<LauncherCategory> EmojiProvider::categories() const {
  // Unicode emoji groups, in emoji-test.txt order.
  return {
      {emojiCategoryLabel("smileys"), "mood-smile"},
      {emojiCategoryLabel("people"), "hand-stop"},
      {emojiCategoryLabel("animals"), "paw"},
      {emojiCategoryLabel("food"), "apple"},
      {emojiCategoryLabel("travel"), "map"},
      {emojiCategoryLabel("activity"), "ball-football"},
      {emojiCategoryLabel("objects"), "bulb"},
      {emojiCategoryLabel("symbols"), "at"},
      {emojiCategoryLabel("flags"), "flag"},
      {emojiCategoryLabel("unicode"), "omega"},
  };
}

LauncherResult EmojiProvider::resultFor(const EmojiEntry& entry, double score) const {
  LauncherResult r;
  const auto tone =
      m_config == nullptr ? 0U : static_cast<std::size_t>(m_config->config().shell.launcher.emojiSkinTone);
  const std::string& emoji = tone > 0 && tone <= entry.tones.size() ? entry.tones[tone - 1] : entry.emoji;
  r.id = "emoji-" + emoji;
  r.title = entry.name;
  r.category = emojiCategoryLabel(entry.category);
  r.subtitle = entry.codepoint != 0 ? std::format("U+{:04X}", entry.codepoint) : r.category;
  r.badge = emoji;
  r.variants = entry.tones;
  r.score = score;
  return r;
}

std::vector<LauncherResult> EmojiProvider::query(std::string_view text) const {
  std::string query = StringUtils::toLower(text);
  if (query.empty()) {
    std::vector<LauncherResult> results;
    results.reserve(m_entries.size());
    for (const auto& e : m_entries) {
      results.push_back(resultFor(e, 0));
    }
    return results;
  }

  const std::uint32_t queryCodepoint = parseCodepointQuery(query);

  struct ScoredEntry {
    int score;
    std::size_t index;
  };

  std::vector<ScoredEntry> scored;

  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    const auto& e = m_entries[i];
    int bestScore = 0;

    // Exact name match
    if (e.nameLower == query) {
      bestScore = 1000;
    }
    // Name prefix
    else if (e.nameLower.size() >= query.size() && e.nameLower.starts_with(query)) {
      bestScore = 500;
    }
    // Name contains
    else if (e.nameLower.contains(query)) {
      bestScore = 200;
    }
    // Keyword match
    else {
      for (const auto& kw : e.keywords) {
        if (kw == query) {
          bestScore = std::max(bestScore, 150);
        } else if (kw.size() >= query.size() && kw.starts_with(query)) {
          bestScore = std::max(bestScore, 100);
        } else if (kw.contains(query)) {
          bestScore = std::max(bestScore, 50);
        }
      }
    }

    if (queryCodepoint != 0 && e.codepoint == queryCodepoint) {
      bestScore = 1000;
    }

    if (bestScore > 0) {
      scored.push_back({bestScore, i});
    }
  }

  std::ranges::stable_sort(scored, std::ranges::greater{}, &ScoredEntry::score);

  std::vector<LauncherResult> results;
  for (std::size_t i = 0; i < scored.size() && i < kMaxResults; ++i) {
    const auto& e = m_entries[scored[i].index];
    results.push_back(resultFor(e, scored[i].score));
  }

  return results;
}

bool EmojiProvider::activate(const LauncherResult& result) {
  if (!result.id.starts_with("emoji-")) {
    return false;
  }

  std::string emoji = result.id.substr(6);
  return m_clipboard != nullptr && m_clipboard->copyText(std::move(emoji));
}
