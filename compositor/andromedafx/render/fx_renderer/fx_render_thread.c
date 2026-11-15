#define _GNU_SOURCE
#include "render/fx_renderer/fx_render_thread.h"
#include "render/egl.h"
#include "render/fx_renderer/backdrop_probe.h"
#include "render/fx_renderer/fx_renderer.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

struct queued_op {
  struct fx_submit_op op;
  struct wlr_dmabuf_attributes dmabuf; // FX_SUBMIT_IMPLICIT: read on the main thread, fds owned by the locked buffer
  bool has_dmabuf;
};

struct op_list {
  struct queued_op* ops;
  size_t len, cap;
};

struct waiter {
  fx_submit_done_fn fn;
  void* data;
};

struct waiter_list {
  struct waiter* items;
  size_t len, cap;
};

// Job hand-off between the threads, under `mutex`.
enum job_state {
  JOB_IDLE,
  JOB_POSTED,
  JOB_DONE,
  JOB_STOP,
};

struct fx_render_thread {
  struct fx_renderer* renderer;
  int event_fd;
  struct wl_event_loop* loop;
  struct wl_event_source* source;
  struct wl_event_source* idle_kick;
  pthread_t thread;

  // Main thread only.
  enum fx_context_location location;
  struct op_list queued;        // for the next job
  struct waiter_list next;      // run after the next job
  struct waiter_list current;   // run after the job in flight
  struct op_list in_flight;     // owned by the render thread while Away

  pthread_mutex_t mutex;
  pthread_cond_t wake;
  enum job_state job;
  bool job_ok;

  _Atomic int64_t busy_since_ns;
  _Atomic int stall_ms;
};

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static bool grow(void** items, size_t* cap, size_t len, size_t size) {
  if (len < *cap) {
    return true;
  }
  const size_t want = *cap == 0 ? 8 : *cap * 2;
  void* grown = realloc(*items, want * size);
  if (grown == NULL) {
    return false;
  }
  *items = grown;
  *cap = want;
  return true;
}

static void add_waiter(struct waiter_list* list, fx_submit_done_fn fn, void* data) {
  if (!grow((void**)&list->items, &list->cap, list->len, sizeof(*list->items))) {
    wlr_log(WLR_ERROR, "render thread: out of memory for a waiter");
    fn(data, false);
    return;
  }
  list->items[list->len++] = (struct waiter){fn, data};
}

static void remove_waiter(struct waiter_list* list, fx_submit_done_fn fn, void* data) {
  for (size_t i = 0; i < list->len; i++) {
    if (list->items[i].fn == fn && list->items[i].data == data) {
      list->items[i] = list->items[--list->len];
      return;
    }
  }
}

static bool attach_implicit_fence(struct wlr_egl* egl, const struct queued_op* queued, EGLSyncKHR sync) {
  if (!queued->has_dmabuf) {
    return false;
  }
  int fd = wlr_egl_dup_fence_fd(egl, sync);
  if (fd < 0) {
    return false;
  }
  bool attached = true;
  for (int i = 0; i < queued->dmabuf.n_planes; i++) {
    struct dma_buf_import_sync_file data = {.flags = DMA_BUF_SYNC_WRITE, .fd = fd};
    int ret;
    do {
      ret = ioctl(queued->dmabuf.fd[i], DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &data);
    } while (ret != 0 && (errno == EINTR || errno == EAGAIN));
    if (ret != 0) {
      wlr_log_errno(WLR_DEBUG, "DMA_BUF_IOCTL_IMPORT_SYNC_FILE failed");
      attached = false;
      break;
    }
  }
  close(fd);
  return attached;
}

static bool run_op(struct fx_renderer* renderer, const struct queued_op* queued) {
  struct wlr_egl* egl = renderer->egl;
  switch (queued->op.kind) {
  case FX_SUBMIT_PROBE_FENCE:
    fx_backdrop_probes_fence(renderer);
    return true;
  case FX_SUBMIT_FLUSH:
    glFlush();
    return true;
  case FX_SUBMIT_TIMELINE: {
    EGLSyncKHR sync = wlr_egl_create_sync(egl, -1);
    if (sync == EGL_NO_SYNC_KHR) {
      return false;
    }
    int fd = wlr_egl_dup_fence_fd(egl, sync);
    wlr_egl_destroy_sync(egl, sync);
    if (fd < 0) {
      return false;
    }
    bool ok = wlr_drm_syncobj_timeline_import_sync_file(queued->op.timeline, queued->op.point, fd);
    close(fd);
    return ok;
  }
  case FX_SUBMIT_IMPLICIT: {
    EGLSyncKHR sync = wlr_egl_create_sync(egl, -1);
    if (sync == EGL_NO_SYNC_KHR) {
      glFinish();
      return true;
    }
    // The native fence has no fd until flushed.
    glFlush();
    bool ok = attach_implicit_fence(egl, queued, sync) && !egl->ignores_implicit_fences;
    if (!ok && egl->eglClientWaitSyncKHR != NULL) {
      ok = egl->eglClientWaitSyncKHR(egl->display, sync, 0, EGL_FOREVER_KHR) == EGL_CONDITION_SATISFIED_KHR;
    }
    wlr_egl_destroy_sync(egl, sync);
    if (!ok) {
      glFinish();
    }
    return true;
  }
  }
  return false;
}

