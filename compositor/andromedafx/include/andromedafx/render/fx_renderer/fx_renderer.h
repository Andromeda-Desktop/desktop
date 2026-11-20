#ifndef ANDROMEDAFX_RENDER_FX_RENDERER_H
#define ANDROMEDAFX_RENDER_FX_RENDERER_H

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wlr/backend.h>
#include <wlr/render/interface.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_buffer.h>

struct fx_renderer;
struct wlr_output;

struct wlr_renderer* fx_renderer_create_with_drm_fd(int drm_fd);
/* Creates through GBM without enumerating EGL devices. */
struct wlr_renderer* fx_renderer_create_with_drm_fd_gbm(int drm_fd);
struct wlr_renderer* fx_renderer_create(struct wlr_backend* backend);

struct wl_event_loop;
// Moves every GL flush and fence of `renderer` to its own thread, completing on `loop`. False when the thread cannot
// start; the renderer then submits on the calling thread.
bool fx_renderer_start_render_thread(struct wlr_renderer* renderer, struct wl_event_loop* loop);

// Runs the flushes and fences queued so far, then `done` on the event loop (ok false when one failed). Without a render
// thread they already ran and `done` runs now. Never blocks.
void fx_renderer_kick(struct wlr_renderer* renderer, void (*done)(void* data, bool ok), void* data);
// While set, render passes leave their flushes and fences queued for fx_renderer_kick (an output frame's build).
// Otherwise a pass submits before it returns, as without a render thread.
void fx_renderer_defer_submits(struct wlr_renderer* renderer, bool defer);
// Drops a `done` not yet run, for an owner that goes away.
void fx_renderer_cancel_kick(struct wlr_renderer* renderer, void (*done)(void* data, bool ok), void* data);
// True while the render thread runs a job, holds queued work or has a `done` not yet run (a capture copy in flight).
bool fx_renderer_work_pending(struct wlr_renderer* renderer);
// CLOCK_MONOTONIC ns at which the render thread's running job started; 0 when it is idle or there is none.
int64_t fx_renderer_busy_since_ns(struct wlr_renderer* renderer);
// False while the render thread holds the GL context: no GL call may start on the caller's thread until a kick's done.
bool fx_renderer_context_home(struct wlr_renderer* renderer);
// Links the glass composite variants now, at startup, instead of on their first draw: each costs a frame or more to
// link (the overview's first open dropped 2), and the desktop's glass uses them all. False if the context is away.
bool fx_renderer_warm_glass(struct wlr_renderer* renderer);
// Test hook: the next render-thread job sleeps `ms` first. False without a render thread.
bool fx_renderer_inject_submit_stall(struct wlr_renderer* renderer, int ms);
// Runs on the render thread around each job, for a stall watch. Process-wide; NULL removes it. `watch` must outlive
// the renderer.
struct fx_render_job_watch {
  void (*begin)(void);
  void (*end)(void);
};
void fx_renderer_set_job_watch(const struct fx_render_job_watch* watch);

struct fx_renderer* fx_get_renderer(struct wlr_renderer* wlr_renderer);

bool fx_renderer_check_ext(struct wlr_renderer* renderer, const char* ext);
GLuint fx_renderer_get_buffer_fbo(struct wlr_renderer* renderer, struct wlr_buffer* buffer);
void fx_renderer_clear_output_effect_buffers(struct wlr_output* output);

/**
 * Begins a pass for an output swapchain buffer. HDR passes use output-local
 * scratch storage instead of attaching one copy to every swapchain buffer.
 */
struct wlr_render_pass* fx_renderer_begin_output_buffer_pass(
    struct wlr_output* output, struct wlr_buffer* buffer, const struct wlr_buffer_pass_options* options
);

//
// fx_texture
//

struct fx_texture_attribs {
  GLenum target; /* either GL_TEXTURE_2D or GL_TEXTURE_EXTERNAL_OES */
  GLuint tex;

  bool has_alpha;
};

struct wlr_texture* fx_texture_from_buffer(struct wlr_renderer* wlr_renderer, struct wlr_buffer* buffer);

void fx_texture_get_attribs(struct wlr_texture* texture, struct fx_texture_attribs* attribs);

// After wlr_render_timer_get_duration_ns: CPU record time and GPU execution time (first to last GPU timestamp) of
// the pass, in ns. False for a timer of another renderer.
bool fx_render_timer_split(struct wlr_render_timer* timer, int64_t* cpu_ns, int64_t* gpu_exec_ns);

#endif
