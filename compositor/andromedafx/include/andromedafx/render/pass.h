#ifndef SCENE_FX_RENDER_PASS_H
#define SCENE_FX_RENDER_PASS_H

#include "render/egl.h"
#include "render/tracy.h"
#include "types/fx/clipped_region.h"

#include <GLES2/gl2.h>
#include <stdbool.h>
#include <andromedafx/render/effect.h>
#include <andromedafx/types/fx/blur_data.h>
#include <wlr/render/color.h>
#include <wlr/render/interface.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>

struct fx_gles_render_pass {
  struct wlr_render_pass base;
  struct fx_framebuffer* buffer;
  struct fx_framebuffer* output_buffer;
  float projection_matrix[9];
  struct wlr_egl_context prev_ctx;
  struct fx_render_timer* timer;
  struct wlr_drm_syncobj_timeline* signal_timeline;
  uint64_t signal_point;
  // A client reads the target right after submit (capture destinations).
  bool implicit_sync_target;
  bool has_color_transform;
  struct wlr_color_transform* color_transform;
  struct fx_offscreen_buffers* output_buffers;
  float output_matrix[9];
  enum wlr_color_transfer_function output_tf;
  // Curve of SDR content (untagged or sRGB-tagged buffers, compositor colors)
  // in this pass. A PQ or gamma 2.2 target decodes it with gamma 2.2, the curve
  // SDR content is authored for; any other target keeps sRGB. Without a color
  // transform it is also the target encoding.
  enum wlr_color_transfer_function sdr_tf;
  GLuint output_lut;
  float output_lut_dim;
  pixman_region32_t updated_region;
  // A new or resized shared blend buffer needs every output pixel initialized.
  bool needs_full_damage;
  // Set while an add_* call renders into an offscreen buffer instead of the
  // pass target, so it must not extend updated_region.
  bool suppress_updated;
  // The pass target while the optimized blur source draws into its own buffer, else NULL.
  struct fx_framebuffer* optimized_source_parent;
  bool optimized_source_suppress;
  // Reject the frame instead of displaying a partially applied animation.
  bool require_animation_success;
  bool animation_failed;

  // The region where there's blur
  pixman_region32_t blur_padding_region;
  bool has_blur;
  // Contains output-specific framebuffers.
  // NULL when no advanced effects like blur is being used in the current pass.
  // Call `fx_render_pass_init_offscreen_buffers` to use advanced effects.
  struct fx_offscreen_buffers* fx_offscreen_buffers;
  unsigned animation_depth;
  // Between fx_render_pass_begin_shared_blur and its end: one blur, and the sharp backdrop for lenses, of a run of
  // live blur nodes at `strength`.
  struct {
    bool active;
    float strength;
    struct fx_framebuffer* blur;
    struct fx_framebuffer* sharp;
    // Set: `blur` holds the half-resolution level and each node does the final upsample in its composite.
    bool fused;
    struct blur_data final_upsample;
  } shared_blur;
  struct wlr_texture* workspace_incoming; // bound only while compositing a workspace pair
  // A scratch backdrop beneath an isolated subtree, excluded from composition.
  unsigned isolation_depth;
  bool isolation_content;
  struct fx_framebuffer* animation_parents[FX_ANIMATION_DEPTH];
  struct wlr_texture* animation_textures[FX_ANIMATION_DEPTH];
  bool animation_suppress[FX_ANIMATION_DEPTH];
  struct wlr_box animation_boxes[FX_ANIMATION_DEPTH];
  // One past the capture index of the open group capture, or 0. Captures
  // inside it allocate from the offscreen group set.
  unsigned group_depth;
  bool group_used;
  struct wl_list animation_history_updates;
  // The target's unfiltered composition was copied into the output buffer's
  // effect capture; it becomes readable once the pass submits.
  bool effect_capture_saved;
#ifdef TRACY_ENABLE
  // Spans the pass's GPU work from begin to the end of submit's commands.
  struct tracy_gpu_zone_context frame_gpu_zone;
#endif
};

