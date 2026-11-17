#ifndef FX_RENDER_THREAD_H
#define FX_RENDER_THREAD_H

#include <stdbool.h>
#include <stdint.h>

struct fx_renderer;
struct wl_event_loop;
struct wlr_buffer;
struct wlr_drm_syncobj_timeline;

// Where the renderer's only EGL context is current. The main thread records GL commands only while it is Home; the
// render thread holds it Away while it runs the queued submits, the only calls that enter the kernel and can wait on
// another process's buffer locks.
enum fx_context_location {
  FX_CONTEXT_HOME,
  FX_CONTEXT_AWAY,
};

enum fx_submit_kind {
  // Flush, then signal `timeline` at `point` with the render fence.
  FX_SUBMIT_TIMELINE,
  // Flush, then attach the render fence to `buffer`'s DMA-BUF for its implicit-sync reader.
  FX_SUBMIT_IMPLICIT,
  // Flush only.
  FX_SUBMIT_FLUSH,
  // The fence the backdrop probes captured this pass wait on (backdrop_probe.c).
  FX_SUBMIT_PROBE_FENCE,
  // Flush, then wait until the GPU ran every recorded command, for a readback the main thread maps after the job.
  FX_SUBMIT_FINISH,
};

struct fx_submit_op {
  enum fx_submit_kind kind;
  struct wlr_drm_syncobj_timeline* timeline;
  uint64_t point;
  struct wlr_buffer* buffer;
};

typedef void (*fx_submit_done_fn)(void* data, bool ok);

struct fx_render_thread* fx_render_thread_create(struct fx_renderer* renderer, struct wl_event_loop* loop);
void fx_render_thread_destroy(struct fx_render_thread* thread);

enum fx_context_location fx_render_thread_location(const struct fx_render_thread* thread);

// Queues `op` for the next kick; takes its own timeline reference and buffer lock, dropped on the main thread after
// the op ran.
void fx_render_thread_queue(struct fx_render_thread* thread, const struct fx_submit_op* op);

// Hands the context to the render thread to run every queued op, then `done` runs on the event loop with the context
// Home again. Home with nothing queued: `done` runs now. Away with nothing queued: it waits for the job in flight.
// Never blocks.
void fx_render_thread_kick(struct fx_render_thread* thread, fx_submit_done_fn done, void* data);

#define FX_RENDER_THREAD_SYNC_TIMEOUT_MS 1000

// Blocks until the job in flight and every queued op ran and the context is Home, at most
// FX_RENDER_THREAD_SYNC_TIMEOUT_MS: false when the context is still away then (a stalled GPU submit), and the caller
// fails its work. The waiters run from the event loop afterwards, never inside this call. For passes outside an output
// frame.
bool fx_render_thread_sync(struct fx_render_thread* thread);

// Kicks with no waiter once the current event dispatch returns, for passes nobody waits on (cursor, captures).
void fx_render_thread_kick_later(struct fx_render_thread* thread);

// Removes a pending `done` whose owner goes away.
void fx_render_thread_cancel(struct fx_render_thread* thread, fx_submit_done_fn done, void* data);

// CLOCK_MONOTONIC ns at which the running job started, 0 when idle. Any thread.
int64_t fx_render_thread_busy_since_ns(const struct fx_render_thread* thread);
bool fx_render_thread_work_pending(const struct fx_render_thread* thread);

// Test hook: the next job sleeps `ms` before its ops, as a submit stuck on another process's buffer lock does.
void fx_render_thread_inject_stall(struct fx_render_thread* thread, int ms);

#endif
