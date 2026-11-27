#include "ui/token_slots.h"

#include "design_tokens.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace desk::tokens {

  namespace {

    std::optional<std::uint32_t> parseColor(std::string_view text) {
      if (text.size() != 9 || text[0] != '#') {
        return std::nullopt;
      }
      std::uint32_t value = 0;
      const auto* end = text.data() + text.size();
      const auto [ptr, ec] = std::from_chars(text.data() + 1, end, value, 16);
      return ec == std::errc{} && ptr == end ? std::optional(value) : std::nullopt;
    }

    bool fits(const Tokens::Slot& slot, const SlotValue& value) {
      const auto* number = std::get_if<double>(&value);
      const auto* text = std::get_if<std::string>(&value);
      switch (slot.kind) {
      case 'f':
        return number != nullptr && std::isfinite(*number);
      case 'i':
        return number != nullptr && std::trunc(*number) == *number && *number >= std::numeric_limits<int>::min() &&
               *number <= std::numeric_limits<int>::max();
      case 'b':
        return std::holds_alternative<bool>(value);
      case 'c':
        return text != nullptr && parseColor(*text).has_value();
      case 's':
        return text != nullptr;
      default:
        return false;
      }
    }

    void writeNumber(const Tokens::Slot& slot, double number) {
      switch (slot.kind) {
      case 'f':
        *static_cast<float*>(slot.ptr) = static_cast<float>(number);
        break;
      case 'i':
        *static_cast<int*>(slot.ptr) = static_cast<int>(number);
        break;
      case 'b':
        *static_cast<bool*>(slot.ptr) = number != 0.0;
        break;
      case 'c':
        *static_cast<std::uint32_t*>(slot.ptr) = static_cast<std::uint32_t>(number);
        break;
      default:
        break;
      }
    }

    double readNumber(const Tokens::Slot& slot) {
      return slot.kind == 'i' ? *static_cast<const int*>(slot.ptr) : *static_cast<const float*>(slot.ptr);
    }

    // "+a.b -4.0": each term reads an earlier slot or is a number (the generator orders and bakes them).
    double evaluate(std::string_view expr, const std::unordered_map<std::string_view, const Tokens::Slot*>& byPath) {
      double total = 0.0;
      while (!expr.empty()) {
        const auto end = expr.find(' ');
        const std::string_view term = expr.substr(0, end);
        expr = end == std::string_view::npos ? std::string_view{} : expr.substr(end + 1);
        const double sign = term[0] == '-' ? -1.0 : 1.0;
        const std::string_view operand = term.substr(1);
        double value = 0.0;
        if (const auto [ptr, ec] = std::from_chars(operand.data(), operand.data() + operand.size(), value);
            ec != std::errc{} || ptr != operand.data() + operand.size()) {
          const auto it = byPath.find(operand);
          assert(it != byPath.end());
          value = readNumber(*it->second);
        }
        total += sign * value;
      }
      return total;
    }

  } // namespace

  std::expected<void, std::string> applySlotValues(const SlotValues& values) {
    std::unordered_map<std::string_view, const Tokens::Slot*> byPath;
    for (const auto& slot : Tokens::slots()) {
      byPath.emplace(slot.path, &slot);
    }
    for (const auto& [path, value] : values) {
      const auto it = byPath.find(path);
      if (it == byPath.end()) {
        return std::unexpected("unknown token " + path);
      }
      if (!fits(*it->second, value)) {
        return std::unexpected(path + ": wrong value type");
      }
    }

    static std::unordered_map<std::string_view, std::string> strings; // s slots point into these
    // A derived slot whose sources all keep their built-in values keeps its built-in value exactly (float rounding).
    std::unordered_set<std::string_view> changed;
    const auto readsChanged = [&](std::string_view expr) {
      for (const auto term : std::views::split(expr, ' ')) {
        if (changed.contains(std::string_view(term).substr(1))) {
          return true;
        }
      }
      return false;
    };
    for (const auto& slot : Tokens::slots()) {
      const auto given = values.find(slot.path);
      if (given == values.end()) {
        if (slot.kind == 's') {
          *static_cast<std::string_view*>(slot.ptr) = slot.text;
        } else if (!slot.expr.empty() && readsChanged(slot.expr)) {
          writeNumber(slot, evaluate(slot.expr, byPath));
          changed.insert(slot.path);
        } else {
          writeNumber(slot, slot.number);
        }
        continue;
      }
      changed.insert(slot.path);
      const SlotValue& value = given->second;
      if (slot.kind == 's') {
        *static_cast<std::string_view*>(slot.ptr) = strings[slot.path] = std::get<std::string>(value);
      } else if (slot.kind == 'c') {
        writeNumber(slot, *parseColor(std::get<std::string>(value)));
      } else if (const auto* flag = std::get_if<bool>(&value)) {
        writeNumber(slot, *flag ? 1.0 : 0.0);
      } else {
        writeNumber(slot, std::get<double>(value));
      }
    }
    Tokens::refreshEntries();
    return {};
  }

} // namespace desk::tokens