bool fx_render_pass_begin_animation(struct fx_gles_render_pass* pass);
bool fx_render_pass_begin_isolation(struct fx_gles_render_pass* pass);
void fx_render_pass_cancel_isolation(struct fx_gles_render_pass* pass);
// Opens the pass's single group capture. box is in parent pixels; translate
// draws into the capture before end_capture.
bool fx_render_pass_begin_capture(struct fx_gles_render_pass* pass, const struct wlr_box* box);
void fx_render_pass_end_capture(
    struct fx_gles_render_pass* pass, const struct wlr_box* box, const pixman_region32_t* clip
);
// `box` and `logical_box` are the node's boxes; `expand` (logical px) grows the
// drawn rectangle on every side. `uv` in the program spans the drawn rectangle.
void fx_render_pass_end_animation(
    struct fx_gles_render_pass* pass, struct fx_effect_shader* shader, const struct fx_animation_parameters* parameters,
    const struct wlr_box* box, const struct wlr_box* logical_box, enum wl_output_transform transform,
    const pixman_region32_t* clip, int expand
);

// Consume the current capture as a shadow caster. Always restores the parent
// target, including on allocation failure. False requests the analytic fallback.
bool fx_render_pass_end_animation_shadow(
    struct fx_gles_render_pass* pass, float softness, float offset_x, float offset_y, const float color[4],
    const pixman_region32_t* clip
);

struct fx_gradient {
  float degree;
  /* The full area the gradient fit too, for borders use the window size */
  struct wlr_box range;
  /* The center of the gradient, {0.5, 0.5} for normal*/
  float origin[2];
  /* 1 = Linear, 2 = Conic */
  int linear;
  /* Whether or not to blend the colors */
  int blend;
  int count;
  float* colors;
};

#define FX_LIQUID_MERGE_MAX 4

// Glass shapes drawn as one: a smooth union with blend reach `reach` (render px).
struct fx_liquid_merge {
  int count;
  struct wlr_box boxes[FX_LIQUID_MERGE_MAX];
  float radii[FX_LIQUID_MERGE_MAX];
  float reach;
};

struct fx_render_texture_options {
  struct wlr_render_texture_options base;
  const struct wlr_box* clip_box; // Used to clip csd. Ignored if NULL
  struct fx_corner_fradii corners;
  float discard_transparent; // discard pixels with alpha < this; 0.0 discards nothing
  struct clipped_fregion clipped_region;
  // Texels the draw may sample, in buffer coordinates. A source box snapped to
  // whole texels can reach past the region a cropped surface actually owns;
  // this keeps sampling inside it by duplicating its edge texel. Empty draws
  // with the regular shader and no sampling clamp at all; only pass a box when
  // the source box actually reaches texels outside it.
  struct wlr_fbox sample_box;
  // Optional primary luminances, used by transfer functions whose shape
  // depends on the display black and white levels (BT.1886).
  const struct wlr_color_luminances* luminances;
  // Compress HDR highlights into the SDR range before blending this content.
  // Intermediate textures leave this unset so a scene is not mapped twice.
  bool map_hdr_to_sdr;
  // Liquid glass strength, 0 to 1; draws with the effects shader. Used for blurred backdrops.
  float liquid;
  // Specular rim only, 0 to 1 (frosted shapes); the shader draws the larger of rim and liquid.
  float rim;
  // Lens strength, 0 to 1: the sharp backdrop shows through the rim band (needs lens_texture).
  float lens;
  // Glass body look before the tint: {saturation gain, dim}; 0 = none. Saturation 1 + gain, brightness 1 - dim.
  float vibrancy[2];
  // Backdrop pre-mix toward the tint colour: rgb, a = amount 0 to 1 (glass density over a same-tone backdrop).
  float tint_mix[4];
  // Sharp backdrop for the liquid lens, same size and orientation as texture; NULL = no lens.
  struct wlr_texture* lens_texture;
  // A blur's sharp source, same size and orientation as texture, mixed under it by source_mix (0 to 1).
  struct wlr_texture* source_texture;
  float source_mix;
  // The liquid shape (render coords, radius in render px) whose edge gets the rim; empty = clip_box and corners.
  struct wlr_box liquid_box;
  // Render px added to liquid_box x, y, width, height in the shader: a sub-pixel shape.
  float liquid_frac[4];
  float liquid_radius;
  // Radians clockwise of the liquid shape about its centre; liquid_box bounds the rotated shape.
  float liquid_rotation;
  // The two negative corners of one edge flare across that edge, not along it (corner_alpha.frag flare_across).
  bool flare_across;
  // Render px per logical px for the rim band; 0 = 1.
  float liquid_scale;
  // Pointer key light: x, y in liquid_box coords, reach in render px (0: the fixed top-left light).
  float light[3];
  // Merged glass: count >= 2 replaces liquid_box with the smooth union of these shapes (render coords), which also
  // gives the coverage; clip_box is their bounding box.
  struct fx_liquid_merge merge;
};

