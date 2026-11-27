#pragma once

#include <expected>
#include <functional>
#include <map>
#include <string>
#include <variant>

namespace desk::tokens {

  // A number fits f slots and i slots (integral only), a bool fits b, "#rrggbbaa" fits c, any string fits s.
  using SlotValue = std::variant<double, bool, std::string>;
  using SlotValues = std::map<std::string, SlotValue, std::less<>>;

  // Sets every slot of Tokens::slots(): its value from `values`, else its expression (a derived themable slot), else
  // its built-in value; then refreshes the colour entry tables. All-or-nothing: an unknown path or a wrong type
  // returns the reason and changes nothing. Main thread only.
  std::expected<void, std::string> applySlotValues(const SlotValues& values);

} // namespace desk::tokens
