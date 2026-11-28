#ifndef RENDER_FX_RENDERER_BACKDROP_PROBE_H
#define RENDER_FX_RENDERER_BACKDROP_PROBE_H

#include "andromedafx/render/backdrop_probe.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

struct fx_renderer;

/** True when no sample is in flight and the first sample, or 100 ms since the last one, is due. */
bool fx_backdrop_probe_due(const struct fx_backdrop_probe* probe);
// A pass redrew the whole probe box but did not capture: the sample is out of date until the next capture.
void fx_backdrop_probe_mark_stale(struct fx_backdrop_probe* probe);

// The source of a capture that read the optimized blur cache. cache false: a live backdrop, never equal to another.
struct fx_backdrop_probe_source {
  bool cache;
  uint64_t cache_generation;
  struct wlr_box box;
  float strength;
  bool linear;
};

// The last completed capture read `source`: the backdrop is unchanged since, so a redraw needs no new capture.
bool fx_backdrop_probe_sampled(const struct fx_backdrop_probe* probe, const struct fx_backdrop_probe_source* source);

/** Queues the probe and flushes it alone: no other probe may be queued. source: NULL for a live backdrop. */
void fx_backdrop_probe_capture(
    struct fx_backdrop_probe* probe, GLuint texture, GLenum target, int texture_width, int texture_height,
    const struct wlr_box* box, bool linear, const struct fx_backdrop_probe_source* source
);

/**
 * Queues the probe's 4x2 sample grid of box (source texture px, top-left origin) for the next flush; false when the
 * probe has a sample in flight or the batch is full. linear: the source holds linear-light values.
 */
bool fx_backdrop_probe_queue(struct fx_backdrop_probe* probe, const struct wlr_box* box, bool linear);
bool fx_backdrop_probes_queued(const struct fx_renderer* renderer);
/**
 * Draws every queued grid from texture into one atlas and reads them back with one copy, which the next submit fences.
 * Without a texture the queued probes stay due. Restores viewport, blend and scissor; the caller rebinds its framebuffer.
 */
void fx_backdrop_probes_flush(
    struct fx_renderer* renderer, GLuint texture, GLenum target, int texture_width, int texture_height
);

// Fences every probe captured since the last submit with one sync; called at render pass submit.
void fx_backdrop_probes_fence(struct fx_renderer* renderer);
// Hands `sync` (made on the render thread for this pass) to every probe awaiting one; destroys it when none does.
// Main thread only: it walks the probe list.
void fx_backdrop_probes_attach_fence(struct fx_renderer* renderer, EGLSyncKHR sync);
/** Releases the GL objects of every probe of this renderer; the renderer's context is current. */
void fx_backdrop_probes_release(struct fx_renderer* renderer);

#endif
