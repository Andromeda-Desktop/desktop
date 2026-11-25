// Drives andromeda_remote_input_v1 from stdin commands and reports events on stdout, one line each:
//   start <persist_mode> <restore_token|->  connect  move <dx> <dy>  quit
#include <andromeda-remote-input-v1-client-protocol.h>
#include <cstdint>
#include <cstring>
#include <libei.h>
#include <poll.h>
#include <print>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace {

  struct State {
    andromeda_remote_input_manager_v1* manager = nullptr;
    andromeda_remote_input_session_v1* session = nullptr;
    ei* context = nullptr;
    std::vector<ei_device*> devices;
    uint32_t sequence = 0;
    bool done = false;
  };

  void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, andromeda_remote_input_manager_v1_interface.name) == 0) {
      state.manager = static_cast<andromeda_remote_input_manager_v1*>(
          wl_registry_bind(registry, name, &andromeda_remote_input_manager_v1_interface, 1)
      );
    }
  }
  void globalRemove(void*, wl_registry*, uint32_t) {}
  const wl_registry_listener kRegistry{.global = global, .global_remove = globalRemove};

  void onGrant(void*, andromeda_remote_input_session_v1*, uint32_t, uint32_t mode, const char* token) {
    std::println("grant mode={} token={}", mode, token);
  }
  void onStarted(void*, andromeda_remote_input_session_v1*, uint32_t, uint32_t status, uint32_t capabilities) {
    std::println("started status={} capabilities={}", status, capabilities);
  }
  void onEisFd(void* data, andromeda_remote_input_session_v1*, uint32_t, int32_t fd) {
    auto& state = *static_cast<State*>(data);
    state.context = ei_new_sender(&state);
    ei_configure_name(state.context, "remote-input-client");
    if (ei_setup_backend_fd(state.context, fd) != 0) {
      std::println("eis-failed setup");
      return;
    }
    std::println("eis-fd");
  }
  void onResult(void*, andromeda_remote_input_session_v1*, uint32_t, uint32_t status) {
    std::println("result status={}", status);
  }
  void onClosed(void*, andromeda_remote_input_session_v1*) { std::println("closed"); }
  const andromeda_remote_input_session_v1_listener kSession{
      .grant = onGrant, .started = onStarted, .eis_fd = onEisFd, .result = onResult, .closed = onClosed
  };

  void handleEi(State& state) {
    ei_dispatch(state.context);
    while (ei_event* event = ei_get_event(state.context)) {
      switch (ei_event_get_type(event)) {
        case EI_EVENT_CONNECT: std::println("ei-connected"); break;
        case EI_EVENT_DISCONNECT: std::println("ei-disconnected"); break;
        case EI_EVENT_SEAT_ADDED: {
          ei_seat* seat = ei_event_get_seat(event);
          std::println(
              "seat keyboard={:d} pointer={:d}", ei_seat_has_capability(seat, EI_DEVICE_CAP_KEYBOARD),
              ei_seat_has_capability(seat, EI_DEVICE_CAP_POINTER)
          );
          ei_seat_bind_capabilities(
              seat, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, nullptr
          );
          break;
        }
        case EI_EVENT_DEVICE_ADDED: state.devices.push_back(ei_device_ref(ei_event_get_device(event))); break;
        case EI_EVENT_DEVICE_RESUMED: {
          ei_device* device = ei_event_get_device(event);
          ei_device_start_emulating(device, ++state.sequence);
          if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER)) {
            std::println("pointer-ready");
          }
          break;
        }
        default: break;
      }
      ei_event_unref(event);
    }
  }

  void command(State& state, wl_display* display, const std::string& line) {
    std::istringstream words(line);
    std::string action;
    words >> action;
    if (action == "start") {
      uint32_t mode = 0;
      std::string token;
      words >> mode >> token;
      andromeda_remote_input_session_v1_set_persistence(state.session, mode, token == "-" ? "" : token.c_str(), "instance");
      andromeda_remote_input_session_v1_start(state.session, 1, "org.test.Remote", 3);
    } else if (action == "connect") {
      andromeda_remote_input_session_v1_connect_eis(state.session, 2);
    } else if (action == "move") {
      double dx = 0;
      double dy = 0;
      words >> dx >> dy;
      for (ei_device* device : state.devices) {
        if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER)) {
          ei_device_pointer_motion(device, dx, dy);
          ei_device_frame(device, ei_now(state.context));
          std::println("moved");
        }
      }
    } else if (action == "quit") {
      state.done = true;
    }
    wl_display_flush(display);
  }

} // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println("no-display");
    return 1;
  }
  State state;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistry, &state);
  wl_display_roundtrip(display);
  if (state.manager == nullptr) {
    std::println("no-manager");
    return 3;
  }
  state.session = andromeda_remote_input_manager_v1_create_session(state.manager);
  andromeda_remote_input_session_v1_add_listener(state.session, &kSession, &state);
  wl_display_roundtrip(display);
  std::println("ready");
  std::string pending;
  while (!state.done) {
    wl_display_flush(display);
    pollfd fds[3] = {
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
        {.fd = wl_display_get_fd(display), .events = POLLIN, .revents = 0},
        {.fd = state.context != nullptr ? ei_get_fd(state.context) : -1, .events = POLLIN, .revents = 0},
    };
    if (poll(fds, 3, -1) < 0) {
      return 1;
    }
    if ((fds[1].revents & POLLIN) != 0 && wl_display_dispatch(display) < 0) {
      std::println("display-gone");
      return 0;
    }
    if ((fds[2].revents & POLLIN) != 0) {
      handleEi(state);
    }
    if ((fds[0].revents & (POLLIN | POLLHUP)) != 0) {
      char buffer[512];
      const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
      if (count <= 0) {
        break;
      }
      pending.append(buffer, static_cast<size_t>(count));
      size_t newline = 0;
      while ((newline = pending.find('\n')) != std::string::npos) {
        command(state, display, pending.substr(0, newline));
        pending.erase(0, newline + 1);
      }
    }
  }
  return 0;
}
