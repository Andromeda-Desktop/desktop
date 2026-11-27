#pragma once

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "platform/display.h"
#include "platform/popup.h"
#include "platform/subsurface.h"
#include "platform/toplevel.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class CocoaDisplay;
class CocoaSurface;

[[nodiscard]] std::uint32_t cocoaKeyMods(NSEventModifierFlags flags) noexcept;
// The pasteboard type for a MIME type; nil when Cocoa has none.
[[nodiscard]] NSString* cocoaPasteboardType(const std::string& mime);
[[nodiscard]] std::optional<std::string> cocoaReadPasteboard(NSPasteboard* pasteboard, NSString* type);

// The flipped (y down, logical px) view that draws one Surface through its CAMetalLayer and turns AppKit events
// into Display callbacks.
@interface AndromedaSurfaceView : NSView <NSDraggingDestination>
@property(nonatomic) BOOL takesKeyboard;
- (instancetype)initWithOwner:(CocoaSurface*)owner frame:(NSRect)frame;
- (void)detach;
@end

class CocoaDisplay final : public Display {
public:
  CocoaDisplay();
  ~CocoaDisplay() override;

  bool connect(std::optional<std::chrono::steady_clock::time_point> deadline) override;
  void disconnect() override;
  [[nodiscard]] void* nativeDisplay() const noexcept override { return nullptr; }

  [[nodiscard]] int pollFd() const noexcept override { return -1; }
  [[nodiscard]] PollPrep preparePoll() override { return {}; }
  [[nodiscard]] PumpResult finishPoll(short /*revents*/) override { return PumpResult::Ok; }
  void cancelPoll() override {}
  int wait(pollfd* fds, nfds_t count, int timeoutMs) override;
  [[nodiscard]] int repeatPollTimeoutMs() const override { return -1; }
  void repeatTick() override {}

  [[nodiscard]] std::unique_ptr<Toplevel> createToplevel() override;
  [[nodiscard]] std::unique_ptr<Popup> createPopup() override;
  [[nodiscard]] std::unique_ptr<Subsurface> createSubsurface() override;

  [[nodiscard]] std::uint32_t lastInputSerial() const noexcept override { return m_serial; }
  void setCursorShape(std::uint32_t serial, std::uint32_t shape) override;
  [[nodiscard]] std::uint32_t keyboardModifiers() const noexcept override;
  [[nodiscard]] bool hasPointerPosition() const noexcept override { return m_pointerSurface != nullptr; }
  [[nodiscard]] double lastPointerX() const noexcept override { return m_pointerX; }
  [[nodiscard]] double lastPointerY() const noexcept override { return m_pointerY; }
  [[nodiscard]] OutputId lastPointerOutputId() const noexcept override;
  [[nodiscard]] OutputId firstOutputId() const noexcept override;
  [[nodiscard]] std::optional<OutputInfo> outputInfo(OutputId output) const override;
  [[nodiscard]] Surface* pointerSurface() const noexcept override { return m_pointerSurface; }
  [[nodiscard]] Surface* keyboardSurface() const noexcept override { return m_keyboardSurface; }
  [[nodiscard]] InputSource lastInputSource() const noexcept override { return m_lastInputSource; }
  [[nodiscard]] bool isLive(const Surface* surface) const noexcept override;

  [[nodiscard]] TextInputService* textInputService() const noexcept override { return m_textInput; }
  void setTextInputService(TextInputService* service) override { m_textInput = service; }

  void setPointerEventCallback(std::function<void(const PointerEvent&)> callback) override {
    m_pointerCallback = std::move(callback);
  }
  void setPinchEventCallback(std::function<void(const PinchEvent&)> callback) override {
    m_pinchCallback = std::move(callback);
  }
  void setAxisStopCallback(std::function<void(Surface* surface, std::uint32_t axis)> callback) override {
    m_axisStopCallback = std::move(callback);
  }
  void setKeyboardEventCallback(std::function<void(const KeyboardEvent&)> callback) override {
    m_keyboardCallback = std::move(callback);
  }

  void setClipboardChangedCallback(std::function<void()> changed) override { m_clipboardChanged = std::move(changed); }
  void readClipboard(
      const std::vector<std::string>& accepted, std::function<void(const std::string& mime, std::string bytes)> done,
      std::size_t maxBytes
  ) override;
  bool setClipboard(
      std::uint32_t serial, std::vector<std::string> mimeTypes, std::function<std::string(const std::string& mime)> data
  ) override;
  // macOS has no primary selection: it lives in this process only.
  [[nodiscard]] std::optional<std::string> primaryText() override { return m_primary; }
  void setPrimaryText(std::uint32_t /*serial*/, std::string text) override { m_primary = std::move(text); }
  void whenPrimaryText(std::function<void(std::optional<std::string>)> done) override { done(m_primary); }
  [[nodiscard]] OutputId outputForSurface(const Surface& surface) const noexcept override;

  void ringBell(const Surface& surface) const override;
  [[nodiscard]] std::string requestActivationToken(const Surface& surface, const std::string& appId) const override;
  void activateSurface(const Surface& surface, const std::string& token) const override;
  void requestAttentionTokenAsync(const Surface& surface, std::function<void(std::string token)> done) const override;

