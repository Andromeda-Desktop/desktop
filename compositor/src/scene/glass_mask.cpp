#include "scene/glass_mask.h"

#include "wlr.h"

namespace andromeda {

  namespace {
    // Frosted digits: the blurred scene mixed toward white reads as glass on a dimmed backdrop. An opacity relative
    // to tint_regular: a 0.18 mix at the default Tint, scaled by the Tint slider like every shape.
    constexpr float kTintOpacity = 0.24F;
    constexpr float kAlphaTest = 0.5F;
  } // namespace

  void GlassMask::hide() {
    if (m_mask != nullptr) {
      wlr_scene_node_destroy(&m_mask->node);
      m_mask = nullptr;
    }
    m_serial = 0;
    m_blur.hide();
  }

  void GlassMask::update(wlr_scene_tree* tree, wlr_surface* surface, const DskSurfaceManager::State* state) {
    if (tree == nullptr || state == nullptr || state->glassMask == nullptr || wlr_box_empty(&state->glassMaskBox)) {
      hide();
      return;
    }
    if (m_mask == nullptr || m_serial != state->glassMaskSerial) {
      // A new node per mask: changing the blur's mask source damages it, a new buffer on a hidden node would not.
      if (m_mask != nullptr) {
        wlr_scene_node_destroy(&m_mask->node);
      }
      m_mask = wlr_scene_buffer_create(tree, state->glassMask);
      if (m_mask == nullptr) {
        m_blur.hide();
        return;
      }
      wlr_scene_node_set_enabled(&m_mask->node, false);
      m_serial = state->glassMaskSerial;
    }
    const wlr_box& box = state->glassMaskBox;
    const glass::Material material = configGlass(glass::Kind::Shape);
    m_blur.setMaterial(material);
    m_blur.setRim(0.0F);
    m_blur.setLens(0.0F);
    m_blur.setLive(true);
    // Reduce transparency makes the tint opaque, so the digits turn solid white.
    m_blur.setTintMix({1.0F, 1.0F, 1.0F}, glass::tintFor(material, glass::Kind::Shape, false, kTintOpacity));
    m_blur.update(
        tree, surface, box, box, corner_radii_none(), nullptr,
        SurfaceBlurOptions{.ignoreAlpha = kAlphaTest, .enabled = true, .optimized = false, .always = true}, 1.0F,
        m_mask
    );
    m_blur.raiseToTop();
  }

} // namespace andromeda
