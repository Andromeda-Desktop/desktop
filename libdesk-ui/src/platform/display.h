#pragma once

#include "platform/input_events.h"
#include "platform/output_id.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class Popup;
class TextInputService;
class Surface;
class Toplevel;

struct OutputInfo {
  std::int32_t logicalWidth = 0;
  std::int32_t logicalHeight = 0;
  float scale = 1.0F; // the scale the window system renders this output at
};

// The connection to the window system: creates windows and popups and answers seat and output queries.
// Wayland: WaylandConnection.
class Display {
public:
  virtual ~Display() = default;

  [[nodiscard]] virtual std::unique_ptr<Toplevel> createToplevel() = 0;
  [[nodiscard]] virtual std::unique_ptr<Popup> createPopup() = 0;

  // The serial of the last input event, for requests that must name the event that caused them.
  [[nodiscard]] virtual std::uint32_t lastInputSerial() const noexcept = 0;
  // shape: a CursorShape value.
  virtual void setCursorShape(std::uint32_t serial, std::uint32_t shape) = 0;
  [[nodiscard]] virtual std::uint32_t keyboardModifiers() const noexcept = 0;
  [[nodiscard]] virtual bool hasPointerPosition() const noexcept = 0;
  // Surface-local logical pixels on the last surface the pointer entered.
  [[nodiscard]] virtual double lastPointerX() const noexcept = 0;
  [[nodiscard]] virtual double lastPointerY() const noexcept = 0;
  [[nodiscard]] virtual OutputId lastPointerOutputId() const noexcept = 0;
  [[nodiscard]] virtual std::optional<OutputInfo> outputInfo(OutputId output) const = 0;
  // The surfaces under the pointer and with keyboard focus; null when none or not a Surface.
  [[nodiscard]] virtual Surface* pointerSurface() const noexcept = 0;
  [[nodiscard]] virtual Surface* keyboardSurface() const noexcept = 0;
  [[nodiscard]] virtual InputSource lastInputSource() const noexcept = 0;
  // False once surface is destroyed: a popup that outlives its parent checks before it touches it.
  [[nodiscard]] virtual bool isLive(const Surface* surface) const noexcept = 0;
  // Input method support; null when the window system has none.
  [[nodiscard]] virtual TextInputService* textInputService() const noexcept = 0;
  [[nodiscard]] virtual OutputId outputForSurface(const Surface& surface) const noexcept = 0;

  virtual void ringBell(const Surface& surface) const = 0;
  // appId marks the token as an app launch. "" when the window system has no activation tokens.
  [[nodiscard]] virtual std::string requestActivationToken(const Surface& surface, const std::string& appId) const = 0;
  // Raises and focuses surface with a token from another client; no-op with an empty token.
  virtual void activateSurface(const Surface& surface, const std::string& token) const = 0;
  // A token that marks the window as needing attention instead of focusing it. done runs from the event loop.
  virtual void requestAttentionTokenAsync(const Surface& surface, std::function<void(std::string token)> done) const = 0;
};
