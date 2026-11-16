#include "render/scene/terminal_bidi.h"

#include <fribidi.h>

#include <algorithm>
#include <numeric>

std::optional<TerminalBidiRow> terminalBidi(std::span<const TerminalCell> row) {
  const bool rtl = std::ranges::any_of(row, [](const TerminalCell& cell) {
    return cell.codepoint >= 0x590 && FRIBIDI_IS_RTL(fribidi_get_bidi_type(cell.codepoint));
  });
  if (!rtl) {
    return std::nullopt;
  }
  // One unit per drawn character: a wide cell's tail joins its head.
  std::vector<std::uint16_t> unitColumn;
  std::vector<FriBidiChar> text;
  for (std::size_t col = 0; col < row.size(); ++col) {
    if (col > 0 && (row[col - 1].flags & TerminalCell::Wide) != 0 && unitColumn.back() == col - 1) {
      continue;
    }
    unitColumn.push_back(static_cast<std::uint16_t>(col));
    text.push_back(row[col].codepoint == 0 ? U' ' : row[col].codepoint);
  }
  const auto len = static_cast<FriBidiStrIndex>(text.size());
  std::vector<FriBidiCharType> types(text.size());
  std::vector<FriBidiBracketType> brackets(text.size());
  std::vector<FriBidiLevel> levels(text.size());
  std::vector<FriBidiArabicProp> joining(text.size());
  fribidi_get_bidi_types(text.data(), len, types.data());
  fribidi_get_bracket_types(text.data(), len, types.data(), brackets.data());
  FriBidiParType base = FRIBIDI_PAR_LTR;
  if (fribidi_get_par_embedding_levels_ex(types.data(), brackets.data(), len, &base, levels.data()) == 0) {
    return std::nullopt;
  }
  fribidi_get_joining_types(text.data(), len, joining.data());
  fribidi_join_arabic(types.data(), len, levels.data(), joining.data());
  // No ligature flag: a lam-alef ligature would leave an empty cell behind.
  fribidi_shape(FRIBIDI_FLAG_SHAPE_MIRRORING | FRIBIDI_FLAG_SHAPE_ARAB_PRES, levels.data(), len, joining.data(), text.data());
  std::vector<FriBidiStrIndex> order(text.size());
  std::iota(order.begin(), order.end(), 0);
  if (fribidi_reorder_line(0, types.data(), len, 0, base, levels.data(), nullptr, order.data()) == 0) {
    return std::nullopt;
  }
  TerminalBidiRow out;
  out.cells.reserve(row.size());
  out.logical.reserve(row.size());
  for (const FriBidiStrIndex unit : order) {
    const std::uint16_t col = unitColumn[static_cast<std::size_t>(unit)];
    TerminalCell cell = row[col];
    if (cell.codepoint != 0) {
      cell.codepoint = text[static_cast<std::size_t>(unit)];
    }
    out.cells.push_back(cell);
    out.logical.push_back(col);
    if ((cell.flags & TerminalCell::Wide) != 0 && col + 1U < row.size()) {
      out.cells.push_back(row[col + 1U]);
      out.logical.push_back(static_cast<std::uint16_t>(col + 1U));
    }
  }
  return out;
}
