#include "ui/controls/status_row.h"

#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <utility>

StatusRow::StatusRow(float scale) : m_scale(scale) {
  const float s = scale;
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Center);
  setGap(Tokens::spaceP8 * s);
  setPadding(Tokens::spaceP8 * s);
  setRadius(Style::scaledRadius(Tokens::radiusR8, s));
  setMinHeight(Tokens::sizeRowTwoCompact * s);
  addChild(ui::glyph({.out = &m_symbol, .glyphSize = Tokens::sizeSymbol * s}));
  auto text = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  text->addChild(ui::label({.out = &m_name, .fontSize = Tokens::typeBody.size * s, .maxLines = 1,
                            .ellipsize = TextEllipsize::End}));
  text->addChild(ui::label({.out = &m_status, .fontSize = Tokens::typeFootnote.size * s,
                            .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1,
                            .ellipsize = TextEllipsize::End}));
  m_text = addChild(std::move(text));
  addChild(ui::label({.out = &m_time, .fontSize = Tokens::typeFootnote.size * s, .fontFeatures = std::string("tnum"),
                      .maxLines = 1}));
  auto area = ui::inputArea({.participatesInLayout = false});
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnLeave([this] { applyState(); });
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    if (m_onClick) {
      m_onClick();
    }
  });
  m_area = static_cast<InputArea*>(addChild(std::move(area)));
  applyState();
}

void StatusRow::setData(const Data& data) {
  m_data = data;
  m_symbol->setGlyph(data.symbol);
  m_name->setText(data.name);
  m_status->setText(data.status);
  m_status->setVisible(!data.status.empty());
  m_status->setParticipatesInLayout(!data.status.empty());
  m_time->setText(data.time);
  if (m_rail) {
    m_area->setTooltip(data.name);
  }
  applyState();
  markLayoutDirty();
}

void StatusRow::setTime(const std::string& time) {
  m_data.time = time;
  if (m_time->setText(time)) {
    markLayoutDirty();
  }
}

void StatusRow::setRail(bool rail) {
  m_rail = rail;
  setJustify(rail ? FlexJustify::Center : FlexJustify::Start);
  for (Node* node : {m_text, static_cast<Node*>(m_time)}) {
    node->setVisible(!rail);
    node->setParticipatesInLayout(!rail);
  }
  m_area->setTooltip(rail ? m_data.name : std::string());
  markLayoutDirty();
}

void StatusRow::setOnClick(std::function<void()> callback) { m_onClick = std::move(callback); }

void StatusRow::applyState() {
  const bool strong = m_data.unseen || m_data.selected;
  if (!m_data.selected && m_area != nullptr && m_area->hovered()) {
    setFill(colorSpecFromRole(ColorRole::Fill1));
  } else {
    setFill(clearColorSpec());
  }
  const ColorSpec label1 = colorSpecFromRole(ColorRole::Label1);
  m_symbol->setColor(m_data.symbolDanger ? colorSpecFromRole(ColorRole::Danger)
                                         : (m_data.selected ? label1 : colorSpecFromRole(ColorRole::Label2)));
  m_name->setColor(label1);
  m_name->setFontWeight(m_data.unseen ? FontWeight::SemiBold : FontWeight::Normal);
  m_status->setColor(colorSpecFromRole(m_data.selected ? ColorRole::Label1 : ColorRole::Label2));
  m_time->setColor(colorSpecFromRole(strong ? ColorRole::Label1 : ColorRole::Label2));
  m_time->setFontWeight(m_data.unseen ? FontWeight::SemiBold : FontWeight::Normal);
}

void StatusRow::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);
  placeArea();
}

void StatusRow::doArrange(Renderer& renderer, const LayoutRect& rect) {
  Flex::doArrange(renderer, rect);
  placeArea();
}

void StatusRow::placeArea() {
  if (m_area != nullptr) {
    m_area->setPosition(0.0F, 0.0F);
    m_area->setSize(width(), height());
  }
}
