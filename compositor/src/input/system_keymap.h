#pragma once

#include "core/loop_bus.h"
#include "input/keyboard_layout.h"

#include <functional>

struct wl_event_loop;

namespace andromeda {

  // Reads the X11 keymap systemd-localed reports (`localectl set-x11-keymap`) over the compositor's system bus.
  class SystemKeymapQuery {
  public:
    explicit SystemKeymapQuery(wl_event_loop* loop);

    // `done` gets the names, empty when localed is unreachable or reports no keymap. It runs once, never after this
    // object died.
    void query(std::function<void(XkbNames)> done);

  private:
    bus::Client m_bus;
  };

  // `names`, or empty when libxkbcommon cannot compile them. Compiles a keymap: run it off the event loop.
  [[nodiscard]] XkbNames validSystemKeymap(XkbNames names);

} // namespace andromeda
