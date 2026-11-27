#include "theme/dev_tokens.h"

#include "core/log.h"
#include "design_tokens.h"
#include "theme/builtin_palettes.h"
#include "ui/token_slots.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <string_view>
#include "protocols/toml.h"
#include <variant>

#ifdef TOKENS_RUNTIME
namespace noctalia::theme::dev_tokens {

  namespace {

    constexpr Logger kLog("dev-tokens");

    using Value = std::variant<double, std::int64_t, bool, std::string>;

    const Value* find(const std::map<std::string, Value>& raw, const std::string& path) {
      const auto it = raw.find(path);
      return it == raw.end() ? nullptr : &it->second;
    }

    void flatten(const toml::table& table, const std::string& prefix, std::map<std::string, Value>& out) {
      for (const auto& [key, node] : table) {
        const std::string path = prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
        if (const auto* sub = node.as_table()) {
          flatten(*sub, path, out);
        } else if (const auto* f = node.as_floating_point()) {
          out[path] = f->get();
        } else if (const auto* i = node.as_integer()) {
          out[path] = i->get();
        } else if (const auto* b = node.as_boolean()) {
          out[path] = b->get();
        } else if (const auto* s = node.as_string()) {
          out[path] = s->get();
        } else {
          out[path] = std::string("?unsupported");
        }
      }
    }

    // "= a.b - c.d + e.f": the same expressions gen-design-tokens.py resolves at build time.
    std::optional<double> evaluate(const std::map<std::string, Value>& raw, std::string_view expr, int depth) {
      if (depth > 16) {
        return std::nullopt;
      }
      expr.remove_prefix(1);
      double total = 0.0;
      double sign = 1.0;
      std::size_t pos = 0;
      bool first = true;
      while (pos <= expr.size()) {
        while (pos < expr.size() && expr[pos] == ' ') {
          ++pos;
        }
        const auto end = expr.find_first_of("+- ", pos);
        const std::string ref(expr.substr(pos, end == std::string_view::npos ? end : end - pos));
        if (ref.empty()) {
          return std::nullopt;
        }
        const Value* v = find(raw, ref);
        if (v == nullptr) {
          return std::nullopt;
        }
        double n = 0.0;
        if (const auto* d = std::get_if<double>(v)) {
          n = *d;
        } else if (const auto* i = std::get_if<std::int64_t>(v)) {
          n = static_cast<double>(*i);
        } else if (const auto* s = std::get_if<std::string>(v); s != nullptr && s->starts_with("=")) {
          const auto inner = evaluate(raw, *s, depth + 1);
          if (!inner) {
            return std::nullopt;
          }
          n = *inner;
        } else {
          return std::nullopt;
        }
        total += sign * n;
        first = false;
        pos = end == std::string_view::npos ? expr.size() + 1 : end;
        while (pos < expr.size() && expr[pos] == ' ') {
          ++pos;
        }
        if (pos >= expr.size()) {
          break;
        }
        if (expr[pos] != '+' && expr[pos] != '-') {
          return std::nullopt;
        }
        sign = expr[pos] == '+' ? 1.0 : -1.0;
        ++pos;
      }
      return first ? std::nullopt : std::optional<double>(total);
    }

  } // namespace

  // Runs on the main loop, which is the only thread that reads Tokens:: except the template worker; that worker
  // reads schemeLightEntries/schemeDarkEntries only after a theme change this reload triggers, not concurrently.
  bool applyFile(const std::filesystem::path& path, std::string& error) {
    toml::table root;
    try {
      root = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
      error = std::string(e.description());
      return false;
    }
    std::map<std::string, Value> raw;
    flatten(root, "", raw);

    desk::tokens::SlotValues values;
    for (const auto& [key, value] : raw) {
      if (const auto* s = std::get_if<std::string>(&value); s != nullptr && s->starts_with("=")) {
        const auto n = evaluate(raw, *s, 0);
        if (!n) {
          error = key + ": bad or circular expression";
          return false;
        }
        values[key] = *n;
      } else if (const auto* i = std::get_if<std::int64_t>(&value)) {
        values[key] = static_cast<double>(*i);
      } else if (const auto* d = std::get_if<double>(&value)) {
        values[key] = *d;
      } else if (const auto* b = std::get_if<bool>(&value)) {
        values[key] = *b;
      } else {
        values[key] = std::get<std::string>(value);
      }
    }
    if (auto applied = desk::tokens::applySlotValues(values); !applied) {
      error = applied.error();
      return false;
    }
    refreshBuiltinPalettes();
    return true;
  }

  std::filesystem::path envPath() {
    const char* v = std::getenv("ANDROMEDA_DEV_TOKENS");
    return v != nullptr ? std::filesystem::path(v) : std::filesystem::path();
  }

  bool applyFromEnv() {
    const auto path = envPath();
    if (path.empty()) {
      return false;
    }
    std::string error;
    if (applyFile(path, error)) {
      kLog.info("loaded design tokens from {}", path.string());
    } else {
      kLog.warn("{}: {}; using built-in tokens", path.string(), error);
    }
    return true;
  }

  void watchEnv(FileWatcher& watcher, std::function<void()> onApplied) {
    auto path = envPath();
    if (path.empty()) {
      return;
    }
    watcher.watch(
        path,
        [path, onApplied = std::move(onApplied)] {
          std::string error;
          if (applyFile(path, error)) {
            kLog.info("reloaded design tokens from {}", path.string());
            onApplied();
          } else {
            kLog.warn("{}: {}; keeping the last good tokens", path.string(), error);
          }
        },
        FileWatcher::WatchTrigger::WriteCompleted
    );
  }

} // namespace noctalia::theme::dev_tokens

#else

namespace noctalia::theme::dev_tokens {

  bool applyFile(const std::filesystem::path&, std::string& error) {
    error = "built without dev_tokens";
    return false;
  }

  std::filesystem::path envPath() { return {}; }
  bool applyFromEnv() { return false; }
  void watchEnv(FileWatcher&, std::function<void()>) {}

} // namespace noctalia::theme::dev_tokens

#endif
