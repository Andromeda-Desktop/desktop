#include "context_row.h"

#include "design_tokens.h"
#include "render/scene/terminal_grid_node.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "time/time_format.h"
#include "ui/palette.h"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>

namespace term {

namespace {

  std::unique_ptr<Label> mono(const std::string& text, ColorRole ink, const ContextRow::Font& font) {
    return ui::label({.text = text, .fontSize = font.size, .fontFamily = font.family, .color = colorSpecFromRole(ink),
                      .maxLines = 1});
  }

  std::unique_ptr<Label> caption(const std::string& text, FontWeight weight, ColorRole ink, float s) {
    return ui::label({.text = text, .fontSize = Tokens::typeCaption.size * s, .fontWeight = weight,
                      .fontFeatures = std::string("tnum"), .color = colorSpecFromRole(ink), .maxLines = 1});
  }

  std::unique_ptr<Node> build(
      const ContextItem& item, float h, float width, float s, const ContextRow::Font& font, std::optional<Color> backing
  ) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .paddingH = Tokens::spaceP8 * s,
                        .fill = colorSpecFromRole(ColorRole::TermCommandBand), .radius = Tokens::radiusR8 * s,
                        .width = width, .height = h});
    // A space cell between path and branch, as the shell would print it.
    row->addChild(mono(item.branch.empty() ? item.cwd : item.cwd + " ", ColorRole::TermBlue, font));
    if (!item.branch.empty()) {
      row->addChild(mono(item.branch, ColorRole::Label2, font));
    }
    row->addChild(ui::box({.flexGrow = 1.0F}));
    switch (item.caption) {
    case ContextItem::Caption::None: break;
    case ContextItem::Caption::Done:
    case ContextItem::Caption::Running:
      row->addChild(caption(item.time, FontWeight::Medium, ColorRole::Label2, s));
      break;
    case ContextItem::Caption::Failed:
      row->addChild(caption(item.exit, FontWeight::SemiBold, ColorRole::Label1, s));
      row->addChild(caption("· " + item.time, FontWeight::Medium, ColorRole::Label1, s));
      break;
    }
    if (!backing) {
      return row;
    }
    // Over a scene the band backs its labels as the grid backs its text runs.
    auto back = ui::box({.fill = fixedColorSpec(*backing), .radius = Tokens::radiusR8 * s, .width = width, .height = h});
    back->addChild(std::move(row));
    return back;
  }

} // namespace

std::string formatTook(std::chrono::duration<double> took) {
  const double seconds = took.count();
  return seconds < 10.0 ? std::format("{:.1f} s", seconds)
      : seconds < 60.0  ? std::format("{} s", static_cast<int>(seconds))
                        : formatClockTime(static_cast<std::int64_t>(seconds));
}

ContextRow::ContextRow(TerminalGridNode& grid) : m_grid(grid) {
  auto layer = std::make_unique<Node>();
  layer->setZIndex(TerminalGridNode::kBelowTextZ);
  m_layer = grid.addChild(std::move(layer));
}

void ContextRow::setOrigin(float x, float y) { m_layer->setPosition(x, y); }

void ContextRow::sync(std::vector<Placed> placed, float cellHeight, float rowWidth, float scale, Font font) {
  const bool sameItems = placed.size() == m_placed.size()
      && std::equal(placed.begin(), placed.end(), m_placed.begin(),
                    [](const Placed& a, const Placed& b) { return a.item == b.item; });
  const bool sameSize = cellHeight == m_cellHeight && rowWidth == m_rowWidth && scale == m_scale && font == m_font;
  const float backing = m_grid.textBacking();
  if (!sameItems || !sameSize || backing != m_backing) {
    m_backing = backing;
    std::optional<Color> back;
    if (backing > 0.0F) {
      const std::uint32_t bg = m_grid.background();
      back = Color{static_cast<float>((bg >> 16) & 0xFF) / 255.0F, static_cast<float>((bg >> 8) & 0xFF) / 255.0F,
                   static_cast<float>(bg & 0xFF) / 255.0F, backing};
    }
    for (Node* node : m_nodes) {
      m_layer->removeChild(node);
    }
    m_nodes.clear();
    for (const Placed& p : placed) {
      m_nodes.push_back(m_layer->addChild(build(p.item, cellHeight, rowWidth, scale, font, back)));
    }
  }
  // The band padding sits left of column 0, so the path lines up with the text below.
  for (std::size_t i = 0; i < placed.size(); ++i) {
    m_nodes[i]->setPosition(-Tokens::spaceP8 * scale, static_cast<float>(placed[i].row) * cellHeight);
  }
  m_placed = std::move(placed);
  m_cellHeight = cellHeight;
  m_rowWidth = rowWidth;
  m_scale = scale;
  m_font = std::move(font);
}

std::optional<ContextRow::Hit> ContextRow::hitTest(float x, float y) const {
  const float lx = x - m_layer->x();
  const float ly = y - m_layer->y();
  for (std::size_t i = 0; i < m_nodes.size(); ++i) {
    const Node* row = m_nodes[i];
    if (ly < row->y() || ly >= row->y() + m_cellHeight) {
      continue;
    }
    const ContextItem& item = m_placed[i].item;
    // Children: the path, then the branch when there is one.
    const std::size_t pills = item.branch.empty() ? 1 : 2;
    for (std::size_t c = 0; c < pills && c < row->children().size(); ++c) {
      const Node* pill = row->children()[c].get();
      const float px = row->x() + pill->x();
      if (lx >= px && lx < px + pill->width()) {
        return c == 0 ? Hit{Hit::Pill::Cwd, item.path} : Hit{Hit::Pill::Branch, item.branch};
      }
    }
  }
  return std::nullopt;
}

} // namespace term
