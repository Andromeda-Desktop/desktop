#include "server/client_trust.h"

#include "andromeda_data_dir.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace andromeda {

  namespace {

    // Kinds of privileged global; each grant is a set of kinds.
    enum Kind : uint16_t {
      kShell = 1U << 0, // dsk_shell_v1, dsk_settings_manager_v1
      kLock = 1U << 1,
      kWindowList = 1U << 2,
      kWindowControl = 1U << 3,
      kCapture = 1U << 4,
      kScreencopy = 1U << 5,
      kClipboard = 1U << 6,
      kInjectInput = 1U << 7,
      kInputCapture = 1U << 8,
      kOutputControl = 1U << 9,
      kRemoteInput = 1U << 10, // RemoteDesktop sessions; the compositor asks the user for each app
    };

    struct Privileged {
      std::string_view interface;
      uint16_t kind;
    };

    constexpr std::array<Privileged, 19> kPrivileged{{
        {"dsk_shell_v1", kShell},
        {"dsk_settings_manager_v1", kShell},
        {"ext_session_lock_manager_v1", kLock},
        {"ext_foreign_toplevel_list_v1", kWindowList},
        {"zwlr_foreign_toplevel_manager_v1", kWindowControl},
        {"ext_image_copy_capture_manager_v1", kCapture},
        {"ext_output_image_capture_source_manager_v1", kCapture},
        {"ext_foreign_toplevel_image_capture_source_manager_v1", kCapture},
        {"zwlr_screencopy_manager_v1", kScreencopy},
        {"zwlr_data_control_manager_v1", kClipboard},
        {"ext_data_control_manager_v1", kClipboard},
        {"zwp_virtual_keyboard_manager_v1", kInjectInput},
        {"zwlr_virtual_pointer_manager_v1", kInjectInput},
        {"andromeda_input_capture_manager_v1", kInputCapture},
        {"andromeda_remote_input_manager_v1", kRemoteInput},
        {"dsk_output_manager_v1", kOutputControl},
        {"zwlr_output_manager_v1", kOutputControl},
        {"zwlr_gamma_control_manager_v1", kOutputControl},
        {"zwlr_output_power_manager_v1", kOutputControl},
    }};

    constexpr uint16_t kAll = (1U << 11) - 1;

    constexpr uint16_t kindsOf(Grant grant) {
      switch (grant) {
        case Grant::Shell:
        case Grant::Test: return kAll;
        // The login screen reads appearance and shows itself; nothing that sees or drives the session.
        case Grant::Greeter: return kShell | kLock;
        // The portal lists windows and outputs, captures the one the user chose, and bridges RemoteDesktop and its
        // clipboard; the portal, not the compositor, limits each session's clipboard to its consent and unpaused time.
        case Grant::Portal:
          return kWindowList | kCapture | kRemoteInput | kClipboard;
        // Link syncs the clipboard with the phone and lets the phone drive the pointer and keyboard.
        case Grant::Link: return kClipboard | kInjectInput;
        // A tool the user granted: everything but the shell's own protocols.
        case Grant::User: return kAll & ~(kShell | kLock);
        case Grant::None: return 0;
      }
      return 0;
    }

    const Privileged* find(std::string_view interface) {
      const auto* it = std::ranges::find(kPrivileged, interface, &Privileged::interface);
      return it == kPrivileged.end() ? nullptr : it;
    }

    bool listed(std::string_view exe, std::span<const std::string> paths) {
      return std::ranges::any_of(paths, [exe](const std::string& path) {
        return path.ends_with('/') ? exe.starts_with(path) : exe == path;
      });
    }

  } // namespace

  namespace {
    std::vector<std::string> g_testBinaries;
  } // namespace

  void addTestBinary(std::string path) { g_testBinaries.push_back(std::move(path)); }
  std::span<const std::string> testBinaries() { return g_testBinaries; }

  bool privilegedGlobal(std::string_view interface) { return find(interface) != nullptr; }

  bool grantAllows(Grant grant, std::string_view interface) {
    const Privileged* privileged = find(interface);
    return privileged == nullptr || (kindsOf(grant) & privileged->kind) != 0;
  }

  Grant grantFor(const TrustInputs& in) {
    if (in.exe.empty()) {
      return Grant::None;
    }
    const std::string bin(kBinDir);
    // The shell, its lock and the greeter are one binary; greeter mode decides which set it gets.
    if (in.exe == bin + "/andromeda-shell") {
      return in.greeterMode ? Grant::Greeter : Grant::Shell;
    }
    if (listed(in.exe, in.testBinaries)) {
      return in.greeterMode ? Grant::Greeter : Grant::Test;
    }
    if (in.greeterMode) {
      return Grant::None;
    }
    if (in.exe == std::string(kLibexecDir) + "/xdg-desktop-portal-andromeda") {
      return Grant::Portal;
    }
    if (in.exe == bin + "/andromeda-link-session") {
      return Grant::Link;
    }
    return listed(in.exe, in.userBinaries) ? Grant::User : Grant::None;
  }

  std::string_view stripDeleted(std::string_view exe) {
    constexpr std::string_view kDeleted = " (deleted)";
    return exe.ends_with(kDeleted) ? exe.substr(0, exe.size() - kDeleted.size()) : exe;
  }

} // namespace andromeda
