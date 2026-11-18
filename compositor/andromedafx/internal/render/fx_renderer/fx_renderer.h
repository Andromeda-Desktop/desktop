#ifndef _FX_OPENGL_H
#define _FX_OPENGL_H

#include "render/fx_renderer/shaders.h"
#include "render/tracy.h"

#include <GLES2/gl2.h>
#include <stdbool.h>
#include <time.h>
#include <andromedafx/render/fx_renderer/fx_renderer.h>
#include <wlr/render/egl.h>
#include <wlr/render/interface.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_texture.h>

enum fx_gl_object {
  FX_GL_TEXTURE,
  FX_GL_FRAMEBUFFER,
  FX_GL_RENDERBUFFER,
  FX_GL_BUFFER,
  FX_GL_PROGRAM,
  FX_GL_QUERY,
  FX_GL_EGL_IMAGE,
};

struct fx_gl_garbage {
  enum fx_gl_object kind;
  GLuint name;
  EGLImageKHR image;
};
#include <wlr/util/addon.h>
#include <wlr/util/box.h>

struct fx_framebuffer;
struct wlr_allocator;

void fx_renderer_set_allocator(struct wlr_renderer* renderer, struct wlr_allocator* allocator);
bool fx_output_set_sdr_transfer_function(struct wlr_output* output, enum wlr_color_transfer_function transfer_function);
enum wlr_color_transfer_function fx_output_get_sdr_transfer_function(struct wlr_output* output);

struct fx_pixel_format {
  uint32_t drm_format;
  // optional field, if empty then internalformat = format
  GLint gl_internalformat;
  GLint gl_format, gl_type;
};

bool is_fx_pixel_format_supported(const struct fx_renderer* renderer, const struct fx_pixel_format* format);
const struct fx_pixel_format* get_fx_format_from_drm(uint32_t fmt);
const struct fx_pixel_format* get_fx_format_from_gl(GLint gl_format, GLint gl_type, bool alpha);
void get_fx_shm_formats(const struct fx_renderer* renderer, struct wlr_drm_format_set* out);
GLenum fx_resolve_gl_type(const struct fx_renderer* renderer, GLenum gl_type);
GLint fx_resolve_internal_format(const struct fx_renderer* renderer, const struct fx_pixel_format* fmt);

GLuint fx_framebuffer_get_fbo(struct fx_framebuffer* buffer);

// Creates or refreshes the sampling view in the current context. Keeps the
// active texture unit and unbinds the target after importing its EGLImage.
GLuint fx_framebuffer_get_texture(struct fx_framebuffer* buffer);
// A client committed the buffer again, so its contents may have changed: drop the shadow copy and import it live.
void fx_framebuffer_drop_shadow(struct fx_framebuffer* buffer);

/**
 * Attaches a stencil renderbuffer to the framebuffer on first use. Leaves the
 * framebuffer bound. Returns false when no stencil could be attached.
 */
bool fx_framebuffer_ensure_stencil(struct fx_framebuffer* buffer);

///
/// fx_framebuffer
///

// How a sampled client buffer reaches the GPU. Live: its dma-buf import. Copying: the import plus a queued copy into
// shadow_tex. Shadow: only shadow_tex, exact until the client commits the buffer again (clients reuse swapchain
// buffers); that commit returns it to Live.
enum fx_import_mode {
  FX_IMPORT_LIVE,
  FX_IMPORT_COPYING,
  FX_IMPORT_SHADOW,
};

struct fx_framebuffer {
  struct wlr_buffer* buffer;
  struct fx_renderer* renderer;
  struct wl_list link; // fx_renderer.buffers
  bool external_only;
  bool owned;
  // An shm staging slot: an fx_texture keeps its GL name in slot_tex, so the idle trim never drops its import.
  bool staging;
  uint32_t drm_format;
  // Non-NULL when this is a swapchain target using output-local scratch buffers
  struct fx_offscreen_buffers* output_buffers;
  uint64_t output_generation;
  struct fx_framebuffer* blend_buffer;
  struct fx_framebuffer* blend_parent;
  struct fx_framebuffer* sdr_capture_buffer;
  struct fx_framebuffer* sdr_capture_parent;
  bool capture_sdr;
  bool sdr_capture_valid;
  // Encoding of the SDR capture: the sdr_tf of the pass that rendered this buffer
  enum wlr_color_transfer_function sdr_capture_tf;
  // Unfiltered composition of this swapchain buffer while a capture is pending
  // with effects excluded from captures. dmabuf imports read it instead.
  struct fx_framebuffer* effect_capture_buffer;
  struct fx_framebuffer* effect_capture_parent;
  bool effect_capture_valid;
  const void* effect_capture_owner;

