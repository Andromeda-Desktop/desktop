#include "ui/controls/reorder_controller.h"

#include "core/deferred_call.h"
#include "core/input/cursor_shape.h"
#include "core/input/key_modifiers.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace {
  constexpr std::int32_t kLiftedZ = 5;

  float offsetY(const Node& node, const Node& ancestor) {
    float nodeX = 0.0F;
    float nodeY = 0.0F;
    float ancestorX = 0.0F;
    float ancestorY = 0.0F;
    Node::absolutePosition(&node, nodeX, nodeY);
    Node::absolutePosition(&ancestor, ancestorX, ancestorY);
    return nodeY - ancestorY - node.translationY();
  }
} // namespace

ReorderController::ReorderController(float scale, std::function<void(ReorderDrag::Move)> onMove)
    : m_scale(scale), m_onMove(std::move(onMove)), m_drag(Style::dragStartThreshold * scale) {}

ReorderController::~ReorderController() { *m_alive = false; }

void ReorderController::setScale(float scale) {
  m_drag.cancel();
  m_scale = scale;
  m_drag = ReorderDrag(Style::dragStartThreshold * scale);
}

void ReorderController::attach(Node& list, std::vector<Node*> rows, std::vector<Node*> decor) {
  m_drag.cancel();
  if (&list == m_list) {
    removeLiftNodes();
  }
  m_shadow = nullptr;
  m_plate = nullptr;
  m_list = &list;
  m_rows = std::move(rows);
  m_decor = std::move(decor);
  m_offsetAnims.assign(m_rows.size(), 0);
  m_handles = std::move(m_pendingHandles);
  m_handles.resize(m_rows.size(), nullptr);
  m_pendingHandles.clear();
}

void ReorderController::removeLiftNodes() {
  // Compares pointers only: the owner may already have removed (and freed) them with the rows.
  for (Box* node : {m_shadow, m_plate}) {
    const auto& children = m_list->children();
    if (node != nullptr && std::ranges::any_of(children, [node](const auto& c) { return c.get() == node; })) {
      m_list->removeChild(node);
    }
  }
  m_shadow = nullptr;
  m_plate = nullptr;
}

std::unique_ptr<Button> ReorderController::makeHandle(std::size_t index, std::string_view tooltip) {
  auto handle = ui::button({
      .glyph = "grip-vertical",
      .glyphSize = Tokens::sizeSymbolInline * m_scale,
      .variant = ButtonVariant::Ghost,
      .tooltip = std::string(tooltip),
      .minWidth = Tokens::sizeControlS * m_scale,
  });
  m_tooltip = std::string(tooltip);
  Button* button = handle.get();
  button->setCursorShape(CursorShape::Grab);
  if (index >= m_pendingHandles.size()) {
    m_pendingHandles.resize(index + 1, nullptr);
  }
  m_pendingHandles[index] = button;
  button->setOnPress([this, button, index](float, float localY, bool pressed) {
    if (pressed) {
      press(index, *button, localY);
    } else {
      release();
    }
  });
  button->setOnPointerMotion([this, button](float, float localY) { motion(*button, localY); });
  button->inputArea()->setOnCancel([this] { cancel(); });
  button->inputArea()->setOnKeyDown([this, index](const InputArea::KeyData& key) {
    if (!key.pressed) {
      return;
    }
    if (key.sym == XKB_KEY_Escape) {
      cancel();
      return;
    }
    if ((key.modifiers & KeyMod::Alt) == 0 || m_drag.phase() != ReorderDrag::Phase::Idle) {
      return;
    }
    if (key.sym == XKB_KEY_Up && index > 0) {
      commitLater({.from = index, .insertion = index - 1});
    } else if (key.sym == XKB_KEY_Down && index + 1 < m_rows.size()) {
      commitLater({.from = index, .insertion = index + 2});
    }
  });
  return handle;
}

