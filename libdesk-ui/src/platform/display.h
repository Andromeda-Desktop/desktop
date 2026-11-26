#pragma once

#include "platform/drop_target.h"
#include "platform/input_events.h"
#include "platform/output_id.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <vector>

class Popup;
class Subsurface;
class TextInputService;
class Surface;
class Toplevel;

struct OutputInfo {
  std::int32_t logicalWidth = 0;
  std::int32_t logicalHeight = 0;
  float scale = 1.0F; // the scale the window system renders this output at
};

// Lost: the window system closed the connection; Error: the connection failed (logged) and is unusable.
enum class PumpResult : std::uint8_t { Ok, Lost, Error };

struct PollPrep {
  PumpResult result = PumpResult::Ok;
  short events = 0; // what to poll pollFd() for
};

// The connection to the window system: creates windows and popups and answers seat and output queries.
// Wayland: WaylandConnection.
class Display {
public:
  virtual ~Display() = default;

  // With a deadline, a window system that accepts but never answers makes it throw at the deadline.
  virtual bool connect(std::optional<std::chrono::steady_clock::time_point> deadline) = 0;
  // Safe on a dead connection; connect() starts over.
  virtual void disconnect() = 0;
  // The EGL native display (GlSharedContext::initialize converts it).
  [[nodiscard]] virtual void* nativeDisplay() const noexcept = 0;

  // One pass of a poll loop: preparePoll(); if Ok, wait() on pollFd() for its events with the app's fds; then
  // finishPoll(revents), or cancelPoll() when wait() fails. Every Ok preparePoll() needs exactly one of the two.
  [[nodiscard]] virtual int pollFd() const noexcept = 0;
  [[nodiscard]] virtual PollPrep preparePoll() = 0;
  [[nodiscard]] virtual PumpResult finishPoll(short revents) = 0;
  virtual void cancelPoll() = 0;
  // poll(2) semantics. A window system whose events do not arrive on a fd (Cocoa) dispatches them while it waits.
  virtual int wait(pollfd* fds, nfds_t count, int timeoutMs) { return ::poll(fds, count, timeoutMs); }
  // -1: no key repeat pending.
  [[nodiscard]] virtual int repeatPollTimeoutMs() const = 0;
  virtual void repeatTick() = 0;

  [[nodiscard]] virtual std::unique_ptr<Toplevel> createToplevel() = 0;
  [[nodiscard]] virtual std::unique_ptr<Popup> createPopup() = 0;
  [[nodiscard]] virtual std::unique_ptr<Subsurface> createSubsurface() = 0;

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
  // OutputId::None when there is no output.
  [[nodiscard]] virtual OutputId firstOutputId() const noexcept = 0;
  [[nodiscard]] virtual std::optional<OutputInfo> outputInfo(OutputId output) const = 0;
  // The surfaces under the pointer and with keyboard focus; null when none or not a Surface.
  [[nodiscard]] virtual Surface* pointerSurface() const noexcept = 0;
  [[nodiscard]] virtual Surface* keyboardSurface() const noexcept = 0;
  [[nodiscard]] virtual InputSource lastInputSource() const noexcept = 0;
  // False once surface is destroyed: a popup that outlives its parent checks before it touches it.
  [[nodiscard]] virtual bool isLive(const Surface* surface) const noexcept = 0;
  // Input method support; null when the window system has none.
  [[nodiscard]] virtual TextInputService* textInputService() const noexcept = 0;
  virtual void setTextInputService(TextInputService* service) = 0;

  // One receiver each: a second call replaces the first.
  virtual void setPointerEventCallback(std::function<void(const PointerEvent&)> callback) = 0;
  virtual void setPinchEventCallback(std::function<void(const PinchEvent&)> callback) = 0;
  // The end of a scroll gesture on axis (0 vertical, 1 horizontal); surface is null when no Surface owns it.
  virtual void setAxisStopCallback(std::function<void(Surface* surface, std::uint32_t axis)> callback) = 0;
  virtual void setKeyboardEventCallback(std::function<void(const KeyboardEvent&)> callback) = 0;

  // Runs after each clipboard change, ours included. Also starts listening to the primary selection.
  virtual void setClipboardChangedCallback(std::function<void()> changed) = 0;
  // Reads the clipboard as the first type of accepted it offers, off the main thread; done runs on the event loop with
  // the type and bytes, or "" and "" when nothing matched, the source did not answer, or there is no clipboard.
  virtual void readClipboard(
      const std::vector<std::string>& accepted, std::function<void(const std::string& mime, std::string bytes)> done,
      std::size_t maxBytes
  ) = 0;
  // serial: the input event that asked for the copy. False when the window system has no clipboard.
  virtual bool setClipboard(
      std::uint32_t serial, std::vector<std::string> mimeTypes, std::function<std::string(const std::string& mime)> data
  ) = 0;
  // The primary selection (middle-click paste); nullopt and no-ops when the window system has none.
  [[nodiscard]] virtual std::optional<std::string> primaryText() = 0;
  virtual void setPrimaryText(std::uint32_t serial, std::string text) = 0;
  // done gets the text once known: at once, or when the read of a selection that just arrived ends.
  virtual void whenPrimaryText(std::function<void(std::optional<std::string>)> done) = 0;
  [[nodiscard]] virtual OutputId outputForSurface(const Surface& surface) const noexcept = 0;

  virtual void ringBell(const Surface& surface) const = 0;
  // appId marks the token as an app launch. "" when the window system has no activation tokens.
  [[nodiscard]] virtual std::string requestActivationToken(const Surface& surface, const std::string& appId) const = 0;
  // Raises and focuses surface with a token from another client; no-op with an empty token.
  virtual void activateSurface(const Surface& surface, const std::string& token) const = 0;
  // A token that marks the window as needing attention instead of focusing it. done runs from the event loop.
  virtual void requestAttentionTokenAsync(const Surface& surface, std::function<void(std::string token)> done) const = 0;

  // acceptedTypes: best first, shared by every drop target of this display. False when the window system has no
  // drag and drop. A second call for the same surface replaces its target.
  virtual bool setDropTarget(const Surface& surface, std::vector<std::string> acceptedTypes, DropTarget target) = 0;
  virtual void clearDropTarget(const Surface& surface) = 0;
  // Starts a drag from the last pointer press on origin. False when the window system has no drag and drop.
  virtual bool startDrag(const Surface& origin, DragOffer source) = 0;
  // An unmapped surface for DragOffer::icon, in logical px; nullptr when the window system has none.
  [[nodiscard]] virtual std::unique_ptr<Surface> createDragIcon(std::uint32_t width, std::uint32_t height, std::int32_t bufferScale) = 0;

  // False once the window system is gone for good (its socket removed at logout): a reconnect stops waiting.
  [[nodiscard]] virtual bool serverPresent() const = 0;
  // The platform's input method service; the caller owns it and hands it to setTextInputService().
  [[nodiscard]] virtual std::unique_ptr<TextInputService> createTextInputService() = 0;
};

// The display of the platform this build targets, not yet connected.
[[nodiscard]] std::unique_ptr<Display> createDisplay();
