#include "ui/controls/selection_list.h"

#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

SelectionList::SelectionList(float scale) : m_scale(scale) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  auto tint = std::make_unique<Box>();
  tint->setParticipatesInLayout(false);
  tint->setZIndex(-1);
  tint->setVisible(false);
  m_tint = static_cast<Box*>(addChild(std::move(tint)));
}

void SelectionList::setSelected(Node* child, bool animate, std::function<void()> onCross) {
  if (child == m_selected) {
    return;
  }
  const bool travel = animate && m_selected != nullptr && child != nullptr && m_tint->visible();
  m_selected = child;
  m_onCross = std::move(onCross);
  if (!travel) {
    if (m_onCross) {
      std::exchange(m_onCross, {})();
    }
    m_pendingTravel = false;
  } else {
    m_pendingTravel = true; // the target rect is known after the next layout
  }
  markLayoutDirty();
}

void SelectionList::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);
  place();
}

void SelectionList::doArrange(Renderer& renderer, const LayoutRect& rect) {
  Flex::doArrange(renderer, rect);
  place();
}

void SelectionList::place() {
  m_tint->setFill(colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F));
  if (m_selected == nullptr || !m_selected->visible()) {
    m_tint->setVisible(false);
    return;
  }
  m_tint->setRadius(Style::scaledRadius(Tokens::radiusR8, m_scale));
  const float top = m_selected->y();
  const float bottom = top + m_selected->height();
  AnimationManager* animations = animationManager();
  if (std::exchange(m_pendingTravel, false) && animations != nullptr) {
    const bool down = top > m_top;
    const float fromTop = m_top;
    const float fromBottom = m_bottom;
    const float span = std::abs(top - fromTop);
    m_travelling = true;
    // Moving down, the bottom edge leads; moving up, the top edge leads.
    const auto& topSpring = Style::spring(down ? Tokens::springTrail : Tokens::springSnap);
    const auto& bottomSpring = Style::spring(down ? Tokens::springSnap : Tokens::springTrail);
    m_topAnim = animations->drive(m_topAnim, fromTop, top, topSpring, [this, fromTop, span](float v) {
      m_top = v;
      if (m_onCross && span > 0.0F && std::abs(v - fromTop) >= span * 0.5F) {
        std::exchange(m_onCross, {})();
      }
      apply();
    }, [this] {
      m_travelling = false;
      if (m_onCross) {
        std::exchange(m_onCross, {})();
      }
    }, this, 0.0F, Unit::Px);
    m_bottomAnim = animations->drive(m_bottomAnim, fromBottom, bottom, bottomSpring, [this](float v) {
      m_bottom = v;
      apply();
    }, {}, this, 0.0F, Unit::Px);
    if (m_topAnim == 0) { // motion off: the springs jumped
      m_top = top;
      m_bottom = bottom;
      m_travelling = false;
    }
  } else if (!m_travelling) {
    m_top = top;
    m_bottom = bottom;
  }
  m_tint->setVisible(true);
  apply();
}

void SelectionList::apply() {
  m_tint->setPosition(0.0F, m_top);
  m_tint->setFrameSize(width(), std::max(0.0F, m_bottom - m_top));
}
