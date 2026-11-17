#include "shell/settings/global_shortcuts_page_state.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace settings::global_shortcuts {
  namespace {
    template <class... F> struct Overload : F... {
      using F::operator()...;
    };

    Shortcut::Source parseSource(const std::string& text) {
      return text == "config" ? Shortcut::Source::Config
             : text == "stored" ? Shortcut::Source::Stored
                                : Shortcut::Source::None;
    }

    Shortcut::Conflict parseConflict(const std::string& text) {
      using enum Shortcut::Conflict;
      return text == "none"       ? None
             : text == "disabled" ? Disabled
             : text == "reserved" ? Reserved
             : text == "in_use"   ? InUse
                                  : Unassigned;
    }

    App& appFor(std::vector<App>& apps, const std::string& id) {
      const auto found = std::ranges::find(apps, id, &App::id);
      return found != apps.end() ? *found : apps.emplace_back(App{.id = id});
    }

    bool hasShortcut(const std::vector<App>& apps, const std::string& app, const std::string& id) {
      const auto found = std::ranges::find(apps, app, &App::id);
      return found != apps.end() && std::ranges::contains(found->shortcuts, id, &Shortcut::id);
    }

    bool hasApp(const std::vector<App>& apps, const std::string& app) {
      return app.empty() || std::ranges::contains(apps, app, &App::id);
    }
  } // namespace

  std::size_t App::notWorking() const {
    return static_cast<std::size_t>(std::ranges::count_if(
        shortcuts, [](const Shortcut& shortcut) { return shortcut.conflict != Shortcut::Conflict::None; }
    ));
  }

  std::optional<std::vector<App>> parseApps(const nlohmann::json& ok) {
    if (!ok.is_object() || !ok.contains("sessions") || !ok["sessions"].is_array()) {
      return std::nullopt;
    }
    std::vector<App> apps;
    try {
      for (const auto& session : ok["sessions"]) {
        App& app = appFor(apps, session.at("app").get<std::string>());
        const std::string phase = session.value("phase", "");
        app.running = app.running || phase == "bound" || phase == "approving";
        for (const auto& item : session.at("shortcuts")) {
          const std::string id = item.at("id").get<std::string>();
          if (std::ranges::contains(app.shortcuts, id, &Shortcut::id)) {
            continue;
          }
          const auto& trigger = item.at("trigger");
          app.shortcuts.push_back(Shortcut{
              .id = id,
              .description = item.value("description", ""),
              .trigger = trigger.is_string() ? trigger.get<std::string>() : std::string{},
              .source = parseSource(item.value("source", "")),
              .conflict = parseConflict(item.value("conflict", "")),
          });
        }
      }
      // Approved apps without a running session: their stored chords, which the compositor binds when they return.
      if (const auto grants = ok.find("grants"); grants != ok.end() && grants->is_object()) {
        for (const auto& [appId, chords] : grants->items()) {
          App& app = appFor(apps, appId);
          for (const auto& [id, chord] : chords.items()) {
            if (!std::ranges::contains(app.shortcuts, id, &Shortcut::id)) {
              app.shortcuts.push_back(Shortcut{
                  .id = id, .description = {}, .trigger = chord.get<std::string>(),
                  .source = Shortcut::Source::Stored, .conflict = Shortcut::Conflict::None
              });
            }
          }
        }
      }
    } catch (const nlohmann::json::exception&) {
      return std::nullopt;
    }
    // InUse: the reply does not name the holder; it is the live shortcut elsewhere with the same keys.
    for (App& app : apps) {
      for (Shortcut& shortcut : app.shortcuts) {
        if (shortcut.conflict != Shortcut::Conflict::InUse) {
          continue;
        }
        for (const App& other : apps) {
          const bool holds = std::ranges::any_of(other.shortcuts, [&](const Shortcut& candidate) {
            return &candidate != &shortcut && candidate.conflict == Shortcut::Conflict::None
                   && candidate.trigger == shortcut.trigger;
          });
          if (holds) {
            shortcut.holder = other.id;
            break;
          }
        }
      }
    }
    std::ranges::sort(apps, {}, &App::id);
    return apps;
  }

  std::vector<Action> State::handle(Event event) {
    return std::visit(
        Overload{
            [this](Loaded& loaded) -> std::vector<Action> {
              load = loaded.apps ? Load::Ready : Load::Unavailable;
              apps = loaded.apps ? std::move(*loaded.apps) : std::vector<App>{};
              if (const auto* recording = std::get_if<Recording>(&edit);
                  recording != nullptr && !hasShortcut(apps, recording->app, recording->id)) {
                edit = Idle{};
                return {CancelCapture{}};
              }
              if (const auto* confirm = std::get_if<Confirm>(&edit); confirm != nullptr && !hasApp(apps, confirm->app)) {
                edit = Idle{};
              }
              return {};
            },
            [this](Record& record) -> std::vector<Action> {
              if (auto* recording = std::get_if<Recording>(&edit)) {
                if (!recording->pending.empty()) {
                  return {};
                }
                const bool same = recording->app == record.app && recording->id == record.id;
                edit = Idle{};
                if (same || !hasShortcut(apps, record.app, record.id)) {
                  return {CancelCapture{}};
                }
                edit = Recording{.app = std::move(record.app), .id = std::move(record.id), .pending = {}, .refused = {},
                                 .holder = {}};
                return {CancelCapture{}, Capture{}};
              }
              if (!std::holds_alternative<Idle>(edit) || load != Load::Ready
                  || !hasShortcut(apps, record.app, record.id)) {
                return {};
              }
              edit = Recording{.app = std::move(record.app), .id = std::move(record.id), .pending = {}, .refused = {},
                               .holder = {}};
              return {Capture{}};
            },
            [this](Captured& captured) -> std::vector<Action> {
              auto* recording = std::get_if<Recording>(&edit);
              if (recording == nullptr || !recording->pending.empty()) {
                return {};
              }
              if (captured.chord.empty()) {
                edit = Idle{};
                return {};
              }
              recording->pending = std::move(captured.chord);
              const std::string chord = recording->pending == kTurnOffChord ? std::string{} : recording->pending;
              return {Assign{.app = recording->app, .id = recording->id, .chord = chord}};
            },
            [this](Assigned& assigned) -> std::vector<Action> {
              auto* recording = std::get_if<Recording>(&edit);
              if (recording == nullptr || recording->pending.empty()) {
                return {};
              }
              using enum Assigned::Result;
              switch (assigned.result) {
              case Ok:
                edit = Idle{};
                failure.reset();
                return {Reload{}};
              case Reserved:
              case InUse:
                recording->refused = std::exchange(recording->pending, {});
                recording->holder = assigned.result == InUse ? std::move(assigned.holder) : std::string{};
                return {Capture{}};
              case Failed:
                break;
              }
              failure = Failure{.kind = Failure::Kind::Save, .app = recording->app};
              edit = Idle{};
              return {};
            },
            [this](AskRemove& ask) -> std::vector<Action> {
              if (!std::holds_alternative<Idle>(edit) || load != Load::Ready || apps.empty() || !hasApp(apps, ask.app)) {
                return {};
              }
              edit = Confirm{.app = std::move(ask.app)};
              return {};
            },
            [this](Answer& answer) -> std::vector<Action> {
              const auto* confirm = std::get_if<Confirm>(&edit);
              if (confirm == nullptr) {
                return {};
              }
              if (!answer.remove) {
                edit = Idle{};
                return {};
              }
              return {Revoke{.app = confirm->app}};
            },
            [this](Removed& removed) -> std::vector<Action> {
              const auto* confirm = std::get_if<Confirm>(&edit);
              if (confirm == nullptr) {
                return {};
              }
              if (removed.ok) {
                failure.reset();
              } else {
                failure = Failure{.kind = confirm->app.empty() ? Failure::Kind::RemoveAll : Failure::Kind::Remove,
                                  .app = confirm->app};
              }
              edit = Idle{};
              return {Reload{}};
            },
            [this](Leave&) -> std::vector<Action> {
              const bool recording = std::holds_alternative<Recording>(edit);
              edit = Idle{};
              load = Load::Loading;
              failure.reset();
              return recording ? std::vector<Action>{CancelCapture{}} : std::vector<Action>{};
            },
        },
        event
    );
  }

} // namespace settings::global_shortcuts