  // A buffer we only sample (not owned, no fbo) loses its import after FX_CLIENT_IMPORT_IDLE_MS unused: image is
  // then EGL_NO_IMAGE_KHR and tex 0, and the next use imports it again.
  int64_t used_ms;
  enum fx_import_mode import_mode;
  GLuint shadow_tex;
  size_t shadow_bytes;

  EGLImageKHR image;
  GLuint rbo;
  GLuint fbo;
  GLuint tex;
  GLuint sb; // Stencil

  struct wlr_addon addon;
};

/**
 * Should only be used with custom fbs.
 * Note: Does not bind back to the default Framebuffer!
 */
void fx_framebuffer_get_or_create_custom(
    struct fx_renderer* fx_renderer, struct wlr_allocator* allocator, int width, int height, uint32_t format,
    struct fx_framebuffer** fx_buffer, bool* failed
);

struct fx_framebuffer* fx_framebuffer_get_or_create(struct fx_renderer* renderer, struct wlr_buffer* wlr_buffer);

void fx_framebuffer_bind(struct fx_framebuffer* buffer);

/**
 * Destroy the fx_framebuffer.
 * Note: Doesn't drop the wlr_buffer, so should only be used internally.
 */
void fx_framebuffer_destroy(struct fx_framebuffer* buffer);

struct fx_renderer;
// Deletes a GL object now, or once the render thread returns the context. Binds the context itself; 0 is a no-op.
void fx_renderer_delete_gl(struct fx_renderer* renderer, enum fx_gl_object kind, GLuint name);
void fx_renderer_delete_egl_image(struct fx_renderer* renderer, EGLImageKHR image);
// Deletes what was queued while the context was lent. Call with the context Home.
void fx_renderer_collect_gl_garbage(struct fx_renderer* renderer);
// Blocks until the render thread is done with the context, for synchronous GL outside an output frame (capture and
// cursor passes, readback). False when it is still away after FX_RENDER_THREAD_SYNC_TIMEOUT_MS: the caller fails.
// Output frames never start while it is away: output.cpp waits for the context.
bool fx_renderer_take_context(struct fx_renderer* renderer);
// Uploads the textures created while the context was lent. Call with the context Home.
void fx_renderer_upload_pending_textures(struct fx_renderer* renderer);
// Frees the spare slots of each texture no client wrote for `idle_ms`. Returns true while a texture keeps a spare.
bool fx_renderer_trim_shm_slots(struct fx_renderer* renderer, int64_t now_ms, int64_t idle_ms);
// On xe each GPU submit locks every imported dma-buf of the context, so a client that holds its buffer lock stalls
// our frames. Each client buffer unused for `idle_ms` (hidden windows, idle swapchain buffers) is copied into a shadow
// texture and then loses its import, so drawing it again never locks the client's buffer. Without budget room the
// import is dropped with no shadow and made again on the next draw. Returns true while an import is in use or a copy
// is in flight.
bool fx_renderer_trim_client_imports(struct fx_renderer* renderer, int64_t now_ms, int64_t idle_ms);
#define FX_CLIENT_IMPORT_IDLE_MS 2000
// A full-screen 2880x1800 window is 20.7 MB; over the budget an idle import is dropped without a shadow.
#define FX_SHADOW_BUDGET_BYTES ((size_t)512 << 20)

// Drops the buffer's effect capture and clears its owner.
void fx_framebuffer_release_effect_capture(struct fx_framebuffer* buffer);

// Draws all of `source` over `target` in a pass of its own, decoding `source`
// with `transfer_function` and encoding `target` with `target_tf`. Leaves no
// framebuffer bound. The pass takes `target_tf` as its sdr_tf, so an unset
// `transfer_function` copies without conversion. Explicit transfer functions
// retain their declared meaning.
bool fx_framebuffer_copy(
    struct fx_framebuffer* target, struct fx_framebuffer* source, enum wlr_color_transfer_function transfer_function,
    enum wlr_color_transfer_function target_tf
);

///
/// fx_texture
///

#define FX_SHM_SLOTS 3
// A texture no client wrote for this long keeps one slot: no frame still samples it, so the next write goes in place.
#define FX_SHM_SLOT_IDLE_MS 1000

struct fx_texture {
  struct wlr_texture wlr_texture;
  struct fx_renderer* fx_renderer;
  struct wl_list link; // fx_renderer.textures

  GLuint target;

  // If this texture is imported from a buffer, the texture is does not own
  // these states. These cannot be destroyed along with the texture in this
  // case.
  GLuint tex;
  GLuint fbo;

  bool has_alpha;

  uint32_t drm_format;           // for mutable textures only, used to interpret upload data
  struct fx_framebuffer* buffer; // for DMA-BUF imports only
  struct wlr_buffer* locked_buffer;

