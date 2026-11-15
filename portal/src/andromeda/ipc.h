#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace xdpu {

  class Loop;

  struct ScreenCastCommand {
    enum class Kind {
      Clear,
      Output,
      Window,
      FollowWindow,
      FollowOutput,
      FollowStop,
    };

    Kind kind = Kind::Clear;
    std::string value;
    uint64_t serial = 0;
  };

  class AndromedaIpc {
  public:
    using ScreenCastHandler = std::function<void(const ScreenCastCommand&)>;
    using FocusHandler = std::function<void(const std::optional<std::string>&)>;
    // Identifiers of the windows whose capture shows nothing (dsk_surface_v1.set_secure).
    using CaptureBlockedHandler = std::function<void(std::set<std::string>)>;

    AndromedaIpc(
        Loop& loop, ScreenCastHandler screenCastHandler, FocusHandler focusedWindowHandler,
        FocusHandler focusedOutputHandler, CaptureBlockedHandler captureBlockedHandler
    );
    ~AndromedaIpc();

    AndromedaIpc(const AndromedaIpc&) = delete;
    AndromedaIpc& operator=(const AndromedaIpc&) = delete;

    [[nodiscard]] bool connected() const;
    void setScreenCastActive(bool active);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
