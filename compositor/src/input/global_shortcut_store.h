#pragma once

#include "input/global_shortcuts.h"
#include "util/private_state_file.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace andromeda {

  // The approved chords, as {"version":1,"grants":{"<app>":{"<id>":"Ctrl+Alt+M"}}} in an owner-only file.
  class GlobalShortcutStore {
  public:
    // Loaded: the file was valid or missing. Unreadable: unsafe or malformed; the store never writes it.
    enum class Status : uint8_t { Unloaded, Loaded, Unreadable };

    using Done = std::function<void(std::optional<ShortcutGrants> merged)>;

    GlobalShortcutStore(wl_event_loop* loop, std::filesystem::path path);
    // $XDG_STATE_HOME/andromeda/global-shortcuts.json.
    [[nodiscard]] static std::filesystem::path defaultPath();

    // nullopt when the file is unsafe or malformed; a missing file is no grants.
    std::optional<ShortcutGrants> load();
    // On the saver thread, in request order and under the lock: re-reads the file so other compositor instances'
    // grants survive, replaces `app`'s grants (an empty map removes the app; an empty app with an empty map removes
    // every app) and writes it. `done` gets the merged grants; nullopt when not Loaded, the file turned unreadable,
    // a cap is exceeded, or the lock or write fails.
    void update(std::string_view app, std::map<std::string, ShortcutChord> shortcuts, Done done);

    [[nodiscard]] Status status() const { return m_status; }
    [[nodiscard]] const std::filesystem::path& path() const { return m_saver.file(); }

  private:
    Status m_status = Status::Unloaded;
    private_state::Saver m_saver;
  };

  // Keeps a registry's grants and the store's file in step while saves queue up. Memory changes first (sessions
  // lose a revoked shortcut at once); a finished save hands memory the file's grants, except for apps with a later
  // change queued or a failed approval, which keep their live value. After a revoke, saved or not, memory takes the
  // last file content it saw for the revoked apps.
  class ShortcutGrantSync {
  public:
    using Done = std::function<void(bool saved)>;

    ShortcutGrantSync(ShortcutRegistry& registry, wl_event_loop* loop, std::filesystem::path path);
    // Loads the file into the registry; false when it is unsafe or malformed (nothing is saved then).
    bool load();
    // Saves the registry's current grants of `app`.
    void persist(const std::string& app, Done done);
    // Drops `app`'s grants (every app when empty) from the registry, then from the file. Its bound sessions unbind at
    // once and stay unbound when the save fails; the grants come back and the app binds again.
    void revoke(std::string_view app, Done done);
    [[nodiscard]] const std::filesystem::path& path() const { return m_store.path(); }

  private:
    void save(const std::string& app, std::map<std::string, ShortcutChord> grants, Done done);

    ShortcutRegistry& m_registry;
    GlobalShortcutStore m_store;
    // Saves queued per app ("": a revoke of all).
    std::map<std::string, int> m_saving;
    // The file's grants as last loaded or saved.
    ShortcutGrants m_file;
    // Apps whose last approval failed to save: memory keeps their live grants over the file's.
    std::set<std::string> m_unsaved;
  };

} // namespace andromeda
