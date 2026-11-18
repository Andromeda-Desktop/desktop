#pragma once

#include "protocols/glass_material.h"

#include <pixman.h>
extern "C" {
#include <wlr/util/box.h>
}
#include <array>
#include <optional>
#include <vector>

struct fx_corner_radii;
struct wlr_box;
struct wlr_scene_buffer;
struct wlr_scene_blur;
struct wlr_scene_tree;
struct wlr_surface;

namespace andromeda {

  struct SurfaceBlurOptions {
    float ignoreAlpha = 0.0F;
    std::optional<bool> enabled;
    std::optional<bool> optimized;
    // Draws even over an opaque surface (a glass mask sits above the surface content).
    bool always = false;
  };

  // The glass material of compositor-drawn glass from the appearance glass settings. The tint
  // is the dark scheme's: the compositor draws no tint of its own.
  [[nodiscard]] glass::Material configGlass(glass::Kind kind);
  // The material of app windows and their overview cards (glass::windowSettings).
  [[nodiscard]] glass::Material configWindowGlass();

  // True when the surface's opaque region does not cover surfaceBox, given in surface-local coordinates.
  [[nodiscard]] bool surfaceTransparent(wlr_surface* surface, const wlr_box& surfaceBox);

  // Owns the desired-state logic for one SceneFX backdrop-blur node. The node itself is a child of the owner's scene
  // tree and is freed by scene-tree teardown, never by this class (no destructor).
  class SurfaceBlur {
  public:
    // Creates/updates/disables the node to match config and surface state. nodeBox: content box in `parent` coordinates
    // (position + size of the node). surfaceBox: content box in surface-local coordinates (opaque-region test).
    // clipBox: optional visible subset in `parent` coordinates; nullptr = full nodeBox. surfaceOpacity: opacity applied
    // to the surface buffers, used only to decide whether an opaque client reveals the blur. maskSource: optional scene
    // buffer to use instead of finding the surface's regular scene buffer. clipRegion: optional exact blur area in
    // surface-local coordinates (a client blur region); the blur draws only inside it.
    void update(
        wlr_scene_tree* parent, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& surfaceBox,
        fx_corner_radii corners, const wlr_box* clipBox = nullptr, const SurfaceBlurOptions& options = {},
        float surfaceOpacity = 1.0F, wlr_scene_buffer* maskSource = nullptr,
        const pixman_region32_t* clipRegion = nullptr
    );
    // Opacity of the blur effect itself. Keep this independent from surface
    // opacity so compositor opacity matches equivalent client alpha.
    void setAlpha(float alpha);
    // Rim refraction strength, 0 to 1 (dsk_surface_v1 liquid [0]).
    void setLiquid(float liquid);
    // How much lighter the blur is, 0 to 1 (glass::Material::clarity, dsk_surface_v1 blur [0]).
    void setClarity(float clarity);
    // Specular rim alone (frosted sheets): no refraction, no lighter blur.
    void setRim(float rim);
    // Liquid, rim and lens of a glass material.
    void setMaterial(const glass::Material& material);
    // Lens strength, 0 to 1 (dsk_surface_v1 liquid [2] since v9).
    void setLens(float lens);
    // Mixes the backdrop toward rgb by amount, 0 to 1 (glass density).
    void setTintMix(const std::array<float, 3>& rgb, float amount);
    // Shapes (parent tree coords, radius in the same px) drawn as one glass whose shapes join within `reach` px; the
    // clip box passed to update() must be their bounding box. Fewer than 2 shapes draw each shape alone.
    struct MergedShape {
      wlr_box box;
      int radius;
    };
    void setMerge(const std::vector<MergedShape>& shapes, int reach);
    // Fade of a surface, 0 to 1: it scales the blur size, so a fading surface defocuses its backdrop instead of
    // blending a blurred copy over the sharp one.
    void setFade(float fade);
    // Forces a live blur of everything below, overriding the optimized option, for a surface drawn over scene
    // content the cached background blur lacks.
    void setLive(bool live);
    // Samples the blurred backdrop on each render of the node (wlr_scene_blur_set_backdrop_sampling).
    void setBackdropSampling(bool enabled);
    // Never blocks: true with the mean luminance (0 to 1) when a sample finished since the last call.
    bool pollBackdrop(float* luminance);
    [[nodiscard]] bool backdropPending() const;
    // Puts the blur above its siblings (it is created at the bottom, under the surface).
    void raiseToTop();
    // Disable the node (unmap path); update() re-enables.
    void hide();
    // True while the blur node is shown.
    [[nodiscard]] bool active() const;

  private:
    wlr_scene_blur* m_node = nullptr;
    bool m_masked = false;
    bool m_optimized = false;
    bool m_live = false;
    float m_alpha = 1.0F;
    float m_liquid = 0.0F;
    float m_clarity = 0.0F;
    float m_rim = 0.0F;
    float m_lens = 0.0F;
    std::array<float, 3> m_tintRgb{};
    float m_tintAmount = 0.0F;
    float m_fade = 1.0F;
    bool m_backdropSampling = false;
    std::vector<MergedShape> m_merge;
    int m_mergeReach = 0;
  };

} // namespace andromeda