  bool setDropTarget(const Surface& surface, std::vector<std::string> acceptedTypes, DropTarget target) override;
  void clearDropTarget(const Surface& surface) override;
  bool startDrag(const Surface& origin, DragOffer source) override;
  [[nodiscard]] std::unique_ptr<Surface> createDragIcon(std::uint32_t width, std::uint32_t height, std::int32_t bufferScale) override;

  [[nodiscard]] bool serverPresent() const override { return true; }
  [[nodiscard]] std::unique_ptr<TextInputService> createTextInputService() override;

  void registerSurface(CocoaSurface* surface);
  void unregisterSurface(CocoaSurface* surface);
  // Ends a blocking wait(): AppKit callbacks that queue frame work or deferred calls run inside it.
  void wake() const;
  void pointer(PointerEvent event);
  void pinch(const PinchEvent& event);
  void axisStop(Surface* surface, std::uint32_t axis);
  void keyboard(const KeyboardEvent& event, Surface* surface);
  void keyboardFocus(Surface* surface, bool focused);
  void checkClipboard();
  // The left press a later startDrag begins from: AppKit starts a drag session from its mouse-down event.
  void setDragPress(NSEvent* event) { m_dragPress = event; }
  void setReopenCallback(std::function<void()> reopen) override { m_reopen = std::move(reopen); }
  void setQuitCallback(std::function<void(Quit)> quit) override { m_quit = std::move(quit); }
  void reopen();
  // The quit callback, else every Toplevel gets a close request; each may refuse it. True: the process may exit now.
  [[nodiscard]] bool requestQuit(Quit how);
  [[nodiscard]] static OutputId outputOf(NSScreen* screen);
  [[nodiscard]] static NSScreen* screenOf(OutputId output);

private:
  std::function<void()> m_reopen;
  std::function<void(Quit)> m_quit;
  std::unordered_set<CocoaSurface*> m_surfaces;
  std::function<void(const PointerEvent&)> m_pointerCallback;
  std::function<void(const PinchEvent&)> m_pinchCallback;
  std::function<void(Surface*, std::uint32_t)> m_axisStopCallback;
  std::function<void(const KeyboardEvent&)> m_keyboardCallback;
  std::function<void()> m_clipboardChanged;
  std::optional<std::string> m_primary;
  TextInputService* m_textInput = nullptr;
  Surface* m_pointerSurface = nullptr;
  Surface* m_keyboardSurface = nullptr;
  InputSource m_lastInputSource = InputSource::None;
  double m_pointerX = 0.0;
  double m_pointerY = 0.0;
  std::uint32_t m_serial = 0;
  NSInteger m_clipboardChangeCount = -1;
  NSEvent* m_dragPress = nil;
  int m_kqueue = -1;
  CFFileDescriptorRef m_kqueueSource = nullptr;
  CFRunLoopSourceRef m_kqueueRunLoopSource = nullptr;
  id m_appDelegate = nil;
  id m_activeObserver = nil;
  mutable bool m_wakePosted = false;
  bool m_connected = false;
};

// The native side every Cocoa surface shares: its view, layer, display link, input region and drop target.
class CocoaSurface : public virtual Surface {
public:
  explicit CocoaSurface(CocoaDisplay& display);
  ~CocoaSurface() override;

  void setInputRegion(const std::vector<InputRect>& rects) override { m_inputRegion = rects; }
  void setOpaqueRegion(const std::vector<InputRect>& /*rects*/) override {}
  void setBlurRegion(const std::vector<InputRect>& rects) override;
  void setOpaque(bool opaque) override;
  void clearBlurRegion() override;

  [[nodiscard]] CocoaDisplay& display() const noexcept { return m_display; }
  [[nodiscard]] AndromedaSurfaceView* view() const noexcept { return m_view; }
  [[nodiscard]] virtual NSWindow* window() const noexcept { return m_view.window; }
  [[nodiscard]] bool acceptsPoint(NSPoint local) const;
  // The window moved to a screen with another backing scale.
  void backingChanged();
  void displayLinkFired(CFTimeInterval timestamp);
  [[nodiscard]] Surface& surface() noexcept { return *this; }

  struct Drop {
    std::vector<std::string> accepted;
    DropTarget target;
  };
  [[nodiscard]] const std::optional<Drop>& drop() const noexcept { return m_drop; }
  void setDrop(std::optional<Drop> drop);

protected:
  // Adds the view to `superview` at `frame` (logical px), with its layer, display link and render target.
  void createView(NSView* superview, NSRect frame);
  void destroyView();
  // Keeps the blur view under the surface view, at its frame; call after the view moves or restacks.
  void syncBlur();
  // Seeds the buffer scale from the screen before the first configure.
  void seedScale(NSScreen* screen);

