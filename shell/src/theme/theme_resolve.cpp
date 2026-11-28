#include "theme/theme_resolve.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numbers>
#include <set>
#include <utility>

namespace noctalia::theme {

  namespace {

    constexpr std::string_view kOverridesSource = "overrides";

    struct Layer {
      std::string source;
      const ThemePackage* package = nullptr;
    };

    struct Candidate {
      const ThemeValue* value = nullptr;
      const std::string* source = nullptr;
    };

    // "backdrop.desktop.scene" -> "backdrop.desktop"; empty for a key outside the engine tables.
    std::string_view engineEntry(std::string_view key) {
      const auto first = key.find('.');
      if (first == std::string_view::npos || !std::ranges::contains(kEngineTables, key.substr(0, first))) {
        return {};
      }
      return key.substr(0, key.find('.', first + 1));
    }

    struct Expression {
      std::string key;
      char op = 0; // 0, '+', '-' or '*'
      double operand = 0.0;
    };

    std::string_view trim(std::string_view text) {
      while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
      }
      while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
      }
      return text;
    }

    // "<key>" or "<key> <+|-|*> <number>".
    std::optional<Expression> parseExpression(std::string_view text) {
      text = trim(text);
      const auto keyEnd = std::ranges::find_if(text, [](char c) {
        return !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_');
      });
      Expression out{.key = std::string(text.begin(), keyEnd)};
      if (out.key.empty()) {
        return std::nullopt;
      }
      std::string_view rest = trim(text.substr(out.key.size()));
      if (rest.empty()) {
        return out;
      }
      if (rest.front() != '+' && rest.front() != '-' && rest.front() != '*') {
        return std::nullopt;
      }
      out.op = rest.front();
      rest = trim(rest.substr(1));
      const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), out.operand);
      if (ec != std::errc{} || ptr != rest.data() + rest.size() || !std::isfinite(out.operand)) {
        return std::nullopt;
      }
      return out;
    }

    class Resolver {
    public:
      Resolver(const std::vector<Layer>& topFirst, const ThemeVocabulary& vocabulary, const BuiltInNumber& builtIn,
               ResolvedTheme& out)
          : m_vocabulary(vocabulary), m_builtIn(builtIn), m_out(out) {
        std::set<std::string, std::less<>> claimed;
        for (const Layer& layer : topFirst) {
          std::set<std::string, std::less<>> entries;
          for (const auto& [key, value] : layer.package->values) {
            const std::string_view entry = engineEntry(key);
            if (!entry.empty()) {
              if (claimed.contains(entry)) {
                continue;
              }
              entries.emplace(entry);
            }
            m_stack[key].push_back({.value = &value, .source = &layer.source});
          }
          claimed.merge(entries);
        }
      }

      void run() {
        for (const auto& [key, candidates] : m_stack) {
          resolve(key);
        }
      }

    private:
      // The key's value, from the highest layer whose value is valid; nullopt when no layer gives one.
      const ThemeValue* resolve(const std::string& key) {
        if (const auto it = m_out.values.find(key); it != m_out.values.end()) {
          return &it->second;
        }
        if (m_failed.contains(key) || m_visiting.contains(key)) {
          return nullptr;
        }
        const auto stack = m_stack.find(key);
        if (stack == m_stack.end()) {
          return nullptr;
        }
        m_visiting.insert(key);
        const ThemeValue* result = nullptr;
        for (const Candidate& candidate : stack->second) {
          const auto* reference = std::get_if<ThemeReference>(candidate.value);
          if (reference == nullptr) {
            result = &m_out.values.emplace(key, *candidate.value).first->second;
          } else if (const auto number = evaluate(key, *reference, *candidate.source)) {
            result = &m_out.values.emplace(key, *number).first->second;
          } else {
            continue;
          }
          m_out.origin.emplace(key, *candidate.source);
          break;
        }
        m_visiting.erase(key);
        if (result == nullptr) {
          m_failed.insert(key);
        }
        return result;
      }

      std::optional<double> evaluate(const std::string& key, const ThemeReference& reference, const std::string& source) {
        const auto fail = [&](std::string why) -> std::optional<double> {
          m_out.issues.push_back({.key = key,
                                  .message = std::format("'={}': {}; the value below it applies", reference.expression,
                                                         std::move(why)),
                                  .line = reference.line,
                                  .source = source});
          return std::nullopt;
        };
        auto expression = parseExpression(reference.expression);
        if (!expression) {
          return fail("not \"=key\" or \"=key <+|-|*> number\"");
        }
        if (const auto alias = m_vocabulary.aliases.find(expression->key); alias != m_vocabulary.aliases.end()) {
          expression->key = alias->second;
        }
        const auto target = m_vocabulary.keys.find(expression->key);
        if (target == m_vocabulary.keys.end()
            || (target->second.type != VocabularyType::Length && target->second.type != VocabularyType::Number)) {
          return fail(std::format("'{}' is not a number key", expression->key));
        }
        if (m_visiting.contains(expression->key)) {
          return fail("the references form a cycle");
        }
        std::optional<double> base;
        if (m_stack.contains(expression->key)) {
          if (const ThemeValue* value = resolve(expression->key)) {
            base = std::get<double>(*value);
          }
        }
        if (!base) {
          base = m_builtIn ? m_builtIn(expression->key) : std::nullopt;
        }
        if (!base) {
          return fail(std::format("'{}' has no value", expression->key));
        }
        double result = *base;
        switch (expression->op) {
        case '+':
          result += expression->operand;
          break;
        case '-':
          result -= expression->operand;
          break;
        case '*':
          result *= expression->operand;
          break;
        default:
          break;
        }
        const VocabularyKey& own = m_vocabulary.keys.at(key);
        if (result < own.min || result > own.max) {
          return fail(std::format("{} is outside {}..{}", result, own.min, own.max));
        }
        return result;
      }

      const ThemeVocabulary& m_vocabulary;
      const BuiltInNumber& m_builtIn;
      ResolvedTheme& m_out;
      std::map<std::string, std::vector<Candidate>> m_stack; // per key, the highest layer first
      std::set<std::string, std::less<>> m_visiting;
      std::set<std::string, std::less<>> m_failed;
    };

  } // namespace

  std::expected<ResolvedTheme, ThemeIssue> resolveTheme(std::string_view id, const ThemeLookup& lookup,
                                                        const ThemePackage* overrides, const ThemeVocabulary& vocabulary,
                                                        const BuiltInNumber& builtIn) {
    std::vector<ThemePackage> packages;
    ResolvedTheme out;
    auto leaf = lookup(id);
    if (!leaf) {
      leaf.error().source = std::string(id);
      return std::unexpected(std::move(leaf.error()));
    }
    out.schemes = leaf->manifest.schemes;
    packages.push_back(std::move(*leaf));
    out.chain.emplace_back(id);
    for (std::string parent = packages.back().manifest.extends; !parent.empty();
         parent = packages.back().manifest.extends) {
      const auto stop = [&](std::string message) {
        out.issues.push_back({.key = "theme.extends", .message = std::move(message), .source = out.chain.back()});
      };
      if (std::ranges::contains(out.chain, parent)) {
        stop(std::format("'{}' is already in the extends chain; the chain ends here", parent));
        break;
      }
      if (out.chain.size() > static_cast<std::size_t>(kMaxExtendsDepth)) {
        stop(std::format("the extends chain is deeper than {}; '{}' and above are ignored", kMaxExtendsDepth, parent));
        break;
      }
      auto ancestor = lookup(parent);
      if (!ancestor) {
        stop(std::format("'{}' cannot load ({}); the chain ends here", parent, ancestor.error().message));
        break;
      }
      packages.push_back(std::move(*ancestor));
      out.chain.push_back(std::move(parent));
    }

    std::vector<Layer> topFirst;
    if (overrides != nullptr) {
      topFirst.push_back({.source = std::string(kOverridesSource), .package = overrides});
    }
    for (std::size_t i = 0; i < packages.size(); ++i) {
      topFirst.push_back({.source = out.chain[i], .package = &packages[i]});
    }
    for (const Layer& layer : topFirst) {
      for (ThemeIssue issue : layer.package->issues) {
        issue.source = layer.source;
        out.issues.push_back(std::move(issue));
      }
    }
    Resolver(topFirst, vocabulary, builtIn, out).run();
    return out;
  }

  desk::tokens::SlotValues slotValues(const ResolvedTheme& theme, const ThemeVocabulary& vocabulary) {
    desk::tokens::SlotValues out;
    for (const auto& [key, value] : theme.values) {
      if (key.starts_with("scheme.")) {
        if (const auto* color = std::get_if<Color>(&value)) {
          out.emplace(key, formatRgbaHex(*color));
        }
        continue;
      }
      const auto entry = vocabulary.keys.find(key);
      if (entry == vocabulary.keys.end()) {
        continue;
      }
      for (const std::string& token : entry->second.tokens) {
        if (const auto* spring = std::get_if<ThemeSpring>(&value)) {
          const double omega = 2.0 * std::numbers::pi / spring->response;
          out.emplace(token + ".stiffness", omega * omega);
          out.emplace(token + ".damping", 1.0 - spring->bounce);
        } else if (const auto* color = std::get_if<Color>(&value)) {
          out.emplace(token, formatRgbaHex(*color));
        } else if (const auto* number = std::get_if<double>(&value)) {
          out.emplace(token, *number);
        } else if (const auto* text = std::get_if<std::string>(&value)) {
          out.emplace(token, *text);
        }
      }
    }
    return out;
  }

} // namespace noctalia::theme