  // Large shm buffers rotate through up to FX_SHM_SLOTS linear staging buffers
  // the GPU samples in place (fx_texture.c struct shm_staging). A slot the
  // display still scans out holds a lock and is never written.
  struct wlr_buffer* slots[FX_SHM_SLOTS];
  GLuint slot_tex[FX_SHM_SLOTS];
  pixman_region32_t slot_stale[FX_SHM_SLOTS]; // damage each slot has not received yet
  int slot;
  bool slot_failed; // a slot could not be created; stop adding slots
  int64_t slot_written_ms; // CLOCK_MONOTONIC time of the last slot write

  // Created while the render thread held the context: the buffer stays locked (a shm client gets its release only
  // after the upload) and is uploaded when the context returns, before any pass can sample it.
  struct wlr_buffer* pending_upload;
  struct wl_list pending_link; // fx_renderer.pending_textures
};

struct fx_texture* fx_get_texture(struct wlr_texture* wlr_texture);

void fx_texture_destroy(struct fx_texture* texture);

// The staging slot that holds the texture's current pixels, as a linear
// dmabuf for direct scanout; NULL when the texture has no staging slots.
struct wlr_buffer* fx_texture_get_scanout_buffer(struct wlr_texture* wlr_texture);

bool wlr_texture_is_fx(struct wlr_texture* wlr_texture);

bool wlr_renderer_is_fx(struct wlr_renderer* wlr_renderer);

struct fx_render_timer* fx_get_render_timer(struct wlr_render_timer* timer);

struct fx_texture* fx_get_texture(struct wlr_texture* wlr_texture);

struct wlr_renderer* fx_renderer_create_egl(struct wlr_egl* egl);

struct wlr_egl* wlr_fx_renderer_get_egl(struct wlr_renderer* renderer);

void push_fx_debug_(struct fx_renderer* renderer, const char* file, const char* func);

#define push_fx_debug(renderer) push_fx_debug_(renderer, _WLR_FILENAME, __func__)

void pop_fx_debug(struct fx_renderer* renderer);

///
/// Render Timer
///

struct fx_render_timer {
  struct wlr_render_timer base;
  struct fx_renderer* renderer;
  struct timespec cpu_start;
  struct timespec cpu_end;
  GLuint id;
  GLuint begin_id;
  GLint64 gl_cpu_end;
  // Set by the duration read: first to last GPU timestamp of the pass.
  int64_t gpu_exec_ns;
};

bool wlr_render_timer_is_fx(struct wlr_render_timer* timer);

///
/// fx_renderer
///

/**
 * OpenGL ES 2 renderer.
 *
 * Care must be taken to avoid stepping each other's toes with EGL contexts:
 * the current EGL is global state. The GLES2 renderer operations will save
 * and restore any previous EGL context when called. A render pass is seen as
 * a single operation.
 *
 * The GLES2 renderer doesn't support arbitrarily nested render passes. It
 * supports a subset only: after a nested render pass is created, any parent
 * render pass can't be used before the nested render pass is submitted.
 */

struct fx_renderer {
  struct wlr_renderer wlr_renderer;

  // Null until fx_renderer_start_render_thread; submits then flush on the calling thread.
  struct fx_render_thread* render_thread;
  // Sum of fx_framebuffer.shadow_bytes, at most FX_SHADOW_BUDGET_BYTES.
  size_t shadow_bytes;
  // Set around an output frame's build: its submits wait for fx_renderer_kick. Other passes submit before returning.
  bool defer_submits;
  // GL objects released while the render thread held the context, deleted when it returns.
  struct fx_gl_garbage* gl_garbage;
  size_t gl_garbage_len, gl_garbage_cap;

  struct wlr_egl* egl;
  int drm_fd;
  struct wlr_allocator* allocator;

  // GL timestamp minus CLOCK_MONOTONIC (ns), resynced once per second: a GL_TIMESTAMP read is a register read that
  // spins on i915 forcewake, too slow for every frame.
  int64_t gl_clock_offset_ns;
  int64_t gl_clock_synced_ns;
  // KHR_debug output and debug groups: ANDROMEDA_GL_DEBUG or a debug log level.
  bool gl_debug;

  struct wlr_drm_format_set shm_texture_formats;

  bool is_gles3;

  const char* exts_str;
  struct {
    bool EXT_read_format_bgra;
    bool KHR_debug;
    bool OES_egl_image_external;
    bool OES_egl_image;
    bool EXT_texture_type_2_10_10_10_REV;
    bool OES_texture_half_float_linear;
    bool half_float_linear;
    bool EXT_texture_norm16;
    bool EXT_disjoint_timer_query;
  } exts;