  [[nodiscard]] bool hasNativeSurface() const noexcept override { return m_view != nil; }
  [[nodiscard]] std::unique_ptr<NativeEglWindow> createNativeWindow() override;
  [[nodiscard]] bool frameCallbackPending() const noexcept override { return m_frameRequested; }
  void requestFrameCallback() override;
  void cancelFrameCallback() override;
  void commitNative() override {}
  [[nodiscard]] bool scalesByViewport() const noexcept override { return false; }
  void applyNativeScale() override;

  CocoaDisplay& m_display;
  AndromedaSurfaceView* m_view = nil;
  CAMetalLayer* m_layer = nil;
  CADisplayLink* m_link = nil;
  // The window-server blur behind the blur region, masked to it; nil while the region is empty.
  NSVisualEffectView* m_blur = nil;

private:
  std::optional<std::vector<InputRect>> m_inputRegion;
  std::optional<Drop> m_drop;
  bool m_frameRequested = false;
};

class CocoaToplevel final : public CocoaSurface, public Toplevel {
public:
  explicit CocoaToplevel(CocoaDisplay& display) : CocoaSurface(display) {}
  ~CocoaToplevel() override;

  bool initialize() override { return false; }
  bool initialize(OutputId output, const ToplevelSurfaceConfig& config) override;
  void setClosedCallback(std::function<void()> callback) override { m_closedCallback = std::move(callback); }
  void setStateCallback(std::function<void(const ToplevelState&)> callback) override {
    m_stateCallback = std::move(callback);
  }
  void setMinSize(std::uint32_t minWidth, std::uint32_t minHeight) override;
  void setTitle(const std::string& title) override;
  void beginMove(std::uint32_t serial) override;
  // AppKit resizes from the window edges itself.
  void beginResize(std::uint32_t /*serial*/, std::uint32_t /*edges*/) override {}
  void showWindowMenu(std::uint32_t /*serial*/, std::int32_t /*x*/, std::int32_t /*y*/) override {}
  void setMaximized(bool maximized) override;
  void setFullscreen(bool fullscreen) override;
  void setMinimized() override;
  bool requestWindowSize(std::uint32_t width, std::uint32_t height) override;
  void setParentWindow(std::string_view handle) override;
  void exportHandle(std::function<void(const std::string& handle)> ready) override;
  bool setCaptureSecure(bool secure) override;
  void setShortcutsInhibited(bool /*inhibited*/) override {}
  void setMenuBar(const std::vector<MenuBarItem>& /*items*/, std::function<void(std::uint32_t)> /*activated*/) override {}
  [[nodiscard]] const ToplevelState& state() const noexcept override { return m_state; }
  [[nodiscard]] std::uint32_t margin() const noexcept override { return 0; }
  [[nodiscard]] std::uint32_t windowWidth() const noexcept override { return width(); }
  [[nodiscard]] std::uint32_t windowHeight() const noexcept override { return height(); }

  [[nodiscard]] NSWindow* window() const noexcept override { return m_window; }
  void closeRequested();
  void contentResized();
  void updateState();

private:
  NSWindow* m_window = nil;
  id m_delegate = nil;
  ToplevelState m_state;
  std::function<void()> m_closedCallback;
  std::function<void(const ToplevelState&)> m_stateCallback;
};

class CocoaPopup final : public CocoaSurface, public Popup {
public:
  explicit CocoaPopup(CocoaDisplay& display) : CocoaSurface(display) {}
  ~CocoaPopup() override;

  bool initialize() override { return false; }
  bool initialize(const PopupParent& parent, PopupSurfaceConfig config) override;
  bool resize(std::uint32_t width, std::uint32_t height, bool commit = true) override;
  bool repositionAnchor(const PopupSurfaceConfig& anchorConfig, bool commit = true) override;
  void setDismissedCallback(std::function<void()> callback) override { m_dismissedCallback = std::move(callback); }
  [[nodiscard]] std::int32_t configuredX() const noexcept override { return m_configuredX; }
  [[nodiscard]] std::int32_t configuredY() const noexcept override { return m_configuredY; }

  [[nodiscard]] NSWindow* window() const noexcept override { return m_window; }
  // A click outside this popup and its child popups, or the app lost focus.
  void dismiss();

private:
  void place();

  CocoaSurface* m_parent = nullptr;
  NSWindow* m_window = nil;
  id m_clickMonitor = nil;
  id m_resignObserver = nil;
  PopupSurfaceConfig m_config;
  std::int32_t m_configuredX = 0;
  std::int32_t m_configuredY = 0;
  std::function<void()> m_dismissedCallback;
};

class CocoaSubsurface final : public CocoaSurface, public Subsurface {
public:
  explicit CocoaSubsurface(CocoaDisplay& display) : CocoaSurface(display) {}
  ~CocoaSubsurface() override = default;

  bool initialize() override { return false; }
  bool initialize(const Surface& parent, SubsurfaceConfig config) override;
  void setPosition(std::int32_t x, std::int32_t y) override;
  void resize(std::uint32_t width, std::uint32_t height) override;
  void setStacking(SubsurfaceStacking stacking) override;

private:
  [[nodiscard]] NSRect frame() const;
  void applyStacking();

  CocoaSurface* m_parent = nullptr;
  SubsurfaceConfig m_config;
};
