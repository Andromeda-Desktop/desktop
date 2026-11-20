#include "scene/surface_blur.h"

#include "config/config.h"
// clang-format off
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on
#include <pixman.h>

namespace andromeda {

  namespace {
    wlr_scene_buffer* findSurfaceBuffer(wlr_scene_node& root, wlr_surface* surface) {
      struct Lookup {
        wlr_surface* surface;
        wlr_scene_buffer* buffer = nullptr;
      } lookup{surface};

      wlr_scene_node_for_each_buffer(
          &root,
          [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
            auto* lookup = static_cast<Lookup*>(data);
            if (lookup->buffer != nullptr) {
              return;
            }
            wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
            if (sceneSurface != nullptr && sceneSurface->surface == lookup->surface) {
              lookup->buffer = buffer;
            }
          },
          &lookup
      );
      return lookup.buffer;
    }

  } // namespace

  static glass::Settings configGlassSettings() {
    const Config::Appearance& a = config().appearance;
    return {
        .style = static_cast<glass::Style>(a.glassStyle),
        .tint = static_cast<float>(a.glass),
        .frost = static_cast<float>(a.frost),
        .edgeReflection = a.edgeReflection,
        .liquidTint = static_cast<float>(a.liquidTint),
        .liquidBlur = static_cast<float>(a.liquidBlur),
        .lens = static_cast<float>(a.liquid),
        .reduceTransparency = a.reduceTransparency,
    };
  }

  glass::Material configGlass(glass::Kind kind) { return glass::material(kind, configGlassSettings(), false); }

  glass::Material configGlass(glass::Kind kind, bool light) { return glass::material(kind, configGlassSettings(), light); }

  glass::Material configPopoverGlass() { return glass::popover(configGlassSettings(), false); }

  bool configGlassLight(glass::Kind kind) {
    return glass::tone(kind, configGlassSettings().style) == glass::Tone::Scheme
        && config().appearance.scheme == ColorScheme::Light;
  }

  glass::Material configWindowGlass() {
    return glass::material(glass::Kind::Sheet, glass::windowSettings(configGlassSettings()), false);
  }

  bool surfaceTransparent(wlr_surface* surface, const wlr_box& surfaceBox) {
    const pixman_box32_t box = {
        surfaceBox.x, surfaceBox.y, surfaceBox.x + surfaceBox.width, surfaceBox.y + surfaceBox.height
    };
    return pixman_region32_contains_rectangle(&surface->opaque_region, &box) != PIXMAN_REGION_IN;
  }

  void SurfaceBlur::setExactShape(const std::optional<wlr_fbox>& box, float radius) {
    m_exact = box;
    m_exactRadius = radius;
  }

  void SurfaceBlur::setMerge(const std::vector<MergedShape>& shapes, int reach) {
    m_merge = shapes;
    m_mergeReach = reach;
  }

