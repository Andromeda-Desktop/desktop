#pragma once

#include "platform/output_id.h"
#include "platform/surface.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct ToplevelSurfaceConfig {
  std::uint32_t width = 720;
  std::uint32_t height = 480;
  std::uint32_t minWidth = 0;
  std::uint32_t minHeight = 0;
  std::uint32_t maxWidth = 0; // 0: no maximum
  std::uint32_t maxHeight = 0;
  std::string title = "Noctalia";
  const char* appId = "dev.noctalia.Noctalia";
  bool maximized = false; // asked before the first commit, so the first configure is already maximized
  // Transparent band around the window, outside its window geometry, for client-side resize (the GTK way).
  // Only while the window is free: maximized and fullscreen windows have none.
  std::uint32_t resizeMargin = 0;
};

// Window states from the last configure.
struct ToplevelState {
  bool maximized = false;
  bool fullscreen = false;
  bool activated = false;
  bool tiled = false; // any tiled edge
  bool suspended = false; // xdg suspended: minimized, on another workspace, locked or every output off
  bool resizing = false;  // an interactive resize runs
  bool operator==(const ToplevelState&) const = default;
};

// One row of a window's menubar; parent 0 puts it in the bar.
struct MenuBarItem {
  std::uint32_t id = 0;
  std::uint32_t parent = 0;
  std::string label;
  std::string shortcut; // display text, "Ctrl+Q"
  bool enabled = true;
  bool operator==(const MenuBarItem&) const = default;
};

// Window edges for an interactive resize: a bit set (same values as xdg_toplevel resize_edge).
namespace ResizeEdge {
  inline constexpr std::uint32_t None = 0;
  inline constexpr std::uint32_t Top = 1;
  inline constexpr std::uint32_t Bottom = 2;
  inline constexpr std::uint32_t Left = 4;
  inline constexpr std::uint32_t Right = 8;
} // namespace ResizeEdge

// An application window. The platform subclass (Wayland: ToplevelSurface) owns the native window.
class Toplevel : public virtual Surface {
public:
  using Surface::initialize;
  // OutputId::None lets the window system pick the output.
  virtual bool initialize(OutputId output, const ToplevelSurfaceConfig& config) = 0;

  virtual void setClosedCallback(std::function<void()> callback) = 0;
  // Called after a configure whose states differ from the last one.
  virtual void setStateCallback(std::function<void(const ToplevelState&)> callback) = 0;
  virtual void setMinSize(std::uint32_t minWidth, std::uint32_t minHeight) = 0;
  virtual void setTitle(const std::string& title) = 0;
  // serial: the input event that starts the interaction.
  virtual void beginMove(std::uint32_t serial) = 0;
  // edges: ResizeEdge bits.
  virtual void beginResize(std::uint32_t serial, std::uint32_t edges) = 0;
  virtual void showWindowMenu(std::uint32_t serial, std::int32_t x, std::int32_t y) = 0;
  virtual void setMaximized(bool maximized) = 0;
  // On the output the window system picks.
  virtual void setFullscreen(bool fullscreen) = 0;
  virtual void setMinimized() = 0;
  // A floating window picks its own size. False when the window system sizes the window (maximized, fullscreen,
  // tiled with a size) or before the first configure.
  virtual bool requestWindowSize(std::uint32_t width, std::uint32_t height) = 0;

  // handle: another client's window, in the portal form ("wayland:<handle>"); the window system floats this window
  // over it. An empty or foreign-form handle leaves the window unparented.
  virtual void setParentWindow(std::string_view handle) = 0;
  // ready gets this window's handle in the portal form, at once when known, else from the event loop; "" when the
  // window system cannot export windows. Only the last pending ready runs.
  virtual void exportHandle(std::function<void(const std::string& handle)> ready) = 0;
  // Secure: screen capture shows the window blank. False when the window system cannot do it.
  virtual bool setCaptureSecure(bool secure) = 0;
  // Inhibited: the window system passes its own shortcuts to this window while it has keyboard focus.
  virtual void setShortcutsInhibited(bool inhibited) = 0;
  // Shows the menubar in the desktop's global menu bar, when it has one; activated gets the picked item's id. Empty
  // items remove it.
  virtual void setMenuBar(const std::vector<MenuBarItem>& items, std::function<void(std::uint32_t id)> activated) = 0;

  [[nodiscard]] virtual const ToplevelState& state() const noexcept = 0;
  // The window inside the surface: the surface is the window plus margin() on every side.
  [[nodiscard]] virtual std::uint32_t margin() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t windowWidth() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t windowHeight() const noexcept = 0;
};
