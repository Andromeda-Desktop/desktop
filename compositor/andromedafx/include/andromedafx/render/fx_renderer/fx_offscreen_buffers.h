#ifndef _FX_OFFSCREEN_BUFFERS_H
#define _FX_OFFSCREEN_BUFFERS_H

#include <stdbool.h>
#include <stdint.h>
#include <andromedafx/render/effect.h>
#include <andromedafx/types/fx/blur_data.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>

// Scratch buffers sized to the current render target.
struct fx_target_buffers {
  // Blur swaps between the two effects buffers every time it scales the image
  // Buffer used for effects
  struct fx_framebuffer* effects_buffer;
  // Swap buffer used for effects
  struct fx_framebuffer* effects_buffer_swapped;
  // Copy of the target under an in-place effect, which the program samples
  struct fx_framebuffer* in_place_source;
  // Sharp backdrop under liquid shapes, copied per shape rectangle for the lens
  struct fx_framebuffer* lens_source;
  struct fx_framebuffer* animation_buffers[FX_ANIMATION_DEPTH];
  struct fx_framebuffer* animation_backdrop;
};

/**
 * Per-output renderer framebuffers. Each buffer is allocated on first use and
 * kept until it is no longer needed or the output goes away.
 */
struct fx_offscreen_buffers {
  struct wl_list link; // fx_renderer.offscreen_buffers
  struct wlr_addon addon;
  struct fx_renderer* renderer;
  // Allocator of the output the buffers belong to
  struct wlr_allocator* allocator;

  // Nearest-filtered spectrum, allocated only for an audio-reading shader.
  unsigned audio_texture;
  uint8_t audio_bands[FX_AUDIO_BANDS];

  // Linear-light composition target shared by the output swapchain
  struct fx_framebuffer* blend_buffer;
  bool blend_valid;
  uint64_t blend_generation;
  // sdr_tf the blend buffer was last composited with
  enum wlr_color_transfer_function blend_sdr_tf;
  // SDR view shared by capture of an HDR output, encoded with the pass sdr_tf
  struct fx_framebuffer* sdr_capture_buffer;
  uint64_t sdr_capture_generation;

  // Contains the blurred background for tiled windows
  struct fx_framebuffer* optimized_blur_buffer;
  // Whether the optimized blur buffers hold a completed render
  bool optimized_blur_valid;
  // Pixels the optimized blur cache has re-rendered; read by tests
  uint64_t optimized_blur_pixels;
  // sdr_tf the optimized blur buffers were rendered in linear light with, or 0
  // when they hold encoded values
  enum wlr_color_transfer_function optimized_blur_sdr_tf;
  // Contains the non-blurred background for tiled windows. Used for blurring
  // optimized surfaces with an alpha. Just as inefficient as the regular blur.
  struct fx_framebuffer* optimized_no_blur_buffer;
  // Bumped each time the optimized blur buffers are re-rendered or replaced.
  uint64_t optimized_blur_generation;
  uint64_t optimized_blur_rebuilt_frame; // `frame` at the last generation bump
  // optimized_no_blur_buffer blurred at one strength (below 1) and blur data per slot, inside the slot's boxes, valid
  // while the slot generation equals optimized_blur_generation: glass over a still backdrop composites it instead of
  // blurring again each frame. One slot per strength, so a whole-output render of one never evicts another. A partial
  // rebuild drops only the boxes whose blur reach it touches.
  struct fx_strength_blur {
    struct fx_framebuffer* buffer;
    uint64_t generation;
    uint64_t last_used; // strength_blur_clock at the last lookup; the least recent slot is reused
    uint64_t since; // frame at which the slot took this strength
    bool held; // looked up in a frame after since: a settled strength, not one step of a fade
    float strength;
    struct blur_data data;
    struct wlr_box boxes[32]; // more than the glass boxes of one frame (launcher: 7), or boxes evict in turn
    size_t count;
  } strength_blurs[3];
  uint64_t strength_blur_clock;
  // Box shadow shapes drawn once and then copied: the analytic shader takes 4 erf samples per pixel, and a panel of
  // tiles draws ~0.5 MP of shadow per frame. An entry is filled when its key repeats within two passes, so a shadow that
  // changes size every frame never fills one. Alpha is not in the key: the copy multiplies it.
  struct fx_shadow_cache {
    struct fx_shadow_key {
      int width, height, radius;
      float sigma;
      float rgb[3];
      struct wlr_box cut; // clipped region relative to the shadow box
      float cut_radii[4];
      bool linear;
    } key;
    struct fx_framebuffer* buffer;
    bool filled;
    uint64_t last_used; // frame
  } shadows[32];
  uint64_t frame; // output render passes begun
  // Contains the original pixels to draw over the areas where artifact are visible
  struct fx_framebuffer* blur_saved_pixels_buffer;
  // Sized to the output
  struct fx_target_buffers output;
  // Sized to a group capture and everything nested in it, so a group capture
  // never resizes the output set. Dropped after a pass that uses none.
  struct fx_target_buffers group;
};

void fx_offscreen_buffers_destroy(struct fx_offscreen_buffers* fbos);
void fx_offscreen_buffers_clear_group(struct fx_offscreen_buffers* fbos);
struct fx_offscreen_buffers* fx_offscreen_buffers_try_get(struct wlr_output* output);
void fx_offscreen_buffers_invalidate_blend(struct wlr_output* output);
void fx_renderer_clear_animation_buffers(struct wlr_output* output);

#endif