  struct {
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNGLDEBUGMESSAGECALLBACKKHRPROC glDebugMessageCallbackKHR;
    PFNGLDEBUGMESSAGECONTROLKHRPROC glDebugMessageControlKHR;
    PFNGLPOPDEBUGGROUPKHRPROC glPopDebugGroupKHR;
    PFNGLPUSHDEBUGGROUPKHRPROC glPushDebugGroupKHR;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES;
    PFNGLGETGRAPHICSRESETSTATUSKHRPROC glGetGraphicsResetStatusKHR;
    PFNGLGENQUERIESEXTPROC glGenQueriesEXT;
    PFNGLDELETEQUERIESEXTPROC glDeleteQueriesEXT;
    PFNGLQUERYCOUNTEREXTPROC glQueryCounterEXT;
    PFNGLGETQUERYOBJECTIVEXTPROC glGetQueryObjectivEXT;
    PFNGLGETQUERYOBJECTUI64VEXTPROC glGetQueryObjectui64vEXT;
    PFNGLGETINTEGER64VEXTPROC glGetInteger64vEXT;
    TRACY_FN(PFNGLGETQUERYIVEXTPROC glGetQueryivEXT;)
  } procs;

  struct {
    struct quad_shader quad;
    struct quad_shader quad_clip;
    struct quad_grad_shader quad_grad;
    struct quad_round_shader quad_round;
    struct border_shader border;
    struct quad_grad_round_shader quad_grad_round;

    struct tex_shader tex_rgba;
    struct tex_shader tex_rgbx;
    struct tex_shader tex_ext;
    struct tex_shader tex_effects_rgba;
    struct tex_shader tex_effects_rgbx;
    struct tex_shader tex_effects_ext;
    struct tex_shader tex_clamp_rgba;
    struct tex_shader tex_clamp_rgbx;
    struct tex_shader tex_clamp_ext;
    struct tex_shader tex_clamp_effects_rgba;
    struct tex_shader tex_clamp_effects_rgbx;
    struct tex_shader tex_clamp_effects_ext;
    struct output_shader output;

    struct box_shadow_shader box_shadow;
    struct blur_shader blur1;
    struct blur_shader blur2;
    struct blur2_with_effects_shader blur2_with_effects;
    struct blur2_composite_shader blur2_composite;
    struct blur2_composite_shader blur2_composite_corners;
    struct blur2_composite_shader blur2_with_effects_composite;
    struct blur2_composite_shader blur2_with_effects_composite_corners;
    // CORNER_FLARES variants, for glass with negative corner radii (joined shapes).
    struct tex_shader tex_effects_flares_rgba;
    struct blur2_composite_shader blur2_composite_flares;
    struct blur2_composite_shader blur2_with_effects_composite_flares;
    struct blur2_composite_shader blur2_glass_composite;
    struct blur2_composite_shader blur2_glass_composite_flares;
    struct blur2_composite_shader blur2_with_effects_glass_composite;
    struct blur2_composite_shader blur2_with_effects_glass_composite_flares;
  } shaders;

  bool animation_shadow_attempted;
  // Each effect copy failure is logged once per renderer.
  bool in_place_copy_failure_logged;
  bool effect_capture_failure_logged;
  // Set only through the matching fx_renderer_fail_*_for_test functions.
  bool fail_target_copies_for_test;
  bool fail_effect_capture_for_test;
  bool fail_animation_capture_for_test;
  // Set only through fx_renderer_split_blur_composite_for_test.
  bool split_blur_composite_for_test;
  // Set only through fx_renderer_whole_corner_textures_for_test.
  bool whole_corner_textures_for_test;
  struct fx_effect_shader* animation_shadow_horizontal;
  struct fx_effect_shader* animation_shadow_vertical;

  // Linked on first use; a failed link is not retried.
  bool effect_light_attempted;
  GLuint effect_light_program;
  GLint effect_light_proj, effect_light_tex_proj, effect_light_pos, effect_light_tex, effect_light_gain,
      effect_light_linear, effect_light_emission, effect_light_source_linear, effect_light_threshold,
      effect_light_source_region;

  struct wl_list buffers;           // fx_framebuffer.link
  struct wl_list textures;          // fx_texture.link
  struct wl_list pending_textures;  // fx_texture.pending_link
  struct wl_list backdrop_probes;   // fx_backdrop_probe.link
  struct wl_list offscreen_buffers; // fx_offscreen_buffers.link
  struct wl_list output_luts;       // fx_output_lut.link

  TRACY_FN(struct tracy_data* tracy_data;)
};

struct fx_output_lut {
  struct wlr_addon addon;
  struct wl_list link; // fx_renderer.output_luts
  struct fx_renderer* renderer;
  GLuint texture;
  float dim;
};

void fx_output_lut_destroy(struct fx_output_lut* lut);

#endif
