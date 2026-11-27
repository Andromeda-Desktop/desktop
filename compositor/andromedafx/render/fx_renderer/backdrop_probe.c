#include "render/fx_renderer/backdrop_probe.h"
#include "render/egl.h"
#include "render/fx_renderer/fx_renderer.h"
#include "render/fx_renderer/shaders.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/util/log.h>

// A batch draws up to kBatchSlots probes into one atlas, kProbeHeight rows each, and reads them back at once: on gen9
// every separate capture cost a render target switch and a driver download draw (~12 us).
enum { kProbeWidth = 4, kProbeHeight = 2, kBatchSlots = 32 };
// Density follows the backdrop within this; each capture costs a tiny draw and a readback.
static const int64_t kProbeIntervalNs = 100 * 1000 * 1000;

// One fence per render pass for every probe captured in it, created at submit: no mid-frame flush.
struct probe_fence {
  EGLSyncKHR sync;
  int refs;
  bool signaled;
};

// The pack buffer one batch reads its samples into, shared by the probes of that batch.
struct probe_readback {
  GLuint pack_buffer;
  int refs;
};

enum probe_state {
  PROBE_IDLE,
  PROBE_QUEUED,         // in the batch; the next flush draws it
  PROBE_AWAITING_FENCE, // drawn and read back; the pass submit fences it
  PROBE_FENCED,         // poll maps the samples once the fence signalled
};

struct fx_backdrop_probe {
  struct fx_renderer* renderer; // NULL once the renderer is gone
  struct wl_list link;          // fx_renderer.backdrop_probes
  enum probe_state state;
  struct probe_fence* fence;       // PROBE_FENCED
  struct probe_readback* readback; // PROBE_AWAITING_FENCE and PROBE_FENCED
  int slot;                        // the probe's rows in the batch atlas and readback
  struct wlr_box box;              // PROBE_QUEUED: the sampled region, source texture px
  bool linear;
  bool captured;          // the first capture runs at once: a new sheet stays hidden until its first sample
  int64_t last_capture_ns; // CLOCK_MONOTONIC
  bool stale; // a pass redrew the backdrop after the last capture, inside kProbeIntervalNs or while one was in flight
};

static int64_t monotonic_ns(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static const GLchar probe_vert_src[] = "attribute vec2 pos;\n"
                                       "uniform vec4 src;\n"
                                       "varying vec2 uv;\n"
                                       "void main() {\n"
                                       "  uv = src.xy + pos * src.zw;\n"
                                       "  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
                                       "}\n";

static const GLchar probe_frag_src[] = "precision mediump float;\n"
                                       "uniform sampler2D tex;\n"
                                       "varying vec2 uv;\n"
                                       "void main() { gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0); }\n";

static struct {
  GLuint program;
  GLint pos;
  GLint src;
  GLint tex;
  struct fx_renderer* renderer;
} probe_shader;

static struct {
  struct fx_renderer* renderer;
  GLuint texture; // the atlas: kProbeWidth x kProbeHeight * kBatchSlots
  GLuint framebuffer;
  struct fx_backdrop_probe* queued[kBatchSlots];
  int count;
} batch;

static bool ensure_program(struct fx_renderer* renderer) {
  if (probe_shader.renderer == renderer && probe_shader.program != 0) {
    return true;
  }
  probe_shader.program = 0;
  probe_shader.renderer = NULL;
  GLuint vert = compile_shader(GL_VERTEX_SHADER, probe_vert_src);
  GLuint frag = vert != 0 ? compile_shader(GL_FRAGMENT_SHADER, probe_frag_src) : 0;
  if (frag == 0) {
    glDeleteShader(vert);
    return false;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vert);
  glAttachShader(program, frag);
  glLinkProgram(program);
  glDeleteShader(vert);
  glDeleteShader(frag);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok == GL_FALSE) {
    wlr_log(WLR_ERROR, "backdrop probe: shader link failed");
    glDeleteProgram(program);
    return false;
  }
  probe_shader.program = program;
  probe_shader.pos = glGetAttribLocation(program, "pos");
  probe_shader.src = glGetUniformLocation(program, "src");
  probe_shader.tex = glGetUniformLocation(program, "tex");
  probe_shader.renderer = renderer;
  return true;
}