static void* thread_main(void* data) {
  struct fx_render_thread* thread = data;
  pthread_setname_np(pthread_self(), "andromeda-render");
  pthread_mutex_lock(&thread->mutex);
  for (;;) {
    while (thread->job != JOB_POSTED && thread->job != JOB_STOP) {
      pthread_cond_wait(&thread->wake, &thread->mutex);
    }
    if (thread->job == JOB_STOP) {
      break;
    }
    pthread_mutex_unlock(&thread->mutex);

    atomic_store(&thread->busy_since_ns, now_ns());
    const int stall_ms = atomic_exchange(&thread->stall_ms, 0);
    if (stall_ms > 0) {
      struct timespec stall = {.tv_sec = stall_ms / 1000, .tv_nsec = (long)(stall_ms % 1000) * 1000000};
      nanosleep(&stall, NULL);
    }
    bool ok = wlr_egl_make_current(thread->renderer->egl, NULL);
    for (size_t i = 0; ok && i < thread->in_flight.len; i++) {
      ok = run_op(thread->renderer, &thread->in_flight.ops[i]);
    }
    wlr_egl_unset_current(thread->renderer->egl);
    atomic_store(&thread->busy_since_ns, 0);

    pthread_mutex_lock(&thread->mutex);
    thread->job_ok = ok;
    thread->job = JOB_DONE;
    pthread_cond_broadcast(&thread->wake);
    const uint64_t one = 1;
    if (write(thread->event_fd, &one, sizeof(one)) < 0) {
      wlr_log_errno(WLR_ERROR, "render thread: eventfd write failed");
    }
  }
  pthread_mutex_unlock(&thread->mutex);
  return NULL;
}

static void release_ops(struct op_list* list) {
  for (size_t i = 0; i < list->len; i++) {
    struct fx_submit_op* op = &list->ops[i].op;
    if (op->timeline != NULL) {
      wlr_drm_syncobj_timeline_unref(op->timeline);
    }
    if (op->buffer != NULL) {
      wlr_buffer_unlock(op->buffer);
    }
  }
  list->len = 0;
}

static void start_job(struct fx_render_thread* thread) {
  struct op_list swap = thread->in_flight;
  thread->in_flight = thread->queued;
  thread->queued = swap;
  struct waiter_list waiters = thread->current;
  thread->current = thread->next;
  thread->next = waiters;

  // Release behavior NONE (egl.c): unbinding records no flush, so nothing reaches the kernel on this thread.
  wlr_egl_unset_current(thread->renderer->egl);
  wlr_egl_set_lent(thread->renderer->egl, true);
  thread->location = FX_CONTEXT_AWAY;
  pthread_mutex_lock(&thread->mutex);
  thread->job = JOB_POSTED;
  pthread_cond_signal(&thread->wake);
  pthread_mutex_unlock(&thread->mutex);
}

static int handle_done(int fd, uint32_t mask, void* data) {
  struct fx_render_thread* thread = data;
  uint64_t count;
  if (read(fd, &count, sizeof(count)) < 0 && errno != EAGAIN) {
    wlr_log_errno(WLR_ERROR, "render thread: eventfd read failed");
  }
  pthread_mutex_lock(&thread->mutex);
  if (thread->job != JOB_DONE) {
    pthread_mutex_unlock(&thread->mutex);
    return 0;
  }
  const bool ok = thread->job_ok;
  thread->job = JOB_IDLE;
  pthread_mutex_unlock(&thread->mutex);

  thread->location = FX_CONTEXT_HOME;
  wlr_egl_set_lent(thread->renderer->egl, false);
  fx_renderer_collect_gl_garbage(thread->renderer);
  release_ops(&thread->in_flight);
  // A waiter can queue and kick again; it then lands in `next`, not in the list run here.
  struct waiter_list done = thread->current;
  thread->current = (struct waiter_list){0};
  for (size_t i = 0; i < done.len; i++) {
    done.items[i].fn(done.items[i].data, ok);
  }
  free(done.items);
  if (thread->location == FX_CONTEXT_HOME && (thread->queued.len > 0 || thread->next.len > 0)) {
    start_job(thread);
  }
  return 0;
}

