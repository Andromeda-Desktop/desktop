#pragma once

#include "input/global_shortcuts.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace andromeda {

  // The approved chords, as {"version":1,"grants":{"<app>":{"<id>":"Ctrl+Alt+M"}}} in an owner-only file.
  class GlobalShortcutStore {
  public:
    // Loaded: the file was valid or missing. Unreadable: unsafe or malformed; the store never writes it.
    enum class Status : uint8_t { Unloaded, Loaded, Unreadable };

    explicit GlobalShortcutStore(std::filesystem::path path);
    // $XDG_STATE_HOME/andromeda/global-shortcuts.json.
    [[nodiscard]] static std::filesystem::path defaultPath();

    // nullopt when the file is unsafe or malformed; a missing file is no grants.
    std::optional<ShortcutGrants> load();
    // Under the lock, re-reads the file so other compositor instances' grants survive, replaces `app`'s grants (an
    // empty map removes the app; an empty app with an empty map removes every app) and writes it. Returns the merged
    // grants; nullopt when not Loaded, the file turned unreadable, a cap is exceeded, or the lock or write fails.
    std::optional<ShortcutGrants> update(std::string_view app, const std::map<std::string, ShortcutChord>& shortcuts);

    [[nodiscard]] Status status() const { return m_status; }
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

  private:
    std::filesystem::path m_path;
    Status m_status = Status::Unloaded;
  };

} // namespace andromeda