struct fx_render_rect_options {
  struct wlr_render_rect_options base;
  struct clipped_fregion clipped_region;
};

struct fx_render_rect_grad_options {
  struct wlr_render_rect_options base;
  struct fx_gradient gradient;
};

struct fx_render_rounded_rect_options {
  struct wlr_render_rect_options base;
  struct fx_corner_fradii corners;
  struct clipped_fregion clipped_region;
  // Alpha fall-off from fade_from to fade_to (render coords): 0 flat, 1 clear at fade_to; steepest near fade_from.
  float fade;
  float fade_from[2];
  float fade_to[2];
};

struct fx_render_border_options {
  struct wlr_box box;
  const pixman_region32_t* clip;
  struct clipped_fregion clipped_region;
  struct fx_corner_fradii seam_corners;
  struct fx_corner_fradii outer_corners;
  float inner_width;
  float outer_width;
  struct wlr_render_color inner_color;
  struct wlr_render_color outer_color;
};

struct fx_render_rounded_rect_grad_options {
  struct wlr_render_rect_options base;
  struct fx_gradient gradient;
  struct fx_corner_fradii corners;
};

struct fx_render_box_shadow_options {
  struct wlr_box box;
  struct clipped_fregion clipped_region;
  /* Clip region, leave NULL to disable clipping */
  const pixman_region32_t* clip;

  float blur_sigma;
  int corner_radius;
  struct wlr_render_color color;
};

struct fx_backdrop_probe;

struct fx_render_blur_pass_options {
  struct fx_render_texture_options tex_options;
  struct fx_framebuffer* current_buffer;
  struct blur_data* blur_data;
  bool use_optimized_blur;
  float ignore_alpha; // 0.0 = blur whole box; >0 = skip pixels with mask alpha < value
  float blur_strength;
  // The strength a fade scales toward (0 = no fade): a node fading at a lower strength mixes stored levels.
  float blur_full_strength;
  struct fx_corner_fradii corners;
  struct clipped_fregion clipped_region;
  float liquid; // 0 to 1, see fx_render_texture_options.liquid
  float rim;    // 0 to 1, see fx_render_texture_options.rim
  float lens;   // 0 to 1, see fx_render_texture_options.lens
  float vibrancy[2]; // see fx_render_texture_options.vibrancy
  float tint_mix[4]; // see fx_render_texture_options.tint_mix
  struct wlr_box liquid_box;
  float liquid_frac[4]; // see fx_render_texture_options.liquid_frac
  float liquid_radius;
  float liquid_rotation; // see fx_render_texture_options.liquid_rotation
  bool flare_across;     // see fx_render_texture_options.flare_across
  float liquid_scale;
  float light[3]; // see fx_render_texture_options.light
  struct fx_liquid_merge merge; // see fx_render_texture_options.merge
  struct fx_backdrop_probe* backdrop_probe; // optional: sampled from the blurred region, see backdrop_probe.h
  bool probe_only; // samples backdrop_probe and draws nothing
  // The part of the glass this output shows (buffer coords), which backdrop_probe averages; probe_redrawn: this frame's
  // damage covers all of it, so the blurred level is fresh there.
  struct wlr_box probe_box;
  bool probe_redrawn;
  // An optimized blur under alpha 1 draws opaque: its sharp source mixed with the blur by alpha.
  bool fade_from_source;
};

struct fx_gles_render_pass* fx_get_render_pass(struct wlr_render_pass* render_pass);

/**
 * Attaches the output's offscreen buffer set to the render pass. The
 * individual buffers are allocated when an effect first needs them.
 */
