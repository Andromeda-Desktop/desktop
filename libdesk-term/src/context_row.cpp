#include "context_row.h"

#include "design_tokens.h"
#include "render/scene/terminal_grid_node.h"
#include "ui/builders.h"
#include "core/files/resource_paths.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "time/time_format.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>

namespace term {

namespace {

  std::unique_ptr<Label> mono(const std::string& text, const Color& ink, const ContextRow::Font& font) {
    return ui::label({.text = text, .fontSize = font.size, .fontFamily = font.family, .color = fixedColorSpec(ink),
                      .maxLines = 1});
  }

  std::unique_ptr<Label> caption(const std::string& text, FontWeight weight, const Color& ink, float s) {
    return ui::label({.text = text, .fontSize = Tokens::typeCaption.size * s, .fontWeight = weight,
                      .fontFeatures = std::string("tnum"), .color = fixedColorSpec(ink), .maxLines = 1});
  }

  std::unique_ptr<Node> build(
      const ContextItem& item, float h, float width, float s, const ContextRow::Font& font,
      const ContextRow::Inks& inks, std::optional<Color> backing
  ) {
    auto pill = [&] {
      return ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .paddingH = Tokens::spaceP8 * s,
                      .fill = colorSpecFromRole(ColorRole::TermCommandBand), .radius = Style::scaledRadius(Tokens::radiusR8, s),
                      .height = h});
    };
    // Over a scene each pill backs its labels as the grid backs its text runs.
    auto backed = [&](std::unique_ptr<Node> node) -> std::unique_ptr<Node> {
      if (!backing) {
        return node;
      }
      auto back = ui::row({.fill = fixedColorSpec(*backing), .radius = Style::scaledRadius(Tokens::radiusR8, s), .height = h});
      back->addChild(std::move(node));
      return back;
    };
    auto band = pill();
    // A space cell between path and branch, as the shell would print it.
    band->addChild(mono(item.branch.empty() ? item.cwd : item.cwd + " ", inks.path, font));
    if (!item.branch.empty()) {
      band->addChild(mono(item.branch, inks.label2, font));
    }
    auto row = ui::row({.align = FlexAlign::Center, .width = width, .height = h});
    row->addChild(backed(std::move(band)));
    if (item.caption == ContextItem::Caption::None) {
      return row;
    }
    row->addChild(ui::box({.flexGrow = 1.0F}));
    auto tail = pill();
    if (item.caption == ContextItem::Caption::Failed) {
      tail->addChild(caption(item.exit, FontWeight::SemiBold, inks.label1, s));
      tail->addChild(caption("· " + item.time, FontWeight::Medium, inks.label1, s));
    } else {
      tail->addChild(caption(item.time, FontWeight::Medium, inks.label2, s));
    }
    row->addChild(backed(std::move(tail)));
    return row;
  }

  Color rgb(std::uint32_t c) {
    return Color{static_cast<float>((c >> 16) & 0xFF) / 255.0F, static_cast<float>((c >> 8) & 0xFF) / 255.0F,
                 static_cast<float>(c & 0xFF) / 255.0F, 1.0F};
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

void ContextRow::sync(std::vector<Placed> placed, float cellHeight, float rowWidth, float scale, Font font,
                      Inks inks) {
  m_pending = Input{std::move(placed), cellHeight, rowWidth, scale, std::move(font), inks};
}

void ContextRow::apply(Renderer& renderer) {
  if (!m_pending) {
    return;
  }
  auto [placed, cellHeight, rowWidth, scale, font, inks] = std::move(*std::exchange(m_pending, std::nullopt));
  const bool sameItems = placed.size() == m_placed.size()
      && std::equal(placed.begin(), placed.end(), m_placed.begin(),
                    [](const Placed& a, const Placed& b) { return a.item == b.item; });
  const bool sameSize = cellHeight == m_cellHeight && rowWidth == m_rowWidth && scale == m_scale && font == m_font
      && inks == m_inks;
  const float backing = m_grid.textBacking();
  if (!sameItems || !sameSize || backing != m_backing) {
    m_backing = backing;
    std::optional<Color> back;
    if (backing > 0.0F) {
      const std::uint32_t bg = m_grid.background();
      back = rgb(bg);
      back->a = backing;
    }
    for (Node* node : m_nodes) {
      m_layer->removeChild(node);
    }
    for (Node* node : m_marks) {
      m_layer->removeChild(node);
    }
    m_nodes.clear();
    m_marks.clear();
    const int side = static_cast<int>(std::lround(font.size));
    for (const Placed& p : placed) {
      // The grid does not arrange its children, and a layout request from the frame tick does not reach the window.
      Node* row = m_layer->addChild(build(p.item, cellHeight, rowWidth, scale, font, inks, back));
      row->layout(renderer);
      m_nodes.push_back(row);
      auto mark = std::make_unique<Image>();
      mark->setSize(static_cast<float>(side), static_cast<float>(side));
      mark->setForegroundTint(fixedColorSpec(inks.accent));
      (void)mark->setSourceFile(renderer, paths::assetPath("amark.svg").string(), side);
      m_marks.push_back(m_layer->addChild(std::move(mark)));
    }
  }
  // The band padding sits left of column 0, so the path lines up with the text below.
  // The brand mark fills the first of the prompt's blank cells, on the row under the band.
  for (std::size_t i = 0; i < placed.size(); ++i) {
    const float y = static_cast<float>(placed[i].row) * cellHeight;
    m_nodes[i]->setPosition(-Tokens::spaceP8 * scale, y);
    m_marks[i]->setPosition(0.0F, y + cellHeight + (cellHeight - m_marks[i]->height()) / 2.0F);
  }
  m_placed = std::move(placed);
  m_cellHeight = cellHeight;
  m_rowWidth = rowWidth;
  m_scale = scale;
  m_font = std::move(font);
  m_inks = inks;
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
    // The first child is the band (inside its backing box over a scene): the path, then the branch.
    const Node* band = row->children()[0].get();
    float bx = row->x() + band->x();
    if (m_backing > 0.0F) {
      band = band->children()[0].get();
      bx += band->x();
    }
    // The labels in order; the band's fill is a child too, outside layout.
    std::size_t c = 0;
    for (const auto& child : band->children()) {
      if (dynamic_cast<const Label*>(child.get()) == nullptr) {
        continue;
      }
      const float px = bx + child->x();
      if (lx >= px && lx < px + child->width()) {
        return c == 0 ? Hit{Hit::Pill::Cwd, item.path} : Hit{Hit::Pill::Branch, item.branch};
      }
      ++c;
    }
  }
  return std::nullopt;
}

} // namespace term
