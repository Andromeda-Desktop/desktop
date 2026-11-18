#pragma once

#include "platform/output_id.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Popup;
class Surface;
class Toplevel;

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
  [[nodiscard]] virtual OutputId outputForSurface(const Surface& surface) const noexcept = 0;

  virtual void ringBell(const Surface& surface) const = 0;
  // appId marks the token as an app launch. "" when the window system has no activation tokens.
  [[nodiscard]] virtual std::string requestActivationToken(const Surface& surface, const std::string& appId) const = 0;
  // Raises and focuses surface with a token from another client; no-op with an empty token.
  virtual void activateSurface(const Surface& surface, const std::string& token) const = 0;
  // A token that marks the window as needing attention instead of focusing it. done runs from the event loop.
  virtual void requestAttentionTokenAsync(const Surface& surface, std::function<void(std::string token)> done) const = 0;
};