bool fx_render_pass_init_offscreen_buffers(struct wlr_render_pass* render_pass, struct wlr_output* output);

/**
 * Returns the buffer holding the pixels saved around blurred regions,
 * allocating it on first use. NULL without offscreen buffers or when the
 * allocation failed.
 */
struct fx_framebuffer* fx_render_pass_blur_saved_pixels_buffer(struct fx_gles_render_pass* pass);

/**
 * Render a fx texture.
 */
void fx_render_pass_add_texture(
    struct fx_gles_render_pass* render_pass, const struct fx_render_texture_options* options
);

/**
 * Render a rectangle.
 */
void fx_render_pass_add_rect(struct fx_gles_render_pass* render_pass, const struct fx_render_rect_options* options);

/**
 * Render a rectangle with a gradient.
 */
void fx_render_pass_add_rect_grad(
    struct fx_gles_render_pass* render_pass, const struct fx_render_rect_grad_options* options
);

/**
 * Render a rounded rectangle.
 */
void fx_render_pass_add_rounded_rect(
    struct fx_gles_render_pass* render_pass, const struct fx_render_rounded_rect_options* options
);

/**
 * Render a two-color rounded border.
 */
void fx_render_pass_add_border(struct fx_gles_render_pass* render_pass, const struct fx_render_border_options* options);

/**
 * Render a rounded rectangle with a gradient.
 */
void fx_render_pass_add_rounded_rect_grad(
    struct fx_gles_render_pass* render_pass, const struct fx_render_rounded_rect_grad_options* options
);

/**
 * Render a box shadow.
 */
void fx_render_pass_add_box_shadow(
    struct fx_gles_render_pass* pass, const struct fx_render_box_shadow_options* options
);

/**
 * Render blur.
 */
void fx_render_pass_add_blur(struct fx_gles_render_pass* pass, struct fx_render_blur_pass_options* fx_options);

/**
 * Blurs `clip` (buffer coordinates) once at `strength` for a run of live blur nodes drawn one after another, and
 * copies the sharp backdrop inside `lens_region` (may be empty) for their lenses. Until
 * fx_render_pass_end_shared_blur, fx_render_pass_add_blur composites live blurs of that strength from this result.
 * False, with nothing shared, inside a capture or an animation, or when the blur or the copy fails.
 */
bool fx_render_pass_begin_shared_blur(
    struct fx_gles_render_pass* pass, const pixman_region32_t* clip, struct blur_data* blur_data, float strength,
    pixman_region32_t* lens_region
);
void fx_render_pass_end_shared_blur(struct fx_gles_render_pass* pass);

/**
 * Render optimized blur. False when the blur or its copies fail, which leaves
 * the cache stale.
 */
bool fx_render_pass_add_optimized_blur(
    struct fx_gles_render_pass* pass, struct fx_render_blur_pass_options* fx_options
);

/**
 * Whether the optimized blur must be rendered again before it is drawn: its
 * last render did not complete, or has another size or format than the pass,
 * or used another SDR curve, or encoded values where the pass works in linear
 * light or the other way round.
 */
bool fx_render_pass_optimized_blur_stale(const struct fx_gles_render_pass* pass);

/**
 * Binds the optimized blur's sharp buffer as the pass target, so the scene can draw the cache's source into it
 * without what covers it on screen. Pass the result as current_buffer to fx_render_pass_add_optimized_blur, then call
 * fx_render_pass_end_optimized_source. NULL inside a capture or on allocation failure.
 */
struct fx_framebuffer* fx_render_pass_begin_optimized_source(struct fx_gles_render_pass* pass);
void fx_render_pass_end_optimized_source(struct fx_gles_render_pass* pass);

/** True when the optimized blur cache holds a valid blur for this pass, so a partial re-render can update it. */
bool fx_render_pass_optimized_blur_reusable(const struct fx_gles_render_pass* pass);

/**
 * Render from one buffer to another. False when the source cannot be sampled.
 */
bool fx_render_pass_read_to_buffer(
    struct fx_gles_render_pass* pass, pixman_region32_t* region, struct fx_framebuffer* dst_buffer,
    struct fx_framebuffer* src_buffer
);

#endif
