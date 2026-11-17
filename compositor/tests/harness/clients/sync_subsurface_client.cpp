// Maps a toplevel with a synchronized child subsurface, then commits three child buffers without a parent commit,
// the way a GL client keeps drawing into a sync subsurface while its toolkit thread is blocked. Prints
// "queued middle=<released|held>" for the middle buffer, which the newest commit supersedes, then commits the parent
// and prints "applied first=<released|held> middle=<released|held>".
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

namespace {
  struct State {
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    bool configured = false;
  };

  struct Buffer {
    wl_buffer* resource = nullptr;
    bool released = false;
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t /*version*/) {
    auto* state = static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0) {
      state->subcompositor =
          static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state->wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    }
  }

  void registryRemove(void* /*data*/, wl_registry* /*registry*/, uint32_t /*name*/) {}

  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryRemove};

  void wmPing(void* /*data*/, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }

  constexpr xdg_wm_base_listener kWmBaseListener = {.ping = wmPing};

  void surfaceConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    static_cast<State*>(data)->configured = true;
  }

  constexpr xdg_surface_listener kSurfaceListener = {.configure = surfaceConfigure};

  void toplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
  void toplevelClose(void*, xdg_toplevel*) {}
  void toplevelBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
  void toplevelCapabilities(void*, xdg_toplevel*, wl_array*) {}

  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = toplevelBounds,
      .wm_capabilities = toplevelCapabilities,
  };

  void bufferRelease(void* data, wl_buffer* /*buffer*/) { static_cast<Buffer*>(data)->released = true; }

  constexpr wl_buffer_listener kBufferListener = {.release = bufferRelease};

  void createBuffer(State& state, Buffer& buffer, int width, int height, uint32_t color) {
    const int stride = width * 4;
    const size_t size = static_cast<size_t>(stride) * height;
    const int fd = memfd_create("andromeda-sync-subsurface-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
      std::perror("memfd");
      return;
    }
    void* pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels != MAP_FAILED) {
      std::fill_n(static_cast<uint32_t*>(pixels), size / sizeof(uint32_t), color);
      munmap(pixels, size);
    }
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    buffer.resource = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(buffer.resource, &kBufferListener, &buffer);
  }

  const char* word(const Buffer& buffer) { return buffer.released ? "released" : "held"; }
} // namespace

int main() {
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::fputs("cannot connect\n", stderr);
    return 1;
  }
  State state;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(display);
  if (state.compositor == nullptr || state.subcompositor == nullptr || state.shm == nullptr ||
      state.wmBase == nullptr) {
    std::fputs("missing globals\n", stderr);
    return 1;
  }
  xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, nullptr);

  wl_surface* parent = wl_compositor_create_surface(state.compositor);
  wl_surface* child = wl_compositor_create_surface(state.compositor);
  wl_subsurface* subsurface = wl_subcompositor_get_subsurface(state.subcompositor, child, parent);
  wl_subsurface_set_position(subsurface, 10, 10);
  xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, parent);
  xdg_surface_add_listener(xdgSurface, &kSurfaceListener, &state);
  xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
  xdg_toplevel_add_listener(toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(toplevel, "sync-subsurface");
  wl_surface_commit(parent);
  while (!state.configured && wl_display_dispatch(display) >= 0) {
  }

  Buffer parentBuffer;
  createBuffer(state, parentBuffer, 200, 150, 0xFF336699);
  wl_surface_attach(parent, parentBuffer.resource, 0, 0);
  wl_surface_commit(parent);
  wl_display_roundtrip(display);
  std::puts("mapped");
  std::fflush(stdout);

  Buffer first;
  Buffer middle;
  Buffer last;
  createBuffer(state, first, 100, 100, 0xFFFF0000);
  createBuffer(state, middle, 100, 100, 0xFF00FF00);
  createBuffer(state, last, 100, 100, 0xFF0000FF);
  for (Buffer* buffer : {&first, &middle, &last}) {
    wl_surface_attach(child, buffer->resource, 0, 0);
    wl_surface_damage_buffer(child, 0, 0, 100, 100);
    wl_surface_commit(child);
  }
  wl_display_roundtrip(display);
  std::printf("queued middle=%s\n", word(middle));
  std::fflush(stdout);

  wl_surface_commit(parent);
  wl_display_roundtrip(display);
  std::printf("applied first=%s middle=%s\n", word(first), word(middle));
  std::fflush(stdout);

  while (wl_display_dispatch(display) >= 0) {
  }
  return 0;
}
