// Maps an xdg toplevel that redraws on every wl_surface.frame callback, the way a game or a browser does, and prints
// "frame <ms>" (the callback's time) for each callback plus "suspended" / "resumed" when the xdg_toplevel suspended
// state changes. With CONTENT_TYPE=game it declares the game content type before its first commit. With FORK_CHILD=1 it
// first forks an idle child, prints "child <pid>" and stops itself (SIGSTOP) until the test moves it to another scope
// and sends SIGCONT, as a browser forks its zygote before it moves into its own scope. WM_BASE_VERSION binds xdg_wm_base
// at most that version (5 makes a client that cannot be suspended). While the file named by STOP_FILE exists, it
// requests no further callback and prints "stopped". It binds dsk_surface_v1 and prints "presence <value>" for each
// dsk_surface_v1.presence event. With CHILDREN=1 it also maps a subsurface and an xdg popup, each with its own
// dsk_surface_v1, and prints "sub-presence <value>" and "popup-presence <value>" for their presence events.
// Usage: background-client [title].

#include "content-type-v1-client-protocol.h"
#include "dsk-surface-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {
  constexpr int kSize = 64;

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wp_content_type_manager_v1* contentTypeManager = nullptr;
    dsk_surface_manager_v1* dskManager = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_buffer* buffer = nullptr;
    int width = kSize;
    int height = kSize;
    // FILL_CONFIGURE=1: draw at the configured size, as an app fills its tile.
    int configuredWidth = 0;
    int configuredHeight = 0;
    bool mapped = false;
    bool suspended = false;
    wl_surface* popupSurface = nullptr;
  };

  void addDskListener(State& state, wl_surface* surface, const char* label);

  void requestFrame(State& state);
  bool createBuffer(State& state);

  void frameDone(void* data, wl_callback* callback, uint32_t timeMs) {
    wl_callback_destroy(callback);
    auto& state = *static_cast<State*>(data);
        std::println("frame {}", timeMs);
    if (const char* stop = std::getenv("STOP_FILE"); stop != nullptr && access(stop, F_OK) == 0) {
      std::println("stopped");
      std::fflush(stdout);
      return;
    }
    std::fflush(stdout);
    requestFrame(state);
  }
  constexpr wl_callback_listener kFrameListener = {.done = frameDone};

  void requestFrame(State& state) {
    wl_callback_add_listener(wl_surface_frame(state.surface), &kFrameListener, &state);
    wl_surface_attach(state.surface, state.buffer, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, state.width, state.height);
    wl_surface_commit(state.surface);
  }

    void wmBasePing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }

  uint32_t wmBaseVersion() {
    const char* value = std::getenv("WM_BASE_VERSION");
    return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 6U;
  }
  constexpr xdg_wm_base_listener kWmBaseListener = {.ping = wmBasePing};

  void popupConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(surface, serial);
    wl_surface_attach(state.popupSurface, state.buffer, 0, 0);
    wl_surface_commit(state.popupSurface);
  }
  constexpr xdg_surface_listener kPopupSurfaceListener = {.configure = popupConfigure};

  void mapChildren(State& state) {
    wl_surface* child = wl_compositor_create_surface(state.compositor);
    wl_subsurface* subsurface = wl_subcompositor_get_subsurface(state.subcompositor, child, state.surface);
    wl_subsurface_set_position(subsurface, kSize / 2, kSize / 2);
    addDskListener(state, child, "sub-presence");
    wl_surface_attach(child, state.buffer, 0, 0);
    wl_surface_commit(child);

    state.popupSurface = wl_compositor_create_surface(state.compositor);
    xdg_surface* popupXdg = xdg_wm_base_get_xdg_surface(state.wmBase, state.popupSurface);
    xdg_surface_add_listener(popupXdg, &kPopupSurfaceListener, &state);
    xdg_positioner* positioner = xdg_wm_base_create_positioner(state.wmBase);
    xdg_positioner_set_size(positioner, kSize, kSize);
    xdg_positioner_set_anchor_rect(positioner, 0, 0, kSize, kSize);
    xdg_surface_get_popup(popupXdg, state.xdgSurface, positioner);
    xdg_positioner_destroy(positioner);
    addDskListener(state, state.popupSurface, "popup-presence");
    wl_surface_commit(state.popupSurface);
    wl_surface_commit(state.surface);
  }

  void xdgConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(surface, serial);
    if (state.configuredWidth > 0
        && state.configuredHeight > 0
        && (state.configuredWidth != state.width || state.configuredHeight != state.height)) {
      state.width = state.configuredWidth;
      state.height = state.configuredHeight;
      wl_buffer_destroy(state.buffer);
      if (!createBuffer(state)) {
        std::println(stderr, "cannot create a {}x{} buffer", state.width, state.height);
        std::exit(1);
      }
    }
    if (state.mapped) {
      return;
    }
    state.mapped = true;
    requestFrame(state);
    std::println("mapped");
    std::fflush(stdout);
    if (const char* children = std::getenv("CHILDREN"); children != nullptr && std::strcmp(children, "1") == 0) {
      mapChildren(state);
    }
  }
  constexpr xdg_surface_listener kXdgListener = {.configure = xdgConfigure};

  void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array* states) {
    auto& state = *static_cast<State*>(data);
    if (const char* fill = std::getenv("FILL_CONFIGURE"); fill != nullptr && std::strcmp(fill, "1") == 0) {
      state.configuredWidth = width;
      state.configuredHeight = height;
    }
    const auto* configured = static_cast<const uint32_t*>(states->data);
    const bool suspended =
        std::ranges::find(configured, configured + states->size / sizeof(uint32_t), XDG_TOPLEVEL_STATE_SUSPENDED)
        != configured + states->size / sizeof(uint32_t);
    if (suspended != state.suspended) {
      state.suspended = suspended;
      std::println("{}", suspended ? "suspended" : "resumed");
      std::fflush(stdout);
    }
  }
  void toplevelClose(void*, xdg_toplevel*) {}
  void toplevelBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
  void toplevelCapabilities(void*, xdg_toplevel*, wl_array*) {}
  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = toplevelBounds,
      .wm_capabilities = toplevelCapabilities,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0) {
      state.subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase = static_cast<xdg_wm_base*>(
          wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, wmBaseVersion()))
      );
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    } else if (std::strcmp(interface, dsk_surface_manager_v1_interface.name) == 0 && version >= 20) {
      state.dskManager =
          static_cast<dsk_surface_manager_v1*>(wl_registry_bind(registry, name, &dsk_surface_manager_v1_interface, 20));
    } else if (std::strcmp(interface, wp_content_type_manager_v1_interface.name) == 0) {
      state.contentTypeManager = static_cast<wp_content_type_manager_v1*>(
          wl_registry_bind(registry, name, &wp_content_type_manager_v1_interface, 1)
      );
    }
  }
  void registryRemove(void*, wl_registry*, uint32_t) {}
  const dsk_surface_v1_listener kDskListener = {
      .started = [](void*, dsk_surface_v1*, uint32_t, uint32_t) {},
      .done = [](void*, dsk_surface_v1*, uint32_t, uint32_t) {},
      .clip_mapped = [](void*, dsk_surface_v1*, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t) {},
      .backdrop = [](void*, dsk_surface_v1*, uint32_t) {},
      .density = [](void*, dsk_surface_v1*, wl_fixed_t) {},
      .presence = [](void* label, dsk_surface_v1*, uint32_t presence) {
        std::println("{} {}", static_cast<const char*>(label), presence);
        std::fflush(stdout);
      },
  };
  void addDskListener(State& state, wl_surface* surface, const char* label) {
    if (state.dskManager != nullptr) {
      dsk_surface_v1_add_listener(
          dsk_surface_manager_v1_get_surface(state.dskManager, surface), &kDskListener, const_cast<char*>(label)
      );
    }
  }

  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryRemove};

  bool createBuffer(State& state) {
    const int stride = state.width * 4;
    const size_t size = static_cast<size_t>(stride) * state.height;
    const int fd = memfd_create("andromeda-background-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
      return false;
    }
    void* pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
      close(fd);
      return false;
    }
    std::fill_n(static_cast<uint32_t*>(pixels), size / sizeof(uint32_t), 0xFF55AA77);
    munmap(pixels, size);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    state.buffer = wl_shm_pool_create_buffer(pool, 0, state.width, state.height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return state.buffer != nullptr;
  }
} // namespace

