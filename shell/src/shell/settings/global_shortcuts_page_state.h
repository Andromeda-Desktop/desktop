#pragma once

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Settings > Privacy > Global Shortcuts: the page's data and edit state, without I/O. The window runs the actions
// handle() returns (compositor IPC is synchronous) and feeds their results back as events.
namespace settings::global_shortcuts {

  struct Shortcut {
    std::string id;
    std::string description;
    std::string trigger;  // "Ctrl+F9"; empty when the shortcut has no key
    enum class Source : unsigned char { None, Config, Stored } source = Source::None;
    enum class Conflict : unsigned char { None, Unassigned, Disabled, Reserved, InUse } conflict = Conflict::None;
    std::string holder;   // InUse: the app whose shortcut has the trigger, when the reply shows it
  };

  struct App {
    std::string id;
    bool running = false;
    std::vector<Shortcut> shortcuts;
    [[nodiscard]] std::size_t notWorking() const;
  };

  // The `ok` object of the compositor's `global-shortcuts` reply; nullopt when it has the wrong shape.
  [[nodiscard]] std::optional<std::vector<App>> parseApps(const nlohmann::json& ok);

  enum class Load : unsigned char { Loading, Ready, Unavailable };

  struct Idle {};
  struct Recording {
    std::string app;
    std::string id;
    std::string pending;  // the captured chord waiting for its Assigned result
    std::string refused;  // the last refused chord, empty before one
    std::string holder;   // the app holding `refused`; empty when a desktop keybind holds it
  };
  struct Confirm {
    std::string app;  // empty: every app
  };
  using Edit = std::variant<Idle, Recording, Confirm>;

  // Results of the window's actions and the user's input.
  struct Loaded {
    std::optional<std::vector<App>> apps;  // nullopt: no reply
  };
  struct Record {
    std::string app;
    std::string id;
  };
  struct Captured {
    std::string chord;  // empty: Esc
  };
  struct Assigned {
    enum class Result : unsigned char { Ok, Reserved, InUse, Failed } result;
    std::string holder;
  };
  struct AskRemove {
    std::string app;
  };
  struct Answer {
    bool remove;
  };
  struct Removed {
    bool ok;
  };
  struct Leave {};
  using Event = std::variant<Loaded, Record, Captured, Assigned, AskRemove, Answer, Removed, Leave>;

  struct Capture {};
  struct CancelCapture {};
  struct Assign {
    std::string app;
    std::string id;
    std::string chord;  // empty: turn off
  };
  struct Revoke {
    std::string app;  // empty: every app
  };
  struct Reload {};
  using Action = std::variant<Capture, CancelCapture, Assign, Revoke, Reload>;

  struct State {
    Load load = Load::Loading;
    std::vector<App> apps;
    Edit edit = Idle{};
    // The last write that failed; the next successful write clears it.
    struct Failure {
      enum class Kind : unsigned char { Save, Remove, RemoveAll } kind;
      std::string app;  // empty for RemoveAll
    };
    std::optional<Failure> failure;

    // Applies one event; returns the actions to run, in order. An event the current state does not accept changes
    // nothing and returns no action.
    std::vector<Action> handle(Event event);
  };

  // A captured chord that means "turn the shortcut off".
  inline constexpr std::string_view kTurnOffChord = "BackSpace";

} // namespace settings::global_shortcuts
