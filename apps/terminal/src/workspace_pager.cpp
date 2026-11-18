#include "workspace_pager.h"

#include "design_tokens.h"

#include <algorithm>
#include <cmath>

namespace term {

  namespace {
    constexpr std::uint32_t kVelocityWindowMs = 100;

    // §7.7: past a limit by x, the offset is (1 - 1 / (x × 0.55 / D + 1)) × D.
    float rubberBand(float x, float dimension) {
      if (dimension <= 0.0F) return 0.0F;
      return (1.0F - 1.0F / (x * Tokens::motionRubberBand / dimension + 1.0F)) * dimension;
    }
  } // namespace

  void WorkspacePager::setPages(std::size_t count, std::size_t current) {
    m_count = std::max<std::size_t>(count, 1);
    const std::size_t page = std::min(current, m_count - 1);
    m_mode = Rest{page};
    m_samples.clear();
    m_position = static_cast<float>(page) * m_width;
  }

  void WorkspacePager::setWidth(float width) {
    const std::size_t shownPage = page();
    m_width = width;
    if (std::holds_alternative<Rest>(m_mode)) m_position = static_cast<float>(shownPage) * m_width;
  }

  std::size_t WorkspacePager::page() const {
    if (const auto* rest = std::get_if<Rest>(&m_mode)) return rest->page;
    if (const auto* tracking = std::get_if<Tracking>(&m_mode)) return tracking->from;
    return std::get<Settling>(m_mode).target;
  }

  void WorkspacePager::track(float dx, std::uint32_t timeMs) {
    if (!std::holds_alternative<Tracking>(m_mode)) {
      m_mode = Tracking{page()};
      m_samples.clear();
    }
    m_position += dx;
    m_samples.push_back({timeMs, m_position});
    while (m_samples.size() > 2 && timeMs - m_samples.front().time > kVelocityWindowMs) m_samples.pop_front();
  }

  float WorkspacePager::velocity() const {
    if (m_samples.size() < 2) return 0.0F;
    const std::uint32_t t0 = m_samples.front().time;
    double meanT = 0.0;
    double meanP = 0.0;
    for (const Sample& s : m_samples) {
      meanT += static_cast<double>(s.time - t0);
      meanP += s.position;
    }
    const auto n = static_cast<double>(m_samples.size());
    meanT /= n;
    meanP /= n;
    double cov = 0.0;
    double var = 0.0;
    for (const Sample& s : m_samples) {
      const double dt = static_cast<double>(s.time - t0) - meanT;
      cov += dt * (s.position - meanP);
      var += dt * dt;
    }
    return var > 0.0 ? static_cast<float>(cov / var * 1000.0) : 0.0F;
  }

  std::size_t WorkspacePager::release() {
    if (!std::holds_alternative<Tracking>(m_mode)) return page();
    const float decay = Tokens::motionProjectionDecayStepped;
    const float end = m_position + velocity() * decay / (1.0F - decay) / 1000.0F;
    const float step = m_width > 0.0F ? std::round(end / m_width) : 0.0F;
    const auto target = static_cast<std::size_t>(std::clamp(step, 0.0F, static_cast<float>(m_count - 1)));
    m_mode = Settling{target};
    return target;
  }

  void WorkspacePager::goTo(std::size_t page) {
    if (std::holds_alternative<Tracking>(m_mode)) return;
    m_mode = Settling{std::min(page, m_count - 1)};
  }

  void WorkspacePager::animate(float position) {
    if (std::holds_alternative<Settling>(m_mode)) m_position = position;
  }

  void WorkspacePager::settled() {
    const auto* settling = std::get_if<Settling>(&m_mode);
    if (settling == nullptr) return;
    m_mode = Rest{settling->target};
    m_position = static_cast<float>(settling->target) * m_width;
  }

  float WorkspacePager::shown() const {
    if (m_position < 0.0F) return -rubberBand(-m_position, m_width);
    if (m_position > maxPosition()) return maxPosition() + rubberBand(m_position - maxPosition(), m_width);
    return m_position;
  }

} // namespace term
