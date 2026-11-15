#pragma once

#include "settings.h"
#include "ui/signal.h"

#include <filesystem>
#include <string>
#include <variant>

typedef struct _GFileMonitor GFileMonitor;

namespace term {

  // The preferences file and its live state. A hand edit applies as soon as the file is written; a bad file keeps
  // the last good settings in use and reports the error.
  class SettingsStore {
  public:
    struct Missing {};
    struct Loaded {};
    struct Invalid {
      SettingsError error;
    };
    using FileState = std::variant<Missing, Loaded, Invalid>;

    SettingsStore();
    ~SettingsStore();
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    [[nodiscard]] const Settings& settings() const noexcept { return m_settings; }
    [[nodiscard]] const FileState& fileState() const noexcept { return m_state; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    // Writes the file at once; listeners run before it returns.
    void update(const Settings& settings);
    // Settings or the file state changed.
    [[nodiscard]] Signal<>& changed() noexcept { return m_changed; }

  private:
    void load();
    void set(Settings settings, FileState state);

    std::filesystem::path m_path;
    Settings m_settings;
    FileState m_state;
    std::string m_bytes; // the file as last read or written: an event with the same bytes changes nothing
    GFileMonitor* m_monitor = nullptr;
    Signal<> m_changed;
  };

} // namespace term