  void SurfaceBlur::update(
      wlr_scene_tree* parent, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& surfaceBox,
      fx_corner_radii corners, const wlr_box* clipBox, const SurfaceBlurOptions& options, float surfaceOpacity,
      wlr_scene_buffer* maskSource, const pixman_region32_t* clipRegion
  ) {
    const Config::Appearance::Blur& cfg = config().appearance.blur;
    wlr_box drawBox = nodeBox;
    if (clipBox != nullptr) {
      if (!wlr_box_intersection(&drawBox, &nodeBox, clipBox) || drawBox.width <= 0 || drawBox.height <= 0) {
        if (m_node != nullptr) {
          wlr_scene_node_set_enabled(&m_node->node, false);
        }
        return;
      }
    }
    // A compositor opacity below 1 makes even a client-declared opaque surface
    // reveal the backdrop, so it also requires a blur node. Its opacity does
    // not attenuate the blur itself: client-provided alpha does not either.
    const bool want = cfg.enabled
        && options.enabled.value_or(false)
        && drawBox.width > 0
        && drawBox.height > 0
        && (options.always || surfaceOpacity < 1.0F || surfaceTransparent(surface, surfaceBox));
    if (!want) {
      if (m_node != nullptr) {
        wlr_scene_node_set_enabled(&m_node->node, false);
      }
      return;
    }

    if (m_node != nullptr && m_masked != (options.ignoreAlpha > 0.0F)) {
      wlr_scene_node_destroy(&m_node->node);
      m_node = nullptr;
    }

    if (m_node == nullptr) {
      m_node = wlr_scene_blur_create(parent, drawBox.width, drawBox.height);
      if (m_node == nullptr) {
        return;
      }
      wlr_scene_node_lower_to_bottom(&m_node->node);
      m_masked = options.ignoreAlpha > 0.0F;
    }

    wlr_scene_buffer* currentMask = wlr_scene_blur_get_transparency_mask_source(m_node);
    if (maskSource != nullptr) {
      if (currentMask != maskSource) {
        wlr_scene_blur_set_transparency_mask_source(m_node, maskSource);
      }
    } else if (currentMask == nullptr && options.ignoreAlpha > 0.0F) {
      // Only the alpha test reads the mask (fx_pass.c), and a buffer masks one blur node at a time: two nodes of one
      // surface (bar islands) would take it from each other on every update, each take a repaint.
      if (wlr_scene_buffer* mask = findSurfaceBuffer(parent->node, surface)) {
        wlr_scene_blur_set_transparency_mask_source(m_node, mask);
      }
    }

    if (m_node->ignore_alpha != options.ignoreAlpha) {
      wlr_scene_blur_set_ignore_alpha(m_node, options.ignoreAlpha);
    }

    m_optimized = options.optimized.value_or(cfg.optimized);
    wlr_scene_blur_set_should_only_blur_bottom_layer(m_node, m_optimized && !m_live);
    // Live glass samples the cached wallpaper blur in frames where only the wallpaper lies under it.
    wlr_scene_blur_set_adaptive(m_node, !m_optimized && !m_live);

    wlr_scene_node_set_enabled(&m_node->node, true);
    wlr_scene_node_set_position(&m_node->node, drawBox.x, drawBox.y);
    if (m_node->width != drawBox.width || m_node->height != drawBox.height) {
      wlr_scene_blur_set_size(m_node, drawBox.width, drawBox.height);
    }
    if (!fx_corner_radii_eq(m_node->corners, corners)) {
      wlr_scene_blur_set_corner_radii(m_node, corners);
    }
    if (m_node->alpha != m_alpha) {
      wlr_scene_blur_set_alpha(m_node, m_alpha);
    }
    wlr_scene_blur_set_liquid(m_node, m_liquid);
    wlr_scene_blur_set_rim(m_node, m_rim);
    wlr_scene_blur_set_content_scale(m_node, m_contentScale);
    wlr_scene_blur_set_lens(m_node, m_lens);
    wlr_scene_blur_set_tint_mix(m_node, m_tintRgb.data(), m_tintAmount);
    wlr_scene_blur_set_backdrop_sampling(m_node, m_backdropSampling);
    wlr_scene_blur_set_strength(m_node, glass::blurStrength(m_clarity) * m_fade);
    wlr_scene_blur_set_full_strength(m_node, glass::blurStrength(m_clarity));
    // The rim follows the client's blur region (the drawn shape), not the node box: a panel surface is larger than
    // its sheet. The radius is where the staircase first reaches the full width (a rectangle, e.g. a settings
    // sidebar: 0; the launcher pill: half its height).
    wlr_scene_blur_merge merge{};
    if (m_merge.size() >= 2) {
      merge.count = static_cast<int>(std::min<std::size_t>(m_merge.size(), 4));
      merge.reach = m_mergeReach;
      for (int i = 0; i < merge.count; ++i) {
        const MergedShape& shape = m_merge[static_cast<std::size_t>(i)];
        merge.boxes[i] = {shape.box.x - drawBox.x, shape.box.y - drawBox.y, shape.box.width, shape.box.height};
        merge.radii[i] = shape.radius;
      }
    }
    wlr_scene_blur_set_liquid_merge(m_node, &merge);
    if (clipRegion != nullptr && pixman_region32_not_empty(clipRegion)) {
      const pixman_box32_t* e = pixman_region32_extents(clipRegion);
      const wlr_box shape{e->x1 + nodeBox.x - drawBox.x, e->y1 + nodeBox.y - drawBox.y, e->x2 - e->x1, e->y2 - e->y1};
      int count = 0;
      const pixman_box32_t* rects = pixman_region32_rectangles(clipRegion, &count);
      int radius = std::min(shape.width, shape.height) / 2;
      for (int i = 0; i < count; ++i) {
        if (rects[i].x1 == e->x1 && rects[i].x2 == e->x2) {
          const int offset = rects[i].y1 - e->y1;
          radius = std::min(radius, offset > 0 ? offset + 1 : 0);
          break;
        }
      }
      wlr_scene_blur_set_liquid_shape(m_node, &shape, radius);
    } else if (m_exact.has_value() && m_merge.size() < 2) {
      const wlr_fbox shape{m_exact->x - drawBox.x, m_exact->y - drawBox.y, m_exact->width, m_exact->height};
      wlr_scene_blur_set_liquid_shape_exact(m_node, &shape, m_exactRadius);
    } else {
      wlr_scene_blur_set_liquid_shape(m_node, nullptr, 0);
    }
    wlr_scene_buffer* shapeSource = wlr_scene_blur_get_transparency_mask_source(m_node);
    const pixman_region32_t* shape =
        shapeSource != nullptr ? wlr_scene_node_get_clip_region(&shapeSource->node) : nullptr;
    pixman_region32_t clip;
    pixman_region32_init(&clip);
    if (shape != nullptr) {
      pixman_region32_copy(&clip, shape);
      int sourceX = 0;
      int sourceY = 0;
      int blurX = 0;
      int blurY = 0;
      wlr_scene_node_coords(&shapeSource->node, &sourceX, &sourceY);
      wlr_scene_node_coords(&m_node->node, &blurX, &blurY);
      pixman_region32_translate(&clip, sourceX - blurX, sourceY - blurY);
    }
    if (clipRegion != nullptr) {
      pixman_region32_t region;
      pixman_region32_init(&region);
      pixman_region32_copy(&region, clipRegion);
      pixman_region32_translate(&region, nodeBox.x - drawBox.x, nodeBox.y - drawBox.y);
      if (shape != nullptr) {
        pixman_region32_intersect(&clip, &clip, &region);
      } else {
        pixman_region32_copy(&clip, &region);
      }
      pixman_region32_fini(&region);
    }
    wlr_scene_node_set_clip_region(&m_node->node, shape != nullptr || clipRegion != nullptr ? &clip : nullptr);
    pixman_region32_fini(&clip);
  }