float ReorderController::listY(const Node& handle, float localY) const {
  float handleX = 0.0F;
  float handleY = 0.0F;
  float listX = 0.0F;
  float listYPos = 0.0F;
  Node::absolutePosition(&handle, handleX, handleY);
  Node::absolutePosition(m_list, listX, listYPos);
  return handleY + localY - listYPos;
}

void ReorderController::press(std::size_t index, const Node& handle, float localY) {
  if (m_list == nullptr || index >= m_rows.size() || m_drag.phase() != ReorderDrag::Phase::Idle) {
    return;
  }
  std::vector<ReorderDrag::Row> rows;
  rows.reserve(m_rows.size());
  for (const Node* row : m_rows) {
    rows.push_back({.top = offsetY(*row, *m_list), .height = row->height()});
  }
  m_drag.press(index, listY(handle, localY), std::move(rows));
  // A tooltip over the grabbed handle would cover the rows while they move.
  if (m_drag.phase() == ReorderDrag::Phase::Armed && m_handles[index] != nullptr) {
    m_handles[index]->inputArea()->clearTooltip();
  }
}

void ReorderController::restoreTooltips() {
  if (m_tooltip.empty()) {
    return;
  }
  for (Button* handle : m_handles) {
    if (handle != nullptr && !handle->inputArea()->hasTooltip()) {
      handle->inputArea()->setTooltip(m_tooltip);
    }
  }
}

void ReorderController::motion(const Node& handle, float localY) {
  if (m_list == nullptr) {
    return;
  }
  const ReorderDrag::Phase before = m_drag.phase();
  m_drag.motion(listY(handle, localY));
  if (m_drag.phase() != ReorderDrag::Phase::Dragging) {
    return;
  }
  if (before == ReorderDrag::Phase::Armed) {
    lift(true);
  }
  Node* lifted = m_rows[*m_drag.liftedRow()];
  lifted->setTranslation(0.0F, m_drag.offsetFor(*m_drag.liftedRow()));
  driveOffsets();
  syncShadow();
}

void ReorderController::release() {
  const ReorderDrag::Phase before = m_drag.phase();
  m_drag.release();
  restoreTooltips();
  if (before != ReorderDrag::Phase::Dragging) {
    return;
  }
  const std::size_t from = *m_drag.liftedRow();
  Node* lifted = m_rows[from];
  AnimationManager* manager = lifted->animationManager();
  const auto finish = [this, alive = std::weak_ptr<bool>(m_alive)] {
    if (alive.expired()) {
      return;
    }
    lift(false);
    if (const auto move = m_drag.settled()) {
      commitLater(*move);
    }
  };
  const float target = m_drag.offsetFor(from);
  if (manager == nullptr) {
    lifted->setTranslation(0.0F, target);
    finish();
    return;
  }
  m_offsetAnims[from] = manager->drive(
      m_offsetAnims[from], lifted->translationY(), target, Style::spring(Tokens::springMove),
      [this, lifted](float y) {
        lifted->setTranslation(0.0F, y);
        syncShadow();
      },
      finish, lifted, 0.0F, Unit::Px, true
  );
}

void ReorderController::cancel() {
  if (m_drag.phase() == ReorderDrag::Phase::Idle) {
    return;
  }
  const bool wasLifted = m_drag.liftedRow().has_value();
  m_drag.cancel();
  restoreTooltips();
  if (wasLifted) {
    lift(false);
  }
  driveOffsets();
}

