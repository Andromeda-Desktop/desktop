#pragma once

#include "input/global_shortcuts.h"

#include <filesystem>
#include <optional>

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
    // False when not Loaded, over the caps, or the write fails.
    bool save(const ShortcutGrants& grants);

    [[nodiscard]] Status status() const { return m_status; }
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

  private:
    std::filesystem::path m_path;
    Status m_status = Status::Unloaded;
  };

} // namespace andromeda