struct fx_render_thread* fx_render_thread_create(struct fx_renderer* renderer, struct wl_event_loop* loop) {
  struct fx_render_thread* thread = calloc(1, sizeof(*thread));
  if (thread == NULL) {
    return NULL;
  }
  thread->renderer = renderer;
  thread->loop = loop;
  thread->location = FX_CONTEXT_HOME;
  pthread_mutex_init(&thread->mutex, NULL);
  pthread_cond_init(&thread->wake, NULL);
  thread->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (thread->event_fd < 0) {
    goto error;
  }
  thread->source = wl_event_loop_add_fd(loop, thread->event_fd, WL_EVENT_READABLE, handle_done, thread);
  if (thread->source == NULL) {
    goto error;
  }
  if (pthread_create(&thread->thread, NULL, thread_main, thread) != 0) {
    goto error;
  }
  return thread;

error:
  wlr_log(WLR_ERROR, "render thread: setup failed");
  if (thread->source != NULL) {
    wl_event_source_remove(thread->source);
  }
  if (thread->event_fd >= 0) {
    close(thread->event_fd);
  }
  pthread_cond_destroy(&thread->wake);
  pthread_mutex_destroy(&thread->mutex);
  free(thread);
  return NULL;
}

void fx_render_thread_destroy(struct fx_render_thread* thread) {
  if (thread == NULL) {
    return;
  }
  pthread_mutex_lock(&thread->mutex);
  // Teardown joins a job in flight: the context and the buffers it signals must outlive it.
  while (thread->job == JOB_POSTED) {
    pthread_cond_wait(&thread->wake, &thread->mutex);
  }
  thread->job = JOB_STOP;
  pthread_cond_signal(&thread->wake);
  pthread_mutex_unlock(&thread->mutex);
  pthread_join(thread->thread, NULL);
  wlr_egl_set_lent(thread->renderer->egl, false);

  if (thread->idle_kick != NULL) {
    wl_event_source_remove(thread->idle_kick);
  }
  wl_event_source_remove(thread->source);
  close(thread->event_fd);
  release_ops(&thread->in_flight);
  release_ops(&thread->queued);
  for (size_t i = 0; i < thread->current.len; i++) {
    thread->current.items[i].fn(thread->current.items[i].data, false);
  }
  for (size_t i = 0; i < thread->next.len; i++) {
    thread->next.items[i].fn(thread->next.items[i].data, false);
  }
  free(thread->current.items);
  free(thread->next.items);
  free(thread->in_flight.ops);
  free(thread->queued.ops);
  pthread_cond_destroy(&thread->wake);
  pthread_mutex_destroy(&thread->mutex);
  free(thread);
}

enum fx_context_location fx_render_thread_location(const struct fx_render_thread* thread) {
  return thread->location;
}

void fx_render_thread_queue(struct fx_render_thread* thread, const struct fx_submit_op* op) {
  struct op_list* list = &thread->queued;
  if (!grow((void**)&list->ops, &list->cap, list->len, sizeof(*list->ops))) {
    wlr_log(WLR_ERROR, "render thread: out of memory for a submit");
    return;
  }
  struct queued_op* queued = &list->ops[list->len++];
  *queued = (struct queued_op){.op = *op};
  if (op->timeline != NULL) {
    queued->op.timeline = wlr_drm_syncobj_timeline_ref(op->timeline);
  }
  if (op->buffer != NULL) {
    queued->op.buffer = wlr_buffer_lock(op->buffer);
    queued->has_dmabuf = wlr_buffer_get_dmabuf(op->buffer, &queued->dmabuf);
  }
}

void fx_render_thread_kick(struct fx_render_thread* thread, fx_submit_done_fn done, void* data) {
  if (thread->location == FX_CONTEXT_HOME && thread->queued.len == 0) {
    done(data, true);
    return;
  }
  if (thread->location == FX_CONTEXT_AWAY && thread->queued.len == 0) {
    add_waiter(&thread->current, done, data);
    return;
  }
  add_waiter(&thread->next, done, data);
  if (thread->location == FX_CONTEXT_HOME) {
    start_job(thread);
  }
}

static void ignore_done(void* data, bool ok) {
  (void)data;
  (void)ok;
}

static void handle_idle_kick(void* data) {
  struct fx_render_thread* thread = data;
  thread->idle_kick = NULL;
  if (thread->queued.len > 0) {
    fx_render_thread_kick(thread, ignore_done, NULL);
  }
}

void fx_render_thread_kick_later(struct fx_render_thread* thread) {
  if (thread->idle_kick == NULL) {
    thread->idle_kick = wl_event_loop_add_idle(thread->loop, handle_idle_kick, thread);
  }
}

void fx_render_thread_cancel(struct fx_render_thread* thread, fx_submit_done_fn done, void* data) {
  remove_waiter(&thread->current, done, data);
  remove_waiter(&thread->next, done, data);
}

int64_t fx_render_thread_busy_since_ns(const struct fx_render_thread* thread) {
  return atomic_load(&thread->busy_since_ns);
}

void fx_render_thread_inject_stall(struct fx_render_thread* thread, int ms) {
  atomic_store(&thread->stall_ms, ms);
}