static bool ensure_atlas(struct fx_renderer* renderer) {
  if (batch.renderer == renderer && batch.framebuffer != 0) {
    return true;
  }
  batch.renderer = renderer;
  glGenTextures(1, &batch.texture);
  glBindTexture(GL_TEXTURE_2D, batch.texture);
  glTexImage2D(
      GL_TEXTURE_2D, 0, GL_RGBA, kProbeWidth, kProbeHeight * kBatchSlots, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL
  );
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenFramebuffers(1, &batch.framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, batch.framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, batch.texture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    wlr_log(WLR_ERROR, "backdrop probe: incomplete framebuffer");
    glDeleteFramebuffers(1, &batch.framebuffer);
    glDeleteTextures(1, &batch.texture);
    batch.framebuffer = batch.texture = 0;
    return false;
  }
  return true;
}

// Ends whatever sample the probe has in flight; the probe is idle after it.
static void release_sample(struct fx_backdrop_probe* probe) {
  if (probe->state == PROBE_QUEUED) {
    for (int i = 0; i < batch.count; i++) {
      if (batch.queued[i] == probe) {
        batch.queued[i] = batch.queued[--batch.count];
        break;
      }
    }
  }
  struct probe_fence* fence = probe->fence;
  probe->fence = NULL;
  if (fence != NULL && --fence->refs == 0) {
    wlr_egl_destroy_sync(probe->renderer->egl, fence->sync);
    free(fence);
  }
  struct probe_readback* readback = probe->readback;
  probe->readback = NULL;
  if (readback != NULL && --readback->refs == 0) {
    fx_renderer_delete_gl(probe->renderer, FX_GL_BUFFER, readback->pack_buffer);
    free(readback);
  }
  probe->state = PROBE_IDLE;
}

struct fx_backdrop_probe* fx_backdrop_probe_create(struct wlr_renderer* wlr_renderer) {
  if (wlr_renderer == NULL || !wlr_renderer_is_fx(wlr_renderer)) {
    return NULL;
  }
  struct fx_renderer* renderer = fx_get_renderer(wlr_renderer);
  if (renderer->egl->procs.eglCreateSyncKHR == NULL || renderer->egl->eglClientWaitSyncKHR == NULL) {
    return NULL;
  }
  if (!renderer->is_gles3) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      wlr_log(WLR_INFO, "backdrop probe: off, pixel pack buffers need GLES 3");
    }
    return NULL;
  }
  struct fx_backdrop_probe* probe = calloc(1, sizeof(*probe));
  if (probe == NULL) {
    return NULL;
  }
  probe->renderer = renderer;
  wl_list_insert(&renderer->backdrop_probes, &probe->link);
  return probe;
}

void fx_backdrop_probe_destroy(struct fx_backdrop_probe* probe) {
  if (probe == NULL) {
    return;
  }
  if (probe->renderer != NULL) {
    release_sample(probe);
    wl_list_remove(&probe->link);
  }
  free(probe);
}

struct wlr_renderer* fx_backdrop_probe_renderer(const struct fx_backdrop_probe* probe) {
  return probe != NULL && probe->renderer != NULL ? &probe->renderer->wlr_renderer : NULL;
}

// Every probe shares one phase (a new kProbeIntervalNs epoch): a pass that redraws two overlapping probes finds both due
// or neither. Per-probe phases ping-pong: each refresh redraw marks its sibling stale, and the output never idles.
bool fx_backdrop_probe_due(const struct fx_backdrop_probe* probe) {
  return probe != NULL && probe->state == PROBE_IDLE
      && (!probe->captured || monotonic_ns() / kProbeIntervalNs > probe->last_capture_ns / kProbeIntervalNs);
}

void fx_backdrop_probe_mark_stale(struct fx_backdrop_probe* probe) {
  if (probe != NULL) {
    probe->stale = true;
  }
}

bool fx_backdrop_probe_stale(const struct fx_backdrop_probe* probe) {
  return probe != NULL && probe->renderer != NULL && probe->stale;
}

bool fx_backdrop_probe_take_stale(struct fx_backdrop_probe* probe) {
  if (!fx_backdrop_probe_stale(probe) || !fx_backdrop_probe_due(probe)) {
    return false;
  }
  probe->stale = false;
  return true;
}

bool fx_backdrop_probe_pending(const struct fx_backdrop_probe* probe) {
  return probe != NULL && probe->renderer != NULL && probe->state != PROBE_IDLE;
}

