#pragma once

#include "launcher/launcher_provider.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class ConfigService;

class EmojiProvider : public LauncherProvider {
public:
  EmojiProvider(ClipboardService* clipboard, const ConfigService* config) : m_clipboard(clipboard), m_config(config) {}

  [[nodiscard]] std::string_view defaultPrefix() const override { return "emo"; }
  [[nodiscard]] std::string_view id() const override { return "Emoji"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "mood-smile-beam"; }
  [[nodiscard]] bool supportsGrid() const override { return true; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }
  [[nodiscard]] bool trackUsage() const override { return true; }

  void initialize() override;

  [[nodiscard]] std::vector<LauncherCategory> categories() const override;
  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  struct EmojiEntry {
    std::string emoji;
    std::string name;
    std::string nameLower;
    std::vector<std::string> keywords;
    std::string category;
    std::vector<std::string> tones;
    // 0 for emoji entries: only unicode.json entries match a codepoint query.
    std::uint32_t codepoint = 0;
  };

  void loadEntries(std::string_view assetName, bool unicode);

  [[nodiscard]] LauncherResult resultFor(const EmojiEntry& entry, double score) const;

  std::vector<EmojiEntry> m_entries;
  ClipboardService* m_clipboard = nullptr;
  const ConfigService* m_config = nullptr;
};
