#include "ui/controls/status_row.h"

#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/spinner.h"
#include "ui/palette.h"
#include "ui/style.h"

#include "core/input/input_codes.h"
#include <algorithm>
#include <cmath>
#include <utility>

StatusRow::StatusRow(float scale) : m_scale(scale) {
  const float s = scale;
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Center);
  setGap(Tokens::spaceP8 * s);
  setPadding(Tokens::spaceP8 * s);
  setRadius(Style::scaledRadius(Tokens::radiusR12, s));
  // Under the fill: the raised chip's E1 shadow.
  insertChildAt(0, ui::box({.out = &m_shadow,
                            .fill = colorSpecFromRole(ColorRole::Shadow, isResolvedLightTheme() ? Tokens::elevationE1.keyLight
                                                                                                : Tokens::elevationE1.keyDark),
                            .radius = Style::scaledRadius(Tokens::radiusR12, s),
                            .visible = false,
                            .participatesInLayout = false}));
  m_shadow->setShadow(Tokens::elevationE1.keyBlur * s, Tokens::elevationE1.keyY * s);
  setMinHeight(Tokens::sizeRowTwoCompact * s);
  addChild(ui::glyph({.out = &m_symbol, .glyphSize = Tokens::sizeSymbol * s}));
  addChild(ui::spinner({.out = &m_spinner, .color = colorSpecFromRole(ColorRole::Label2), .spinnerSize = Tokens::sizeSymbol * s,
                        .visible = false, .participatesInLayout = false}));
  auto text = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  text->addChild(ui::label({.out = &m_name, .fontSize = Tokens::typeBody.size * s, .maxLines = 1,
                            .ellipsize = TextEllipsize::End}));
  text->addChild(ui::input({.out = &m_field, .fontSize = Tokens::typeBody.size * s, .visible = false,
                            .participatesInLayout = false,
                            .onSubmit = [this](const std::string& text) { endRename(text); },
                            .onKeyEvent = [this](std::uint32_t sym, std::uint32_t /*mods*/) {
                              constexpr std::uint32_t kEscape = 0xff1b;
                              if (sym != kEscape) {
                                return false;
                              }
                              endRename(std::nullopt);
                              return true;
                            },
                            .submitOnFocusLoss = true}));
  text->addChild(ui::label({.out = &m_status, .fontSize = Tokens::typeFootnote.size * s,
                            .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1,
                            .ellipsize = TextEllipsize::End}));
  // The token lines (body 16 + footnote 16) keep a two-line row at 48 with its 8 padding.
  m_name->setLineHeight(Tokens::typeBody.line * s);
  m_status->setLineHeight(Tokens::typeFootnote.line * s);
  m_text = addChild(std::move(text));
  addChild(ui::label({.out = &m_time, .fontSize = Tokens::typeFootnote.size * s, .fontFeatures = std::string("tnum"),
                      .maxLines = 1}));
  auto area = ui::inputArea({.participatesInLayout = false});
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnLeave([this] { applyState(); });
  area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT || !m_onDrag) {
      return;
    }
    if (data.pressed) {
      m_drag = Drag::Pressed;
      m_pressX = data.sceneX;
      m_pressY = data.sceneY;
      return;
    }
    const bool dropped = m_drag == Drag::Dragging;
    m_drag = dropped || m_drag == Drag::Cancelled ? Drag::Dropped : Drag::Idle;
    if (dropped) {
      setOpacity(1.0F);
      m_onDrag(DragPhase::Drop, data.sceneX, data.sceneY);
    }
  });
  area->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_drag == Drag::Pressed && std::hypot(data.sceneX - m_pressX, data.sceneY - m_pressY) > Tokens::spaceP4 * m_scale) {
      m_drag = Drag::Dragging;
      setOpacity(Tokens::componentSplitDragSourceOpacity);
      applyState();
    }
    if (m_drag == Drag::Dragging) {
      m_onDrag(DragPhase::Move, data.sceneX, data.sceneY);
    }
  });
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT && m_drag == Drag::Dropped) {
      m_drag = Drag::Idle;
      return;
    }
    if (data.button == BTN_RIGHT) {
      if (m_onSecondaryClick) {
        m_onSecondaryClick(data.sceneX, data.sceneY);
      }
    } else if (m_onClick) {
      m_onClick();
    }
  });
  m_area = static_cast<InputArea*>(addChild(std::move(area)));
  // After the area, so the buttons take the hit; last in the flex, so they trail.
  m_actions = static_cast<Flex*>(addChild(ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .visible = false,
                                                   .participatesInLayout = false})));
  applyState();
}

void StatusRow::setData(const Data& data) {
  m_data = data;
  m_symbol->setGlyph(data.symbol);
  m_symbol->setVisible(!data.spinning);
  m_symbol->setParticipatesInLayout(!data.spinning);
  m_spinner->setVisible(data.spinning);
  m_spinner->setParticipatesInLayout(data.spinning);
  if (data.spinning != m_spinner->spinning()) {
    data.spinning ? m_spinner->start() : m_spinner->stop();
  }
  m_name->setText(data.name);
  m_status->setText(data.status);
  m_status->setVisible(!data.status.empty());
  m_status->setParticipatesInLayout(!data.status.empty());
  m_time->setText(data.time);
  applyState();
  markLayoutDirty();
}

void StatusRow::setTime(const std::string& time) {
  m_data.time = time;
  if (m_time->setText(time)) {
    markLayoutDirty();
  }
}

