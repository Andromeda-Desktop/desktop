#include "andromeda_data_dir.h"
#include "check.h"
#include "server/client_trust.h"

#include <array>
#include <string>
#include <vector>

using namespace andromeda;

namespace {

  constexpr std::array<std::string_view, 19> kPrivilegedNames{
      "dsk_shell_v1", "dsk_settings_manager_v1", "ext_session_lock_manager_v1", "zwlr_foreign_toplevel_manager_v1",
      "ext_foreign_toplevel_list_v1", "zwlr_screencopy_manager_v1", "ext_image_copy_capture_manager_v1",
      "ext_output_image_capture_source_manager_v1", "ext_foreign_toplevel_image_capture_source_manager_v1",
      "zwlr_data_control_manager_v1", "ext_data_control_manager_v1", "zwp_virtual_keyboard_manager_v1",
      "zwlr_virtual_pointer_manager_v1", "andromeda_input_capture_manager_v1", "andromeda_remote_input_manager_v1", "dsk_output_manager_v1",
      "zwlr_output_manager_v1", "zwlr_gamma_control_manager_v1", "zwlr_output_power_manager_v1",
  };
  constexpr std::array<std::string_view, 5> kOrdinaryNames{"wl_compositor", "xdg_wm_base", "wl_seat",
                                                           "zwlr_layer_shell_v1", "wp_security_context_manager_v1"};

  bool in(std::string_view name, std::initializer_list<std::string_view> set) {
    return std::ranges::find(set, name) != set.end();
  }

  // The design table (TASK-ar8.md), written out by name.
  bool expected(Grant grant, std::string_view name) {
    const bool shellOnly = in(name, {"dsk_shell_v1", "dsk_settings_manager_v1", "ext_session_lock_manager_v1"});
    switch (grant) {
      case Grant::Shell:
      case Grant::Test: return true;
      case Grant::Greeter: return shellOnly;
      case Grant::Portal:
        return in(
            name,
            {"ext_foreign_toplevel_list_v1", "ext_image_copy_capture_manager_v1",
             "ext_output_image_capture_source_manager_v1", "ext_foreign_toplevel_image_capture_source_manager_v1",
             "andromeda_remote_input_manager_v1", "zwlr_data_control_manager_v1", "ext_data_control_manager_v1"}
        );
      case Grant::Link:
        return in(name, {"zwlr_data_control_manager_v1", "ext_data_control_manager_v1", "zwp_virtual_keyboard_manager_v1",
                         "zwlr_virtual_pointer_manager_v1"});
      case Grant::User: return !shellOnly;
      case Grant::None: return false;
    }
    return false;
  }

} // namespace

// Every grant against every global: a privileged global shows iff the design table lists it; others always show.
ANDROMEDA_TEST(everyGrantSeesExactlyItsPrivilegedGlobals) {
  for (const Grant grant : {Grant::None, Grant::Shell, Grant::Greeter, Grant::Portal, Grant::Link, Grant::User,
                            Grant::Test}) {
    for (const std::string_view name : kPrivilegedNames) {
      CHECK(privilegedGlobal(name));
      CHECK(grantAllows(grant, name) == expected(grant, name));
    }
    for (const std::string_view name : kOrdinaryNames) {
      CHECK(!privilegedGlobal(name));
      CHECK(grantAllows(grant, name));
    }
  }
}

// Every binary class in and out of greeter mode: the greeter session grants only the greeter set, to the shell binary
// and test binaries; an unreadable identity or an unknown binary gets nothing.
ANDROMEDA_TEST(everyBinaryGetsItsGrant) {
  const std::string bin(kBinDir);
  const std::vector<std::string> user{"/usr/bin/grim"};
  const std::vector<std::string> test{"/src/build/tests/", "/usr/bin/wtype"};
  struct Case {
    std::string exe;
    Grant session;
    Grant greeter;
  };
  const std::vector<Case> cases{
      {bin + "/andromeda-shell", Grant::Shell, Grant::Greeter},
      {std::string(kLibexecDir) + "/xdg-desktop-portal-andromeda", Grant::Portal, Grant::None},
      {bin + "/andromeda-link-session", Grant::Link, Grant::None},
      {"/usr/bin/grim", Grant::User, Grant::None},
      {"/src/build/tests/global-client", Grant::Test, Grant::Greeter},
      {"/usr/bin/wtype", Grant::Test, Grant::Greeter},
      {"/src/build/testsX/global-client", Grant::None, Grant::None},
      {"/tmp/andromeda-shell", Grant::None, Grant::None},
      {"", Grant::None, Grant::None},
  };
  for (const Case& c : cases) {
    for (const bool greeter : {false, true}) {
      const Grant grant = grantFor({.exe = c.exe, .greeterMode = greeter, .userBinaries = user, .testBinaries = test});
      CHECK(grant == (greeter ? c.greeter : c.session));
    }
  }
}

ANDROMEDA_TEST(deletedBinaryKeepsItsPath) {
  CHECK(stripDeleted("/usr/bin/andromeda-shell (deleted)") == "/usr/bin/andromeda-shell");
  CHECK(stripDeleted("/usr/bin/andromeda-shell") == "/usr/bin/andromeda-shell");
}

int main() { return RUN_TESTS(); }
