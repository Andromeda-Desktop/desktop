#include "theme/theme_package.h"

#include "protocols/toml.h" // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace noctalia::theme {

  namespace {

    int lineOf(const toml::node& node) { return static_cast<int>(node.source().begin.line); }

    bool validId(std::string_view id) {
      if (id.size() < 2 || id.size() > 40) {
        return false;
      }
      return std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
    }

    std::string joinKey(std::string_view prefix, std::string_view key) {
      return prefix.empty() ? std::string(key) : std::format("{}.{}", prefix, key);
    }

    std::optional<toml::table> parseToml(std::string_view text, ThemeIssue& error) {
      try {
        return toml::parse(text);
      } catch (const toml::parse_error& e) {
        error = {.key = "", .message = std::string(e.description()), .line = static_cast<int>(e.source().begin.line)};
        return std::nullopt;
      }
    }

    std::optional<VocabularyType> typeNamed(std::string_view name) {
      constexpr std::array<std::pair<std::string_view, VocabularyType>, 6> kTypes = {{
          {"color", VocabularyType::Color},
          {"length", VocabularyType::Length},
          {"number", VocabularyType::Number},
          {"font", VocabularyType::Font},
          {"spring", VocabularyType::Spring},
          {"enum", VocabularyType::Enum},
      }};
      const auto* it = std::ranges::find(kTypes, name, &std::pair<std::string_view, VocabularyType>::first);
      return it == kTypes.end() ? std::nullopt : std::optional(it->second);
    }

    std::vector<std::string> stringArray(const toml::node_view<const toml::node>& view) {
      std::vector<std::string> out;
      if (const auto* array = view.as_array()) {
        for (const auto& node : *array) {
          if (const auto text = node.value<std::string>()) {
            out.push_back(*text);
          }
        }
      }
      return out;
    }

    class ValueReader {
    public:
      ValueReader(ThemePackage& out, const ThemeVocabulary& vocabulary) : m_out(out), m_vocabulary(vocabulary) {}

      void readVocabularyKey(const toml::node& node, std::string name, const toml::table& siblings) {
        if (const auto alias = m_vocabulary.aliases.find(name); alias != m_vocabulary.aliases.end()) {
          if (siblings.contains(alias->second)) {
            issue(name, std::format("ignored: renamed to '{}', which is set too", alias->second), lineOf(node));
            return;
          }
          issue(name, std::format("renamed to '{}'", alias->second), lineOf(node));
          name = alias->second;
        }
        const auto entry = m_vocabulary.keys.find(name);
        if (entry == m_vocabulary.keys.end()) {
          issue(name, "not in the theme vocabulary, ignored", lineOf(node));
          return;
        }
        const VocabularyKey& key = entry->second;
        switch (key.type) {
        case VocabularyType::Color:
          readColor(node, name);
          return;
        case VocabularyType::Length:
        case VocabularyType::Number:
          readNumber(node, name, key);
          return;
        case VocabularyType::Font:
          if (const auto text = node.value<std::string>(); text && !text->empty()) {
            m_out.values[name] = *text;
          } else {
            issue(name, "a font is a non-empty family name", lineOf(node));
          }
          return;
        case VocabularyType::Enum:
          if (const auto text = node.value<std::string>(); text && std::ranges::contains(key.choices, *text)) {
            m_out.values[name] = *text;
          } else {
            issue(name, "not one of the allowed names", lineOf(node));
          }
          return;
        case VocabularyType::Spring:
          readSpring(node, name);
          return;
        }
      }

      void readColor(const toml::node& node, const std::string& key) {
        Color color;
        const auto text = node.value<std::string>();
        if (!text || !text->starts_with('#') || (text->size() != 7 && text->size() != 9) || !tryParseHexColor(*text, color)) {
          issue(key, "not a #rrggbb or #rrggbbaa colour", lineOf(node));
          return;
        }
        m_out.values[key] = color;
      }

      // Engine tables (motion, backdrop, assets): flattened; the engine checks kinds and parameters.
      void readFlat(const toml::node& node, const std::string& key) {
        if (const auto* table = node.as_table()) {
          for (const auto& [child, value] : *table) {
            readFlat(value, joinKey(key, child.str()));
          }
        } else if (const auto* array = node.as_array()) {
          for (std::size_t i = 0; i < array->size(); ++i) {
            readFlat(*array->get(i), std::format("{}.{}", key, i));
          }
        } else if (const auto* text = node.as_string()) {
          m_out.values[key] = text->get();
        } else if (const auto* integer = node.as_integer()) {
          m_out.values[key] = static_cast<double>(integer->get());
        } else if (const auto* real = node.as_floating_point()) {
          m_out.values[key] = real->get();
        } else if (const auto* flag = node.as_boolean()) {
          m_out.values[key] = flag->get();
        } else {
          issue(key, "unsupported value type", lineOf(node));
        }
      }

      void issue(const std::string& key, std::string message, int line) {
        m_out.issues.push_back({.key = key, .message = std::move(message), .line = line});
      }

    private:
      void readNumber(const toml::node& node, const std::string& name, const VocabularyKey& key) {
        if (const auto text = node.value<std::string>(); text && text->starts_with('=')) {
          m_out.values[name] = ThemeReference{.expression = text->substr(1)};
          return;
        }
        const auto number = node.value<double>();
        if (!number) {
          issue(name, "a number or a \"=\" reference", lineOf(node));
        } else if (*number < key.min || *number > key.max) {
          issue(name, std::format("{} is outside {}..{}", *number, key.min, key.max), lineOf(node));
        } else {
          m_out.values[name] = *number;
        }
      }

      void readSpring(const toml::node& node, const std::string& name) {
        const auto* table = node.as_table();
        const auto response = table ? (*table)["response"].value<double>() : std::nullopt;
        const auto bounce = table ? (*table)["bounce"].value<double>() : std::nullopt;
        if (!response || !bounce || table->size() != 2 || *response <= 0.0 || *bounce < 0.0 || *bounce >= 1.0) {
          issue(name, "a spring is { response > 0, 0 <= bounce < 1 }", lineOf(node));
          return;
        }
        m_out.values[name] = ThemeSpring{.response = *response, .bounce = *bounce};
      }

      ThemePackage& m_out;
      const ThemeVocabulary& m_vocabulary;
    };

    constexpr std::array<std::string_view, 3> kEngineTables = {"motion", "backdrop", "assets"};

  } // namespace

  std::expected<ThemeVocabulary, ThemeIssue> parseThemeVocabulary(std::string_view tomlText) {
    ThemeIssue error;
    const auto root = parseToml(tomlText, error);
    if (!root) {
      return std::unexpected(error);
    }
    ThemeVocabulary out;
    out.version = static_cast<int>((*root)["version"].value_or(std::int64_t{0}));
    if (out.version < 1) {
      return std::unexpected(ThemeIssue{.key = "version", .message = "version must be an integer >= 1", .line = 1});
    }
    out.roles = stringArray((*root)["roles"]);
    const auto* keys = (*root)["key"].as_table();
    if (keys == nullptr) {
      return std::unexpected(ThemeIssue{.key = "key", .message = "no [key.*] tables", .line = 1});
    }
    for (const auto& [name, node] : *keys) {
      const std::string keyName(name.str());
      const auto fail = [&](std::string message) {
        return std::unexpected(ThemeIssue{.key = keyName, .message = std::move(message), .line = lineOf(node)});
      };
      const auto* table = node.as_table();
      if (table == nullptr) {
        return fail("a key is a table");
      }
      const auto type = typeNamed((*table)["type"].value_or(std::string{}));
      if (!type) {
        return fail("unknown type");
      }
      VocabularyKey key{.type = *type, .tokens = stringArray((*table)["tokens"])};
      if (key.tokens.empty()) {
        return fail("a key sets at least one token");
      }
      if (*type == VocabularyType::Length || *type == VocabularyType::Number) {
        const auto* range = (*table)["range"].as_array();
        const auto min = range && range->size() == 2 ? range->get(0)->value<double>() : std::nullopt;
        const auto max = range && range->size() == 2 ? range->get(1)->value<double>() : std::nullopt;
        if (!min || !max || *min > *max) {
          return fail("a number key needs range = [min, max]");
        }
        key.min = *min;
        key.max = *max;
      }
      if (*type == VocabularyType::Enum) {
        key.choices = stringArray((*table)["values"]);
        if (key.choices.empty()) {
          return fail("an enum key needs values");
        }
      }
      for (const auto& alias : stringArray((*table)["aliases"])) {
        if (!out.aliases.emplace(alias, keyName).second) {
          return fail(std::format("alias '{}' is used twice", alias));
        }
      }
      out.keys.emplace(keyName, std::move(key));
    }
    for (const auto& [alias, target] : out.aliases) {
      if (out.keys.contains(alias)) {
        return std::unexpected(ThemeIssue{.key = alias, .message = "an alias equals a current key", .line = 1});
      }
    }
    return out;
  }

  std::expected<ThemePackage, ThemeIssue> parseThemePackage(std::string_view tomlText, std::string_view folderName,
                                                            const ThemeVocabulary& vocabulary) {
    ThemeIssue error;
    const auto parsed = parseToml(tomlText, error);
    if (!parsed) {
      return std::unexpected(error);
    }
    const toml::table& root = *parsed;
    const auto* meta = root["theme"].as_table();
    if (meta == nullptr) {
      return std::unexpected(ThemeIssue{.key = "theme", .message = "no [theme] table", .line = 1});
    }
    ThemePackage out;
    ThemeManifest& m = out.manifest;
    const auto fail = [&](std::string_view key, std::string message) {
      const toml::node* node = meta->get(key);
      return std::unexpected(ThemeIssue{
          .key = std::format("theme.{}", key), .message = std::move(message), .line = node ? lineOf(*node) : lineOf(*meta)});
    };

    const auto format = (*meta)["format"].value<std::int64_t>();
    if (!format || *format < 1) {
      return fail("format", "format must be an integer >= 1");
    }
    if (*format > kThemeFormat) {
      return fail("format", std::format("format {} is newer than this desktop reads ({})", *format, kThemeFormat));
    }
    m.format = static_cast<int>(*format);

    m.id = (*meta)["id"].value_or(std::string{});
    if (!validId(m.id)) {
      return fail("id", "id must be 2-40 characters of a-z, 0-9 and -");
    }
    if (m.id != folderName) {
      return fail("id", std::format("id '{}' differs from its folder '{}'", m.id, folderName));
    }
    m.name = (*meta)["name"].value_or(std::string{});
    if (m.name.empty()) {
      return fail("name", "name is required");
    }
    m.version = (*meta)["version"].value_or(std::string{});
    m.extends = (*meta)["extends"].value_or(m.id == "andromeda" ? std::string{} : std::string{"andromeda"});
    if (m.extends == m.id) {
      return fail("extends", "a theme cannot extend itself");
    }
    if (!m.extends.empty() && !validId(m.extends)) {
      return fail("extends", "extends must name a theme id");
    }

    ValueReader reader(out, vocabulary);
    if (const auto* schemes = (*meta)["schemes"].as_array()) {
      for (const auto& node : *schemes) {
        const auto scheme = node.value<std::string>();
        if (scheme && (*scheme == "dark" || *scheme == "light") && !std::ranges::contains(m.schemes, *scheme)) {
          m.schemes.push_back(*scheme);
        } else {
          reader.issue("theme.schemes", "a scheme is \"dark\" or \"light\", once", lineOf(node));
        }
      }
    }
    if (m.schemes.empty()) {
      m.schemes = {"dark", "light"};
    }

    if (const auto* contracts = root["contracts"].as_table()) {
      for (const auto& [name, node] : *contracts) {
        const auto major = node.value<std::int64_t>();
        if (major && *major >= 1) {
          m.contracts[std::string(name.str())] = static_cast<int>(*major);
        } else {
          reader.issue(joinKey("contracts", name.str()), "a contract version is an integer >= 1", lineOf(node));
        }
      }
    }

    if (const auto* values = root["values"].as_table()) {
      for (const auto& [key, node] : *values) {
        const std::string name(key.str());
        if (name != "dark" && name != "light") {
          reader.readVocabularyKey(node, name, *values);
          continue;
        }
        const auto* roles = node.as_table();
        if (roles == nullptr || !std::ranges::contains(m.schemes, name)) {
          reader.issue(joinKey("values", name), std::format("ignored: schemes does not list {}", name), lineOf(node));
          continue;
        }
        for (const auto& [role, value] : *roles) {
          const std::string path = std::format("scheme.{}.{}", name, role.str());
          if (std::ranges::contains(vocabulary.roles, role.str())) {
            reader.readColor(value, path);
          } else {
            reader.issue(path, "not a colour role, ignored", lineOf(value));
          }
        }
      }
    }
    for (const auto table : kEngineTables) {
      if (const auto* node = root.get(table)) {
        reader.readFlat(*node, std::string(table));
      }
    }

    for (const auto& [key, node] : root) {
      const std::string_view name = key.str();
      if (name != "theme" && name != "contracts" && name != "values" && !std::ranges::contains(kEngineTables, name)) {
        reader.issue(std::string(name), "unknown table, ignored", lineOf(node));
      }
    }
    return out;
  }

} // namespace noctalia::theme