void StatusRow::cancelDrag() {
  if (m_drag != Drag::Dragging) {
    return;
  }
  m_drag = Drag::Cancelled;
  setOpacity(1.0F);
  applyState();
  m_onDrag(DragPhase::Cancel, m_pressX, m_pressY);
}

void StatusRow::setOneLine(bool oneLine) {
  setMinHeight((oneLine ? Tokens::sizeRowCompact : Tokens::sizeRowTwoCompact) * m_scale);
}

void StatusRow::setRail(bool rail) {
  m_rail = rail;
  setJustify(rail ? FlexJustify::Center : FlexJustify::Start);
  m_text->setVisible(!rail);
  m_text->setParticipatesInLayout(!rail);
  applyState();
  markLayoutDirty();
}

void StatusRow::setActions(std::vector<Action> actions) {
  for (Action& action : actions) {
    Button* button = nullptr;
    m_actions->addChild(ui::button({.out = &button,
                                    .glyph = std::move(action.glyph),
                                    .glyphSize = Tokens::sizeSymbolInline * m_scale,
                                    .variant = ButtonVariant::Ghost,
                                    .tooltip = std::move(action.tooltip),
                                    .minWidth = Tokens::sizeControlS * m_scale,
                                    .minHeight = Tokens::sizeControlS * m_scale,
                                    .padding = 0.0F,
                                    .radius = Style::scaledRadiusMd(m_scale),
                                    .onClick = std::move(action.onClick)}));
    button->setOnEnter([this] { applyState(); });
    button->setOnLeave([this] { applyState(); });
    m_actionButtons.push_back(button);
  }
  applyState();
}

bool StatusRow::hot() const {
  return (m_area != nullptr && m_area->hovered()) || std::ranges::any_of(m_actionButtons, &Button::hovered);
}

void StatusRow::setOnClick(std::function<void()> callback) { m_onClick = std::move(callback); }

void StatusRow::setOnSecondaryClick(std::function<void(float, float)> callback) { m_onSecondaryClick = std::move(callback); }

void StatusRow::beginRename(const std::string& name, std::function<void(std::optional<std::string>)> done) {
  m_mode = Mode::Renaming;
  m_renameDone = std::move(done);
  m_field->setValue(name);
  m_field->selectAll();
  for (const auto& [node, shown] : {std::pair<Node*, bool>{m_name, false}, {m_field, true}}) {
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
  }
  markLayoutDirty();
}

InputArea* StatusRow::renameArea() const noexcept { return m_field->inputArea(); }

void StatusRow::endRename(std::optional<std::string> name) {
  if (m_mode != Mode::Renaming) {
    return; // Return already ended it; the focus loss that follows is not a second answer
  }
  m_mode = Mode::Display;
  for (const auto& [node, shown] : {std::pair<Node*, bool>{m_name, true}, {m_field, false}}) {
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
  }
  markLayoutDirty();
  if (auto done = std::exchange(m_renameDone, nullptr)) {
    done(std::move(name));
  }
}

void StatusRow::applyState() {
  const bool strong = m_data.unseen || m_data.selected;
  if (m_data.chip) {
    setFill(*m_data.chip);
  } else if (!m_data.selected && hot()) {
    setFill(colorSpecFromRole(ColorRole::Fill1));
  } else {
    setFill(clearColorSpec());
  }
  setBorder(m_data.chip ? colorSpecFromRole(ColorRole::Stroke) : clearColorSpec(), m_data.chip ? 0.5F * m_scale : 0.0F);
  m_shadow->setVisible(m_data.chip.has_value());
  // The chip's ink follows its fill, not the sidebar's scheme.
  setPaletteTone(m_data.chip ? std::optional(toneOn(resolveColorSpec(*m_data.chip))) : std::nullopt);
  const ColorSpec label1 = colorSpecFromRole(ColorRole::Label1);
  m_symbol->setColor(m_data.symbolDanger ? colorSpecFromRole(ColorRole::Danger)
                                         : (m_data.selected ? label1 : colorSpecFromRole(ColorRole::Label2)));
  m_name->setColor(label1);
  m_name->setFontWeight(m_data.unseen || m_data.chip ? FontWeight::SemiBold : FontWeight::Normal);
  m_status->setColor(colorSpecFromRole(m_data.selected ? ColorRole::Label1 : ColorRole::Label2));
  m_time->setColor(colorSpecFromRole(strong ? ColorRole::Label1 : ColorRole::Label2));
  m_time->setFontWeight(m_data.unseen ? FontWeight::SemiBold : FontWeight::Normal);
  const bool actions = !m_rail && !m_actionButtons.empty() && m_drag != Drag::Dragging && hot();
  if (m_actions != nullptr && m_actions->visible() != actions) {
    m_actions->setVisible(actions);
    m_actions->setParticipatesInLayout(actions);
    markLayoutDirty();
  }
  m_time->setVisible(!m_rail && !actions);
  m_time->setParticipatesInLayout(!m_rail && !actions);
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
    // Set only on a change: a new tooltip while hovered restarts the shown one.
    std::string tooltip = m_rail || m_name->ellipsized() ? m_data.name : std::string();
    const TooltipContent current = m_area->tooltipContent();
    const auto* shown = std::get_if<std::string>(&current);
    if (shown != nullptr ? *shown != tooltip : !tooltip.empty()) {
      m_area->setTooltip(std::move(tooltip));
    }
  }
  m_shadow->setPosition(0.0F, Tokens::elevationE1.keyY * m_scale);
  m_shadow->setSize(width(), height());
}
