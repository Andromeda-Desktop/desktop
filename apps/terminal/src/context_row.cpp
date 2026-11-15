#include "context_row.h"

#include "design_tokens.h"
#include "render/scene/terminal_grid_node.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"

#include <algorithm>
#include <memory>

namespace term {

namespace {

  std::unique_ptr<Node> pill(const char* glyph, const std::string& text, ColorRole fill, ColorRole ink, float h,
                             float s) {
    auto pill = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .paddingH = Tokens::spaceP8 * s,
                         .fill = colorSpecFromRole(fill), .radius = h * 0.5F, .height = h});
    pill->addChild(ui::glyph({.glyph = std::string(glyph), .glyphSize = Tokens::spaceP12 * s,
                              .color = colorSpecFromRole(ink)}));
    pill->addChild(ui::label({.text = text, .fontSize = Tokens::typeFootnote.size * s,
                              .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ink), .maxLines = 1}));
    return pill;
  }

  std::unique_ptr<Label> caption(const std::string& text, FontWeight weight, ColorRole ink, float s) {
    return ui::label({.text = text, .fontSize = Tokens::typeCaption.size * s, .fontWeight = weight,
                      .fontFeatures = std::string("tnum"), .color = colorSpecFromRole(ink), .maxLines = 1});
  }

  std::unique_ptr<Node> build(const ContextItem& item, float h, float width, float s) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .width = width, .height = h});
    row->addChild(pill("folder", item.cwd, ColorRole::TermPillCwd, ColorRole::TermBlue, h, s));
    if (!item.branch.empty()) {
      row->addChild(pill("git-branch", item.branch, ColorRole::Fill2, ColorRole::Label2, h, s));
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
    return row;
  }

} // namespace

ContextRow::ContextRow(TerminalGridNode& grid) {
  auto layer = std::make_unique<Node>();
  layer->setZIndex(TerminalGridNode::kBelowTextZ);
  m_layer = grid.addChild(std::move(layer));
}

void ContextRow::setOrigin(float x, float y) { m_layer->setPosition(x, y); }

void ContextRow::sync(std::vector<Placed> placed, float cellHeight, float rowWidth, float scale) {
  const bool sameItems = placed.size() == m_placed.size()
      && std::equal(placed.begin(), placed.end(), m_placed.begin(),
                    [](const Placed& a, const Placed& b) { return a.item == b.item; });
  const bool sameSize = cellHeight == m_cellHeight && rowWidth == m_rowWidth && scale == m_scale;
  if (!sameItems || !sameSize) {
    for (Node* node : m_nodes) {
      m_layer->removeChild(node);
    }
    m_nodes.clear();
    for (const Placed& p : placed) {
      m_nodes.push_back(m_layer->addChild(build(p.item, cellHeight, rowWidth, scale)));
    }
  }
  // The pill padding sits left of column 0, so the glyph lines up with the text below.
  for (std::size_t i = 0; i < placed.size(); ++i) {
    m_nodes[i]->setPosition(-Tokens::spaceP8 * scale, static_cast<float>(placed[i].row) * cellHeight);
  }
  m_placed = std::move(placed);
  m_cellHeight = cellHeight;
  m_rowWidth = rowWidth;
  m_scale = scale;
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
    // Children: the cwd pill, then the branch pill when there is a branch.
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
