#include "scene/dsk_glass.h"

#include "protocols/glass_material.h"

#include <algorithm>
#include <cmath>

namespace andromeda {

  DskGlass readDskGlass(const DskSurfaceManager::State& state, const SurfaceMotion& motion) {
    const auto unit = [](double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); };
    const auto& liquid = motion.value(SurfaceMotion::Property::Liquid);
    const auto& tint = motion.value(SurfaceMotion::Property::Tint);
    // Blur [0] is v15; an older client's blur gets lighter with its refraction, liquid [0]. Lens [2] is v9; an older
    // client gets it at the refraction strength.
    const auto& blur = motion.value(state.version >= 15 ? SurfaceMotion::Property::Blur : SurfaceMotion::Property::Liquid);
    return {
        .liquid = unit(liquid[0]),
        .rim = unit(liquid[1]),
        .lens = state.version >= 9 ? unit(liquid[2]) : unit(liquid[0]),
        .dense = unit(liquid[3]),
        .clarity = unit(blur[0]),
        .tint = {static_cast<float>(tint[0]), static_cast<float>(tint[1]), static_cast<float>(tint[2]), unit(tint[3])},
    };
  }

  bool GlassDensity::sample(const DskGlass& glass, float darkest, float brightest) {
    const bool first = !m_backdrop.has_value();
    m_backdrop = Backdrop{.darkest = darkest, .brightest = brightest};
    if (first) {
      // The first sample lands 1-3 frames after map, inside the open fade: apply it at once.
      apply(glass);
      m_density.snap(m_target);
      return false;
    }
    return apply(glass);
  }

  bool GlassDensity::retint(const DskGlass& glass) { return m_backdrop.has_value() && apply(glass); }

  void GlassDensity::reset() {
    m_backdrop.reset();
    m_target = 0.0F;
    m_density.snap(0.0);
  }

  float GlassDensity::amount(float alpha, float dense) const {
    return alpha <= 0.0F ? 0.0F : glass::denser(alpha, static_cast<float>(m_density.current()), dense);
  }

  bool GlassDensity::apply(const DskGlass& glass) {
    const auto& tint = glass.tint;
    const bool light = 0.2126F * tint[0] + 0.7152F * tint[1] + 0.0722F * tint[2] > 0.5F;
    const float threat = light ? m_backdrop->darkest : m_backdrop->brightest;
    const float target = glass.seeThrough() && tint[3] > 0.0F ? glass::adaptDensity(light, threat) : 0.0F;
    // A light/dark switch takes the new target at once; within a tone, jitter inside the deadband is ignored.
    if (light != m_light || std::abs(target - m_target) > glass::kAdaptDeadband || target == 0.0F || target == 1.0F) {
      m_light = light;
      m_target = target;
    }
    if (m_density.target() == m_target) {
      return false;
    }
    m_density.retarget(m_target, glass::kAdaptFadeMs);
    return true;
  }

} // namespace andromeda