int main(int argc, char** argv) {
  if (const char* fork = std::getenv("FORK_CHILD"); fork != nullptr && std::strcmp(fork, "1") == 0) {
    const pid_t child = ::fork();
    if (child == 0) {
      prctl(PR_SET_PDEATHSIG, SIGTERM);
      for (;;) {
        pause();
      }
    }
    std::println("child {}", child);
    std::fflush(stdout);
    raise(SIGSTOP);
  }
  State state;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "background-client: cannot connect");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  if (state.compositor == nullptr || state.shm == nullptr || state.wmBase == nullptr || !createBuffer(state)) {
    std::println(stderr, "background-client: missing required global or buffer");
    return EXIT_FAILURE;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  const char* contentType = std::getenv("CONTENT_TYPE");
  if (contentType != nullptr && std::strcmp(contentType, "game") == 0) {
    if (state.contentTypeManager == nullptr) {
      std::println(stderr, "background-client: no content-type manager");
      return EXIT_FAILURE;
    }
    wp_content_type_v1_set_content_type(
        wp_content_type_manager_v1_get_surface_content_type(state.contentTypeManager, state.surface),
        WP_CONTENT_TYPE_V1_TYPE_GAME
    );
  }
  addDskListener(state, state.surface, "presence");
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, argc > 1 ? argv[1] : "background-client");
  wl_surface_commit(state.surface);

  while (wl_display_dispatch(state.display) >= 0) {
  }
  return EXIT_SUCCESS;
}