bool fx_backdrop_probe_queue(struct fx_backdrop_probe* probe, const struct wlr_box* box, bool linear) {
  // The caller skips a capture while one is in flight; a node redrawn every frame would otherwise never finish one.
  if (probe == NULL || probe->renderer == NULL || probe->state != PROBE_IDLE || box->width <= 0 || box->height <= 0
      || batch.count == kBatchSlots || (batch.count > 0 && batch.queued[0]->renderer != probe->renderer)) {
    return false;
  }
  probe->state = PROBE_QUEUED;
  probe->box = *box;
  probe->linear = linear;
  batch.queued[batch.count++] = probe;
  return true;
}

bool fx_backdrop_probes_queued(const struct fx_renderer* renderer) {
  return batch.count > 0 && batch.queued[0]->renderer == renderer;
}

void fx_backdrop_probes_flush(
    struct fx_renderer* renderer, GLuint texture, GLenum target, int texture_width, int texture_height
) {
  if (!fx_backdrop_probes_queued(renderer)) {
    return;
  }
  struct probe_readback* readback = NULL;
  if (texture != 0 && target == GL_TEXTURE_2D && texture_width > 0 && texture_height > 0 && ensure_program(renderer)
      && ensure_atlas(renderer)) {
    readback = calloc(1, sizeof(*readback));
  }
  if (readback == NULL) {
    // Not drawn: each probe stays due and captures on a later pass.
    while (batch.count > 0) {
      struct fx_backdrop_probe* probe = batch.queued[batch.count - 1];
      release_sample(probe);
      probe->stale = true;
    }
    return;
  }
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  glBindFramebuffer(GL_FRAMEBUFFER, batch.framebuffer);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glUseProgram(probe_shader.program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glUniform1i(probe_shader.tex, 0);
  static const GLfloat quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(probe_shader.pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
  glEnableVertexAttribArray(probe_shader.pos);
  const int64_t now = monotonic_ns();
  for (int i = 0; i < batch.count; i++) {
    struct fx_backdrop_probe* probe = batch.queued[i];
    const struct wlr_box* box = &probe->box;
    glViewport(0, i * kProbeHeight, kProbeWidth, kProbeHeight);
    // Each probe pixel center samples one point of a 4x2 grid over the box; the grid is symmetric in y, so the
    // texture's row order does not move it.
    glUniform4f(
        probe_shader.src, (float)box->x / (float)texture_width, (float)box->y / (float)texture_height,
        (float)box->width / (float)texture_width, (float)box->height / (float)texture_height
    );
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    probe->state = PROBE_AWAITING_FENCE;
    probe->readback = readback;
    probe->slot = i;
    probe->captured = true;
    probe->last_capture_ns = now;
    probe->stale = false;
  }
  glDisableVertexAttribArray(probe_shader.pos);
  // The pass submit fences this copy with the draws, so poll maps the buffer only once the GPU finished it.
  const GLsizeiptr bytes = (GLsizeiptr)batch.count * kProbeWidth * kProbeHeight * 4;
  glGenBuffers(1, &readback->pack_buffer);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, readback->pack_buffer);
  glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
  glReadPixels(0, 0, kProbeWidth, kProbeHeight * batch.count, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  readback->refs = batch.count;
  batch.count = 0;
  if (blend) {
    glEnable(GL_BLEND);
  }
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

void fx_backdrop_probe_capture(
    struct fx_backdrop_probe* probe, GLuint texture, GLenum target, int texture_width, int texture_height,
    const struct wlr_box* box, bool linear
) {
  if (probe == NULL || probe->renderer == NULL) {
    return;
  }
  // Probes left queued by a pass that never ended its shared run sampled another source: they capture later.
  fx_backdrop_probes_flush(probe->renderer, 0, 0, 0, 0);
  if (fx_backdrop_probe_queue(probe, box, linear)) {
    fx_backdrop_probes_flush(probe->renderer, texture, target, texture_width, texture_height);
  }
}

bool fx_backdrop_probe_poll(struct fx_backdrop_probe* probe, struct fx_backdrop_luminance* luminance) {
  if (!fx_backdrop_probe_pending(probe)) {
    return false;
  }
  struct probe_fence* fence = probe->fence;
  if (fence == NULL) {
    return false; // captured in a pass that has not been submitted yet
  }
  struct wlr_egl* egl = probe->renderer->egl;
  // The wait needs no current context; only the readback does.
  if (!fence->signaled) {
    const EGLint status = egl->eglClientWaitSyncKHR(egl->display, fence->sync, 0, 0);
    if (status == EGL_FALSE) {
      release_sample(probe);
      return false;
    }
    if (status != EGL_CONDITION_SATISFIED_KHR) {
      return false;
    }
    fence->signaled = true;
  }
  if (!fx_renderer_context_home(&probe->renderer->wlr_renderer)) {
    return false; // the render thread holds the context; the caller polls again
  }
  struct wlr_egl_context saved;
  if (!wlr_egl_make_current(egl, &saved)) {
    release_sample(probe);
    return false;
  }
  bool done = false;
  const GLsizeiptr bytes = kProbeWidth * kProbeHeight * 4;
  glBindBuffer(GL_PIXEL_PACK_BUFFER, probe->readback->pack_buffer);
  const GLubyte* pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, probe->slot * bytes, bytes, GL_MAP_READ_BIT);
  if (pixels != NULL) {
    // Rec.709 weights apply to linear light; on encoded bytes they underrate saturated colours (pink reads as dark).
    double sum = 0.0;
    // The two lowest and two highest samples: the reported extremes drop the single most extreme one.
    double low[2] = {1.0, 1.0};
    double high[2] = {0.0, 0.0};
    for (int i = 0; i < kProbeWidth * kProbeHeight; i++) {
      const GLubyte* p = &pixels[i * 4];
      double rgb[3];
      for (int c = 0; c < 3; c++) {
        const double v = p[c] / 255.0;
        rgb[c] = probe->linear ? v : (v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
      }
      const double y = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
      sum += y;
      if (y < low[0]) {
        low[1] = low[0];
        low[0] = y;
      } else if (y < low[1]) {
        low[1] = y;
      }
      if (y > high[0]) {
        high[1] = high[0];
        high[0] = y;
      } else if (y > high[1]) {
        high[1] = y;
      }
    }
    luminance->mean = (float)pow(sum / (kProbeWidth * kProbeHeight), 1.0 / 2.2);
    luminance->darkest = (float)pow(low[1], 1.0 / 2.2);
    luminance->brightest = (float)pow(high[1], 1.0 / 2.2);
    done = true;
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  release_sample(probe);
  wlr_egl_restore_context(&saved);
  return done;
}

void fx_backdrop_probes_attach_fence(struct fx_renderer* renderer, EGLSyncKHR sync) {
  struct probe_fence* fence = NULL;
  struct fx_backdrop_probe* probe;
  wl_list_for_each(probe, &renderer->backdrop_probes, link) {
    if (probe->state != PROBE_AWAITING_FENCE || sync == EGL_NO_SYNC_KHR) {
      continue; // without a sync the probes stay awaiting; the next submit fences them
    }
    if (fence == NULL) {
      fence = calloc(1, sizeof(*fence));
      if (fence == NULL) {
        break;
      }
      fence->sync = sync;
    }
    probe->state = PROBE_FENCED;
    probe->fence = fence;
    fence->refs++;
  }
  if (fence == NULL && sync != EGL_NO_SYNC_KHR) {
    wlr_egl_destroy_sync(renderer->egl, sync);
  }
}

void fx_backdrop_probes_fence(struct fx_renderer* renderer) {
  struct wlr_egl* egl = renderer->egl;
  fx_backdrop_probes_attach_fence(renderer, egl->procs.eglCreateSyncKHR(egl->display, EGL_SYNC_FENCE_KHR, NULL));
}

void fx_backdrop_probes_release(struct fx_renderer* renderer) {
  struct fx_backdrop_probe *probe, *tmp;
  wl_list_for_each_safe(probe, tmp, &renderer->backdrop_probes, link) {
    release_sample(probe);
    wl_list_remove(&probe->link);
    wl_list_init(&probe->link);
    probe->renderer = NULL;
  }
  if (probe_shader.renderer == renderer) {
    glDeleteProgram(probe_shader.program);
    probe_shader.program = 0;
    probe_shader.renderer = NULL;
  }
  if (batch.renderer == renderer) {
    glDeleteFramebuffers(1, &batch.framebuffer);
    glDeleteTextures(1, &batch.texture);
    batch.framebuffer = batch.texture = 0;
    batch.renderer = NULL;
  }
}
