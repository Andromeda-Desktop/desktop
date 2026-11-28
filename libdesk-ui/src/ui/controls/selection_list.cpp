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

float SelectionList::selectionTintAlpha() { return isResolvedLightTheme() ? 0.32F : 0.24F; }

void SelectionList::place() {
  m_tint->setFill(m_fill.value_or(colorSpecFromRole(ColorRole::Accent, selectionTintAlpha())));
  if (m_selected == nullptr || !m_selected->visible()) {
    m_tint->setVisible(false);
    return;
  }
  // The tint takes the selected child's corners (a block of rows is rounder than a row).
  const auto* row = dynamic_cast<const Flex*>(m_selected);
  m_tint->setRadius(row != nullptr && row->radius() > 0.0F ? row->radius() : Style::scaledRadius(Tokens::radiusR8, m_scale));
  const float top = m_selected->y();
  // A row that is still opening: the tint takes its rest height, so it does not shrink and grow with the opening.
  const float bottom = top + (m_selected == m_inserting ? std::max(m_insertHeight, m_selected->height())
                                                         : m_selected->height());
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

void SelectionList::insertAnimated(std::unique_ptr<Flex> row, std::size_t index, float height) {
  Flex* added = static_cast<Flex*>(insertChildAt(index + 1, std::move(row)));
  AnimationManager* animations = animationManager();
  if (animations == nullptr) {
    return;
  }
  const float restMin = added->minHeight();
  m_inserting = added;
  m_insertHeight = height;
  added->setClipChildren(true);
  added->setOpacity(0.0F);
  added->setMinHeight(0.0F);
  added->setMaxHeight(0.01F); // 0 reads as no limit
  animations->animateGated(
      0.0F, height, Style::spring(Tokens::springMove),
      [this, added](float h) {
        added->setMinHeight(h);
        added->setMaxHeight(std::max(h, 0.01F));
        markLayoutDirty();
      },
      Tokens::motionContentGate,
      [animations, added, this] {
        animations->animateSpring(0.0F, 1.0F, Style::spring(Tokens::springFade),
                                  [added](float v) { added->setOpacity(v); }, {}, added);
      },
      [this, added, restMin] {
        if (m_inserting == added) {
          m_inserting = nullptr;
        }
        added->setMinHeight(restMin);
        added->setMaxHeight(0.0F);
        added->setClipChildren(false);
        added->setOpacity(1.0F);
        markLayoutDirty();
      },
      added, 0.0F, Unit::Px);
}

void SelectionList::removeAnimated(Flex* row, std::function<void()> beforeFree) {
  auto free = [this, row, beforeFree = std::move(beforeFree)] {
    if (row == m_selected) {
      m_selected = nullptr;
    }
    if (row == m_inserting) {
      m_inserting = nullptr;
    }
    std::unique_ptr<Node> owned = removeChild(row);
    if (beforeFree) {
      beforeFree();
    }
    markLayoutDirty();
  };
  AnimationManager* animations = animationManager();
  if (animations == nullptr) {
    free();
    return;
  }
  animations->cancelForOwner(row); // its opening, if it is still opening
  row->setClipChildren(true);
  const float from = row->height();
  row->setMinHeight(0.0F);
  // Content is gone when 35 % of the height is left, the mirror of insertAnimated's fade-in.
  animations->animateSpring(
      from, 0.0F, Style::spring(Tokens::springMove),
      [this, row, from](float h) {
        row->setMaxHeight(std::max(h, 0.01F));
        row->setOpacity(std::clamp((h / from - 0.35F) / 0.65F, 0.0F, 1.0F));
        markLayoutDirty();
      },
      std::move(free), this, 0.0F, Unit::Px);
}
