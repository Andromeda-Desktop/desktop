#include "launcher/dmenu_socket_path.h"
#include "protocols/xdg_dirs.h"

#include <cstdlib>

namespace noctalia::launcher {

  DmenuSocketPathResult resolveDmenuSocketPath() {
    const std::string runtime = xdg::runtimeDir();
    if (runtime.empty()) {
      return {.path = {}, .error = "XDG_RUNTIME_DIR is not set"};
    }

    const char* display = std::getenv("WAYLAND_DISPLAY");
    if (display == nullptr || display[0] == '\0') {
      return {.path = {}, .error = "WAYLAND_DISPLAY is not set"};
    }

    return {.path = runtime + "/noctalia-dmenu-" + display + ".sock", .error = {}};
  }

} // namespace noctalia::launcher