  void SurfaceBlur::setBackdropSampling(bool enabled) {
    m_backdropSampling = enabled;
    if (m_node != nullptr) {
      wlr_scene_blur_set_backdrop_sampling(m_node, enabled);
    }
  }

  bool SurfaceBlur::pollBackdrop(fx_backdrop_luminance* luminance) {
    return m_node != nullptr && wlr_scene_blur_poll_backdrop(m_node, luminance);
  }

  bool SurfaceBlur::backdropPending() const { return m_node != nullptr && wlr_scene_blur_backdrop_pending(m_node); }

  void SurfaceBlur::setAlpha(float alpha) {
    m_alpha = alpha;
    if (m_node != nullptr && m_node->alpha != alpha) {
      wlr_scene_blur_set_alpha(m_node, alpha);
    }
  }

  void SurfaceBlur::setLiquid(float liquid) {
    m_liquid = liquid;
    if (m_node != nullptr) {
      wlr_scene_blur_set_liquid(m_node, liquid);
    }
  }

  void SurfaceBlur::setClarity(float clarity) {
    m_clarity = clarity;
    if (m_node != nullptr) {
      wlr_scene_blur_set_strength(m_node, glass::blurStrength(clarity) * m_fade);
      wlr_scene_blur_set_full_strength(m_node, glass::blurStrength(clarity));
    }
  }

  void SurfaceBlur::setContentScale(float scale) {
    m_contentScale = scale;
    if (m_node != nullptr) {
      wlr_scene_blur_set_content_scale(m_node, scale);
    }
  }

  void SurfaceBlur::setRim(float rim) {
    m_rim = rim;
    if (m_node != nullptr) {
      wlr_scene_blur_set_rim(m_node, rim);
    }
  }

  void SurfaceBlur::setMaterial(const glass::Material& material) {
    setLiquid(material.level);
    setClarity(material.clarity);
    setRim(material.rim);
    setLens(material.lens);
  }

  void SurfaceBlur::setLens(float lens) {
    m_lens = lens;
    if (m_node != nullptr) {
      wlr_scene_blur_set_lens(m_node, lens);
    }
  }

  void SurfaceBlur::setTintMix(const std::array<float, 3>& rgb, float amount) {
    m_tintRgb = rgb;
    m_tintAmount = amount;
    if (m_node != nullptr) {
      wlr_scene_blur_set_tint_mix(m_node, rgb.data(), amount);
    }
  }

  void SurfaceBlur::setFade(float fade) {
    m_fade = fade;
    if (m_node != nullptr) {
      wlr_scene_blur_set_strength(m_node, glass::blurStrength(m_clarity) * fade);
    }
  }

  void SurfaceBlur::setLive(bool live) {
    m_live = live;
    if (m_node != nullptr) {
      wlr_scene_blur_set_should_only_blur_bottom_layer(m_node, m_optimized && !m_live);
      wlr_scene_blur_set_adaptive(m_node, !m_optimized && !m_live);
    }
  }

  void SurfaceBlur::hide() {
    if (m_node != nullptr) {
      wlr_scene_node_set_enabled(&m_node->node, false);
    }
  }

  bool SurfaceBlur::active() const { return m_node != nullptr && m_node->node.enabled; }

  wlr_scene_node* SurfaceBlur::sceneNode() const { return m_node != nullptr ? &m_node->node : nullptr; }

  void SurfaceBlur::raiseToTop() {
    if (m_node != nullptr) {
      wlr_scene_node_raise_to_top(&m_node->node);
    }
  }

} // namespace andromeda
