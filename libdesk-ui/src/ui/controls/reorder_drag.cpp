#include "ui/controls/reorder_drag.h"

#include <algorithm>
#include <cmath>

std::optional<std::size_t> ReorderDrag::liftedRow() const noexcept {
  if (m_phase == Phase::Dragging || m_phase == Phase::Settling) {
    return m_from;
  }
  return std::nullopt;
}

void ReorderDrag::press(std::size_t row, float pointerY, std::vector<Row> rows) {
  if (m_phase != Phase::Idle || row >= rows.size()) {
    return;
  }
  m_rows = std::move(rows);
  m_from = row;
  m_insertion = row;
  m_pressY = pointerY;
  m_pointerY = pointerY;
  m_phase = Phase::Armed;
}

void ReorderDrag::motion(float pointerY) {
  if (m_phase == Phase::Armed) {
    m_pointerY = pointerY;
    if (std::abs(pointerY - m_pressY) >= m_slop) {
      m_phase = Phase::Dragging;
    }
  } else if (m_phase == Phase::Dragging) {
    m_pointerY = pointerY;
  } else {
    return;
  }
  if (m_phase == Phase::Dragging) {
    const Row& lifted = m_rows[m_from];
    m_insertion = insertionFor(lifted.top + liftedTravel() + lifted.height * 0.5F);
  }
}

void ReorderDrag::release() {
  if (m_phase == Phase::Armed) {
    m_phase = Phase::Idle;
  } else if (m_phase == Phase::Dragging) {
    m_phase = Phase::Settling;
  }
}

std::optional<ReorderDrag::Move> ReorderDrag::settled() {
  if (m_phase != Phase::Settling) {
    return std::nullopt;
  }
  m_phase = Phase::Idle;
  if (m_insertion == m_from || m_insertion == m_from + 1) {
    return std::nullopt;
  }
  return Move{.from = m_from, .insertion = m_insertion};
}

void ReorderDrag::cancel() { m_phase = Phase::Idle; }

std::size_t ReorderDrag::insertionFor(float liftedCenter) const {
  std::size_t above = 0;
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    if (i != m_from && m_rows[i].top + m_rows[i].height * 0.5F < liftedCenter) {
      ++above;
    }
  }
  return above <= m_from ? above : above + 1;
}

float ReorderDrag::liftedTravel() const {
  const Row& lifted = m_rows[m_from];
  const float minTop = m_rows.front().top;
  const float maxTop = m_rows.back().top + m_rows.back().height - lifted.height;
  return std::clamp(lifted.top + m_pointerY - m_pressY, minTop, std::max(minTop, maxTop)) - lifted.top;
}

float ReorderDrag::pitch() const {
  const float gap = m_rows.size() >= 2 ? m_rows[1].top - (m_rows[0].top + m_rows[0].height) : 0.0F;
  return m_rows[m_from].height + gap;
}

float ReorderDrag::offsetFor(std::size_t row) const {
  if ((m_phase != Phase::Dragging && m_phase != Phase::Settling) || row >= m_rows.size()) {
    return 0.0F;
  }
  const std::size_t target = m_insertion > m_from ? m_insertion - 1 : m_insertion;
  if (row == m_from) {
    if (m_phase == Phase::Dragging) {
      return liftedTravel();
    }
    if (target > m_from) {
      return m_rows[target].top + m_rows[target].height - (m_rows[m_from].top + m_rows[m_from].height);
    }
    return m_rows[target].top - m_rows[m_from].top;
  }
  if (row > m_from && row <= target) {
    return -pitch();
  }
  if (row >= target && row < m_from) {
    return pitch();
  }
  return 0.0F;
}