void ReorderController::lift(bool lifted) {
  if (m_list == nullptr) {
    return;
  }
  const auto rowIndex = lifted ? m_drag.liftedRow() : std::nullopt;
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    Node* row = m_rows[i];
    const bool isLifted = rowIndex == i;
    if (!isLifted && row->scaleX() == 1.0F) {
      continue;
    }
    row->setZIndex(isLifted ? kLiftedZ : 0);
    row->setTransformOrigin(row->width() * 0.5F, row->height() * 0.5F);
    const float to = isLifted ? Tokens::componentEditDragScale : 1.0F;
    if (AnimationManager* manager = row->animationManager(); manager != nullptr) {
      manager->animateSpatial(row->scaleX(), to, Style::spring(Tokens::springSnap), [row](float s) { row->setScale(s); },
                              {}, row);
    } else {
      row->setScale(to);
    }
  }
  for (Node* decor : m_decor) {
    const float to = lifted ? 0.0F : 1.0F;
    if (AnimationManager* manager = decor->animationManager(); manager != nullptr) {
      manager->animateSpring(decor->opacity(), to, Style::spring(Tokens::springFade),
                             [decor](float o) { decor->setOpacity(o); }, {}, decor);
    } else {
      decor->setOpacity(to);
    }
  }
  for (std::size_t i = 0; i < m_handles.size(); ++i) {
    if (m_handles[i] != nullptr) {
      m_handles[i]->setCursorShape(rowIndex == i ? CursorShape::Grabbing : CursorShape::Grab);
    }
  }
  if (lifted && m_shadow == nullptr) {
    const float radius = Tokens::componentListRowRadius * m_scale * Style::cornerRadiusScale();
    m_shadow = static_cast<Box*>(m_list->addChild(ui::box({
        .fill = colorSpecFromRole(
            ColorRole::Shadow, isResolvedLightTheme() ? Tokens::elevationE3.keyLight : Tokens::elevationE3.keyDark
        ),
        .radius = radius,
        .visible = false,
        .participatesInLayout = false,
        .configure = [](Box& box) { box.setZIndex(kLiftedZ - 2); },
    })));
    m_shadow->setShadow(Tokens::elevationE3.keyBlur * m_scale, Tokens::elevationE3.keyY * m_scale);
    m_plate = static_cast<Box*>(m_list->addChild(ui::box({
        .fill = colorSpecFromRole(ColorRole::SolidRegular),
        .radius = radius,
        .visible = false,
        .participatesInLayout = false,
        .configure = [](Box& box) { box.setZIndex(kLiftedZ - 1); },
    })));
  }
  if (lifted) {
    m_shadow->setVisible(true);
    m_plate->setVisible(true);
    syncShadow();
  } else {
    removeLiftNodes();
  }
}

void ReorderController::driveOffsets() {
  const auto lifted = m_drag.liftedRow();
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    if (lifted == i && m_drag.phase() == ReorderDrag::Phase::Dragging) {
      continue;
    }
    Node* row = m_rows[i];
    const float target = m_drag.offsetFor(i);
    AnimationManager* manager = row->animationManager();
    if (manager == nullptr) {
      row->setTranslation(0.0F, target);
      continue;
    }
    if (row->translationY() == target && m_offsetAnims[i] == 0) {
      continue;
    }
    m_offsetAnims[i] = manager->drive(
        m_offsetAnims[i], row->translationY(), target, Style::spring(Tokens::springMove),
        [this, row](float y) {
          row->setTranslation(0.0F, y);
          syncShadow();
        },
        {}, row, 0.0F, Unit::Px, true
    );
  }
}

void ReorderController::syncShadow() {
  if (m_shadow == nullptr || m_list == nullptr) {
    return;
  }
  const auto lifted = m_drag.liftedRow();
  if (!lifted.has_value()) {
    return;
  }
  const Node* row = m_rows[*lifted];
  const float scale = row->scaleX();
  const float w = row->width() * scale;
  const float h = row->height() * scale;
  const float x = row->x() + (row->width() - w) * 0.5F;
  const float y = offsetY(*row, *m_list) + row->translationY() + (row->height() - h) * 0.5F;
  m_plate->setPosition(x, y);
  m_plate->setFrameSize(w, h);
  m_shadow->setPosition(x, y + Tokens::elevationE3.keyY * m_scale);
  m_shadow->setFrameSize(w, h);
}

void ReorderController::commitLater(ReorderDrag::Move move) {
  // The owner rebuilds the rows on a move; never inside a callback that one of those rows owns.
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive), move] {
    if (!alive.expired() && m_onMove) {
      m_onMove(move);
    }
  });
}
