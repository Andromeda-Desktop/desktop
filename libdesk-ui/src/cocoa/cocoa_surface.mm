#include "cocoa/cocoa_internal.h"

#include "cocoa/cocoa_keys.h"
#include "core/deferred_call.h"
#include "core/input/input_codes.h"
#include "core/input/pointer_axis.h"
#include "platform/popup_place.h"
#include "render/backend/native_egl_window.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace {
  // Logical px per wheel notch, as libinput reports on Wayland.
  constexpr double kWheelStep = 15.0;

  class CocoaEglWindow final : public NativeEglWindow {
  public:
    explicit CocoaEglWindow(CAMetalLayer* layer) : m_layer(layer) {}
    // ANGLE's Metal backend takes the CAMetalLayer as the EGLNativeWindowType.
    void* resize(std::uint32_t bufferWidth, std::uint32_t bufferHeight) override {
      m_layer.drawableSize = CGSizeMake(bufferWidth, bufferHeight);
      return (__bridge void*)m_layer;
    }

  private:
    CAMetalLayer* m_layer;
  };

  std::uint32_t eventTime(NSEvent* event) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(event.timestamp * 1000.0));
  }

  std::uint32_t firstCodepoint(NSString* text) {
    if (text.length == 0) {
      return 0;
    }
    const unichar first = [text characterAtIndex:0];
    if (CFStringIsSurrogateHighCharacter(first) && text.length > 1) {
      return static_cast<std::uint32_t>(CFStringGetLongCharacterForSurrogatePair(first, [text characterAtIndex:1]));
    }
    const std::uint32_t cp = first;
    // AppKit puts arrows and F keys in the private use range F700-F8FF.
    return cp >= 0xF700 && cp <= 0xF8FF ? 0 : cp;
  }

  KeyboardEvent keyboardEvent(NSEvent* event, bool pressed) {
    const NSEventModifierFlags flags = event.modifierFlags;
    KeyboardEvent key;
    key.pressed = pressed;
    key.key = cocoa_keys::evdevCode(event.keyCode);
    key.modifiers = cocoaKeyMods(flags);
    key.repeat = pressed && event.isARepeat;
    key.utf32 = firstCodepoint(event.characters);
    key.unshifted = firstCodepoint([event charactersByApplyingModifiers:0]);
    const std::uint32_t function = cocoa_keys::functionKeysym(event.keyCode, (flags & NSEventModifierFlagShift) != 0);
    key.sym = function != 0 ? function : cocoa_keys::keysymForCodepoint(firstCodepoint(event.charactersIgnoringModifiers));
    return key;
  }

  // Whether a flagsChanged event pressed or released its modifier key.
  bool modifierPressed(NSEvent* event) {
    switch (event.keyCode) {
    case 0x38:
    case 0x3C:
      return (event.modifierFlags & NSEventModifierFlagShift) != 0;
    case 0x3B:
    case 0x3E:
      return (event.modifierFlags & NSEventModifierFlagControl) != 0;
    case 0x3A:
    case 0x3D:
      return (event.modifierFlags & NSEventModifierFlagOption) != 0;
    case 0x36:
    case 0x37:
      return (event.modifierFlags & NSEventModifierFlagCommand) != 0;
    case 0x39:
      return (event.modifierFlags & NSEventModifierFlagCapsLock) != 0;
    case 0x3F:
      return (event.modifierFlags & NSEventModifierFlagFunction) != 0;
    default:
      return false;
    }
  }

  std::uint32_t evdevButton(NSInteger number) {
    switch (number) {
    case 0:
      return BTN_LEFT;
    case 1:
      return BTN_RIGHT;
    case 2:
      return BTN_MIDDLE;
    case 3:
      return BTN_SIDE;
    case 4:
      return BTN_EXTRA;
    default:
      return BTN_MIDDLE + static_cast<std::uint32_t>(number);
    }
  }

  NSDragOperation dragOperation(std::uint32_t action) {
    if ((action & DropCopy) != 0) {
      return NSDragOperationCopy;
    }
    if ((action & DropMove) != 0) {
      return NSDragOperationMove;
    }
    return (action & DropAsk) != 0 ? NSDragOperationGeneric : NSDragOperationNone;
  }

  std::uint32_t dropAction(NSDragOperation operation) {
    if ((operation & NSDragOperationCopy) != 0) {
      return DropCopy;
    }
    if ((operation & NSDragOperationMove) != 0) {
      return DropMove;
    }
    return (operation & NSDragOperationGeneric) != 0 ? DropAsk : DropNone;
  }

  // The accepted MIME types the pasteboard can give, in the target's order of preference.
  std::vector<std::string> offeredMimes(const std::vector<std::string>& accepted, NSPasteboard* pasteboard) {
    std::vector<std::string> mimes;
    for (const std::string& mime : accepted) {
      NSString* type = cocoaPasteboardType(mime);
      if (type != nil && [pasteboard availableTypeFromArray:@[ type ]] != nil) {
        mimes.push_back(mime);
      }
    }
    return mimes;
  }
} // namespace

// The flipped content view of every window: surface views and their subsurfaces are its children.
@interface AndromedaContainerView : NSView
@end

@implementation AndromedaContainerView
- (BOOL)isFlipped {
  return YES;
}
@end

@interface AndromedaLinkTarget : NSObject
@property(nonatomic, assign) CocoaSurface* owner;
- (void)tick:(CADisplayLink*)link;
@end

@implementation AndromedaLinkTarget
- (void)tick:(CADisplayLink*)link {
  if (_owner != nullptr) {
    _owner->displayLinkFired(link.timestamp);
  }
}
@end

@interface AndromedaWindow : NSWindow
@property(nonatomic) BOOL keyable;
@end

@implementation AndromedaWindow
- (BOOL)canBecomeKeyWindow {
  return _keyable;
}
- (BOOL)canBecomeMainWindow {
  return _keyable && self.parentWindow == nil;
}
@end

@interface AndromedaWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) CocoaToplevel* owner;
@end

@implementation AndromedaWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
  if (_owner != nullptr) {
    _owner->closeRequested();
  }
  return NO;
}
- (void)windowDidResize:(NSNotification*)notification {
  if (_owner != nullptr) {
    _owner->contentResized();
  }
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
  if (_owner != nullptr) {
    _owner->display().keyboardFocus(&_owner->surface(), true);
    _owner->updateState();
  }
}
- (void)windowDidResignKey:(NSNotification*)notification {
  if (_owner != nullptr) {
    _owner->display().keyboardFocus(&_owner->surface(), false);
    _owner->updateState();
  }
}
- (void)windowDidMiniaturize:(NSNotification*)notification {
  if (_owner != nullptr) {
    _owner->updateState();
  }
}
- (void)windowDidDeminiaturize:(NSNotification*)notification {
  if (_owner != nullptr) {
    _owner->updateState();
  }
}
- (void)windowDidEnterFullScreen:(NSNotification*)notification {
  if (_owner != nullptr) {
    _owner->updateState();
  }
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
  if (_owner != nullptr) {
    _owner->updateState();
  }
}
- (void)windowDidChangeOcclusionState:(NSNotification*)notification {
  if (_owner != nullptr) {
    _owner->updateState();
  }
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
  if (_owner != nullptr) {
    _owner->backingChanged();
  }
}
- (void)windowDidChangeScreen:(NSNotification*)notification {
  if (_owner != nullptr) {
    _owner->backingChanged();
  }
}
@end

@implementation AndromedaSurfaceView {
  CocoaSurface* _owner;
  NSTrackingArea* _tracking;
  BOOL _dropAccepted;
  double _pinchScale;
}

- (instancetype)initWithOwner:(CocoaSurface*)owner frame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    _owner = owner;
  }
  return self;
}

- (void)detach {
  _owner = nullptr;
}

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return _takesKeyboard;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  return YES;
}

// A transparent view under a full-size-content title bar would otherwise drag the window on every click.
- (BOOL)mouseDownCanMoveWindow {
  return NO;
}

- (NSView*)hitTest:(NSPoint)point {
  const NSPoint local = [self convertPoint:point fromView:self.superview];
  if (_owner == nullptr || !NSPointInRect(local, self.bounds) || !_owner->acceptsPoint(local)) {
    return nil;
  }
  return self;
}

- (void)updateTrackingAreas {
  if (_tracking != nil) {
    [self removeTrackingArea:_tracking];
  }
  _tracking = [[NSTrackingArea alloc]
      initWithRect:NSZeroRect
           options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect
             owner:self
          userInfo:nil];
  [self addTrackingArea:_tracking];
  [super updateTrackingAreas];
}

- (PointerEvent)pointerEvent:(PointerEvent::Type)type event:(NSEvent*)event {
  const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
  PointerEvent pointer{.type = type};
  pointer.surface = &_owner->surface();
  pointer.nativeSurface = (__bridge void*)self;
  pointer.sx = local.x;
  pointer.sy = local.y;
  pointer.time = eventTime(event);
  return pointer;
}

- (void)sendPointer:(PointerEvent::Type)type event:(NSEvent*)event {
  if (_owner != nullptr) {
    _owner->display().pointer([self pointerEvent:type event:event]);
  }
}

- (void)sendButton:(NSEvent*)event pressed:(bool)pressed {
  if (_owner == nullptr) {
    return;
  }
  PointerEvent pointer = [self pointerEvent:PointerEvent::Type::Button event:event];
  pointer.button = evdevButton(event.buttonNumber);
  pointer.pressed = pressed;
  _owner->display().pointer(pointer);
}

- (void)mouseEntered:(NSEvent*)event {
  [self sendPointer:PointerEvent::Type::Enter event:event];
}
- (void)mouseExited:(NSEvent*)event {
  [self sendPointer:PointerEvent::Type::Leave event:event];
}
- (void)mouseMoved:(NSEvent*)event {
  [self sendPointer:PointerEvent::Type::Motion event:event];
}
- (void)mouseDragged:(NSEvent*)event {
  [self sendPointer:PointerEvent::Type::Motion event:event];
}
- (void)rightMouseDragged:(NSEvent*)event {
  [self sendPointer:PointerEvent::Type::Motion event:event];
}
- (void)otherMouseDragged:(NSEvent*)event {
  [self sendPointer:PointerEvent::Type::Motion event:event];
}
- (void)mouseDown:(NSEvent*)event {
  if (_owner != nullptr) {
    _owner->display().setDragPress(event);
  }
  [self sendButton:event pressed:true];
}
- (void)mouseUp:(NSEvent*)event {
  [self sendButton:event pressed:false];
}
- (void)rightMouseDown:(NSEvent*)event {
  [self sendButton:event pressed:true];
}
- (void)rightMouseUp:(NSEvent*)event {
  [self sendButton:event pressed:false];
}
- (void)otherMouseDown:(NSEvent*)event {
  [self sendButton:event pressed:true];
}
- (void)otherMouseUp:(NSEvent*)event {
  [self sendButton:event pressed:false];
}

- (void)scrollWheel:(NSEvent*)event {
  // A callback may close this surface: keep the view alive and check the owner before each use.
  AndromedaSurfaceView* const keep = self;
  const bool precise = event.hasPreciseScrollingDeltas;
  const auto axisEvent = [&](std::uint32_t axis, double delta) {
    if (keep->_owner == nullptr || delta == 0.0) {
      return;
    }
    PointerEvent pointer = [self pointerEvent:PointerEvent::Type::Axis event:event];
    pointer.axis = axis;
    pointer.axisInverted = event.isDirectionInvertedFromDevice;
    if (precise) {
      pointer.axisSource = static_cast<std::uint32_t>(AxisSource::Finger);
      pointer.axisValue = -delta;
    } else {
      pointer.axisSource = static_cast<std::uint32_t>(AxisSource::Wheel);
      pointer.axisValue = -delta * kWheelStep;
      pointer.axisValue120 = static_cast<std::int32_t>(std::lround(-delta * 120.0));
      pointer.axisDiscrete = delta > 0.0 ? -1 : 1;
      pointer.axisLines = static_cast<float>(-delta);
    }
    _owner->display().pointer(pointer);
  };
  axisEvent(static_cast<std::uint32_t>(PointerAxis::Vertical), event.scrollingDeltaY);
  axisEvent(static_cast<std::uint32_t>(PointerAxis::Horizontal), event.scrollingDeltaX);
  if (precise && _owner != nullptr && (event.phase == NSEventPhaseEnded || event.phase == NSEventPhaseCancelled)) {
    _owner->display().axisStop(&_owner->surface(), static_cast<std::uint32_t>(PointerAxis::Vertical));
  }
  if (precise && _owner != nullptr && (event.phase == NSEventPhaseEnded || event.phase == NSEventPhaseCancelled)) {
    _owner->display().axisStop(&_owner->surface(), static_cast<std::uint32_t>(PointerAxis::Horizontal));
  }
}

- (void)magnifyWithEvent:(NSEvent*)event {
  if (_owner == nullptr) {
    return;
  }
  const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
  PinchEvent pinch;
  pinch.surface = &_owner->surface();
  pinch.nativeSurface = (__bridge void*)self;
  pinch.sx = local.x;
  pinch.sy = local.y;
  if (event.phase == NSEventPhaseBegan) {
    _pinchScale = 1.0;
    pinch.phase = PinchEvent::Phase::Begin;
  } else if (event.phase == NSEventPhaseEnded || event.phase == NSEventPhaseCancelled) {
    pinch.phase = PinchEvent::Phase::End;
    pinch.cancelled = event.phase == NSEventPhaseCancelled;
  } else {
    _pinchScale *= 1.0 + event.magnification;
    pinch.phase = PinchEvent::Phase::Update;
  }
  pinch.scale = _pinchScale;
  _owner->display().pinch(pinch);
}

- (void)keyDown:(NSEvent*)event {
  if (_owner != nullptr) {
    _owner->display().keyboard(keyboardEvent(event, true), &_owner->surface());
  }
}

- (void)keyUp:(NSEvent*)event {
  if (_owner != nullptr) {
    _owner->display().keyboard(keyboardEvent(event, false), &_owner->surface());
  }
}

- (void)flagsChanged:(NSEvent*)event {
  if (_owner == nullptr) {
    return;
  }
  KeyboardEvent key;
  key.pressed = modifierPressed(event);
  key.key = cocoa_keys::evdevCode(event.keyCode);
  key.sym = cocoa_keys::functionKeysym(event.keyCode, false);
  key.modifiers = cocoaKeyMods(event.modifierFlags);
  _owner->display().keyboard(key, &_owner->surface());
}

- (NSDragOperation)dropMotion:(id<NSDraggingInfo>)info {
  if (_owner == nullptr || !_owner->drop() || !_dropAccepted || !_owner->drop()->target.motion) {
    return NSDragOperationNone;
  }
  const NSPoint local = [self convertPoint:info.draggingLocation fromView:nil];
  const std::uint32_t action =
      _owner->drop()->target.motion(static_cast<float>(local.x), static_cast<float>(local.y));
  return dragOperation(action) & info.draggingSourceOperationMask;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
  _dropAccepted = NO;
  if (_owner == nullptr || !_owner->drop()) {
    return NSDragOperationNone;
  }
  const auto& drop = *_owner->drop();
  const std::vector<std::string> mimes = offeredMimes(drop.accepted, info.draggingPasteboard);
  _dropAccepted = !mimes.empty() && (!drop.target.enter || drop.target.enter(mimes));
  return [self dropMotion:info];
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
  return [self dropMotion:info];
}

- (void)draggingExited:(id<NSDraggingInfo>)info {
  if (_owner != nullptr && _owner->drop() && _dropAccepted && _owner->drop()->target.leave) {
    _owner->drop()->target.leave();
  }
  _dropAccepted = NO;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
  if (_owner == nullptr || !_owner->drop() || !_dropAccepted || !_owner->drop()->target.drop) {
    return NO;
  }
  const auto& drop = *_owner->drop();
  NSPasteboard* pasteboard = info.draggingPasteboard;
  for (const std::string& mime : offeredMimes(drop.accepted, pasteboard)) {
    auto data = cocoaReadPasteboard(pasteboard, cocoaPasteboardType(mime));
    if (!data) {
      continue;
    }
    const NSPoint local = [self convertPoint:info.draggingLocation fromView:nil];
    const std::uint32_t action = dropAction([self dropMotion:info]);
    DeferredCall::callLater([target = drop.target.drop, x = static_cast<float>(local.x), y = static_cast<float>(local.y),
                             action, mime, bytes = std::move(*data)]() mutable { target(x, y, action, mime, std::move(bytes)); });
    _dropAccepted = NO;
    return YES;
  }
  return NO;
}
@end

CocoaSurface::CocoaSurface(CocoaDisplay& display) : m_display(display) {}

CocoaSurface::~CocoaSurface() { destroyView(); }

void CocoaSurface::createView(NSView* superview, NSRect frame) {
  m_view = [[AndromedaSurfaceView alloc] initWithOwner:this frame:frame];
  m_layer = [CAMetalLayer layer];
  m_layer.opaque = NO;
  m_layer.contentsScale = bufferScale();
  m_view.layer = m_layer;
  m_view.wantsLayer = YES;
  [superview addSubview:m_view];

  AndromedaLinkTarget* target = [[AndromedaLinkTarget alloc] init];
  target.owner = this;
  m_link = [m_view displayLinkWithTarget:target selector:@selector(tick:)];
  m_link.paused = YES;
  [m_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];

  m_display.registerSurface(this);
  attachRenderTarget();
}

void CocoaSurface::destroyView() {
  if (m_view == nil) {
    return;
  }
  releaseRenderState();
  m_display.unregisterSurface(this);
  [m_link invalidate];
  m_link = nil;
  clearBlurRegion();
  [m_view detach];
  [m_view removeFromSuperview];
  m_view = nil;
  m_layer = nil;
}

void CocoaSurface::seedScale(NSScreen* screen) {
  const auto scale = static_cast<std::int32_t>(std::max(1L, std::lround(screen != nil ? screen.backingScaleFactor : 1.0)));
  setBufferScale(scale);
  setConfiguredScaleNumerator(static_cast<std::uint32_t>(scale) * 120U);
}

void CocoaSurface::backingChanged() {
  NSWindow* window = this->window();
  if (window == nil) {
    return;
  }
  const auto scale = static_cast<std::int32_t>(std::max(1L, std::lround(window.backingScaleFactor)));
  updateOutputScale(scale, static_cast<std::uint32_t>(scale) * 120U);
  m_display.wake();
}

void CocoaSurface::applyNativeScale() {
  if (m_layer != nil) {
    m_layer.contentsScale = std::max(1, bufferScale());
  }
}

void CocoaSurface::setOpaque(bool opaque) {
  if (m_layer != nil) {
    m_layer.opaque = opaque;
  }
}

void CocoaSurface::setBlurRegion(const std::vector<InputRect>& rects) {
  if (m_view == nil || rects.empty()) {
    clearBlurRegion();
    return;
  }
  if (m_blur == nil) {
    m_blur = [[NSVisualEffectView alloc] initWithFrame:m_view.frame];
    m_blur.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    m_blur.material = NSVisualEffectMaterialUnderWindowBackground;
    m_blur.state = NSVisualEffectStateActive;
  }
  const std::vector<InputRect> region = rects;
  m_blur.maskImage = [NSImage imageWithSize:m_view.bounds.size
                                    flipped:YES
                             drawingHandler:^BOOL(NSRect /*dst*/) {
                               [NSColor.blackColor set];
                               for (const InputRect& r : region) {
                                 NSRectFill(NSMakeRect(r.x, r.y, r.width, r.height));
                               }
                               return YES;
                             }];
  syncBlur();
}

void CocoaSurface::clearBlurRegion() {
  [m_blur removeFromSuperview];
  m_blur = nil;
}

void CocoaSurface::syncBlur() {
  if (m_blur == nil || m_view.superview == nil) {
    return;
  }
  m_blur.frame = m_view.frame;
  m_blur.autoresizingMask = m_view.autoresizingMask;
  [m_view.superview addSubview:m_blur positioned:NSWindowBelow relativeTo:m_view];
}

bool CocoaSurface::acceptsPoint(NSPoint local) const {
  if (!m_inputRegion) {
    return true;
  }
  return std::ranges::any_of(*m_inputRegion, [local](const InputRect& r) {
    return local.x >= r.x && local.x < r.x + r.width && local.y >= r.y && local.y < r.y + r.height;
  });
}

std::unique_ptr<NativeEglWindow> CocoaSurface::createNativeWindow() { return std::make_unique<CocoaEglWindow>(m_layer); }

void CocoaSurface::requestFrameCallback() {
  m_frameRequested = true;
  m_link.paused = NO;
}

void CocoaSurface::cancelFrameCallback() {
  m_frameRequested = false;
  m_link.paused = YES;
}

void CocoaSurface::displayLinkFired(CFTimeInterval timestamp) {
  m_link.paused = YES;
  if (!m_frameRequested) {
    return;
  }
  m_frameRequested = false;
  frameDone(static_cast<std::uint32_t>(static_cast<std::uint64_t>(timestamp * 1000.0)));
  // AppKit runs its own loop during a live resize, so the frame work cannot wait for AppLoop.
  if (m_view.inLiveResize) {
    Surface::drainPendingFrameWork();
    Surface::drainPendingRenders();
  }
  m_display.wake();
}

void CocoaSurface::setDrop(std::optional<Drop> drop) {
  m_drop = std::move(drop);
  if (m_view == nil) {
    return;
  }
  [m_view unregisterDraggedTypes];
  if (!m_drop) {
    return;
  }
  NSMutableArray<NSPasteboardType>* types = [NSMutableArray array];
  for (const std::string& mime : m_drop->accepted) {
    if (NSString* type = cocoaPasteboardType(mime); type != nil && ![types containsObject:type]) {
      [types addObject:type];
    }
  }
  [m_view registerForDraggedTypes:types];
}

CocoaToplevel::~CocoaToplevel() {
  destroyView();
  if (m_window != nil) {
    static_cast<AndromedaWindowDelegate*>(m_delegate).owner = nullptr;
    m_window.delegate = nil;
    [m_window.parentWindow removeChildWindow:m_window];
    [m_window orderOut:nil];
    [m_window close];
    m_window = nil;
  }
}

bool CocoaToplevel::initialize(OutputId output, const ToplevelSurfaceConfig& config) {
  NSScreen* screen = CocoaDisplay::screenOf(output);
  if (screen == nil) {
    screen = NSScreen.mainScreen;
  }
  seedScale(screen);
  const NSRect content = NSMakeRect(0, 0, std::max(1U, config.width), std::max(1U, config.height));
  // The app draws its own header and window buttons, as on Wayland; AppKit keeps the edges, shadow and corners.
  const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
      | NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView;
  AndromedaWindow* window = [[AndromedaWindow alloc] initWithContentRect:content
                                                               styleMask:style
                                                                 backing:NSBackingStoreBuffered
                                                                   defer:NO
                                                                  screen:screen];
  window.keyable = YES;
  window.releasedWhenClosed = NO;
  window.titlebarAppearsTransparent = YES;
  window.titleVisibility = NSWindowTitleHidden;
  for (NSWindowButton button : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
    [window standardWindowButton:button].hidden = YES;
  }
  window.opaque = NO;
  window.backgroundColor = NSColor.clearColor;
  window.tabbingMode = NSWindowTabbingModeDisallowed;
  window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
  window.title = [NSString stringWithUTF8String:config.title.c_str()] ?: @"";
  setAccessibleTitle(config.title);
  if (config.minWidth > 0 || config.minHeight > 0) {
    window.contentMinSize = NSMakeSize(config.minWidth, config.minHeight);
  }
  if (config.maxWidth > 0 || config.maxHeight > 0) {
    window.contentMaxSize = NSMakeSize(config.maxWidth > 0 ? config.maxWidth : CGFLOAT_MAX,
                                       config.maxHeight > 0 ? config.maxHeight : CGFLOAT_MAX);
  }
  m_window = window;

  AndromedaWindowDelegate* delegate = [[AndromedaWindowDelegate alloc] init];
  delegate.owner = this;
  m_delegate = delegate;
  window.delegate = delegate;

  AndromedaContainerView* container = [[AndromedaContainerView alloc] initWithFrame:content];
  container.wantsLayer = YES;
  window.contentView = container;
  createView(container, container.bounds);
  m_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  m_view.takesKeyboard = YES;
  window.initialFirstResponder = m_view;
  [window center];

  onConfigure(static_cast<std::uint32_t>(content.size.width), static_cast<std::uint32_t>(content.size.height));
  setRunning(true);
  [window makeKeyAndOrderFront:nil];
  [window makeFirstResponder:m_view];
  if (config.maximized) {
    [window zoom:nil];
  }
  updateState();
  return true;
}

void CocoaToplevel::closeRequested() {
  // A close request, not a close: the owner may refuse it.
  if (m_closedCallback) {
    m_closedCallback();
  } else {
    setRunning(false);
  }
  m_display.wake();
}

void CocoaToplevel::contentResized() {
  const NSSize size = m_view.bounds.size;
  const auto width = static_cast<std::uint32_t>(std::max(1.0, size.width));
  const auto height = static_cast<std::uint32_t>(std::max(1.0, size.height));
  if (width != this->width() || height != this->height()) {
    onConfigure(width, height);
  }
  updateState();
  m_display.wake();
}

void CocoaToplevel::updateState() {
  if (m_window == nil) {
    return;
  }
  ToplevelState next;
  next.fullscreen = (m_window.styleMask & NSWindowStyleMaskFullScreen) != 0;
  next.maximized = !next.fullscreen && m_window.isZoomed;
  // A popup takes key focus from its window, which stays active as on Wayland.
  next.activated = m_window.isKeyWindow || NSApp.keyWindow.parentWindow == m_window;
  next.suspended = m_window.isMiniaturized || (m_window.occlusionState & NSWindowOcclusionStateVisible) == 0;
  if (next == m_state) {
    return;
  }
  m_state = next;
  if (m_stateCallback) {
    m_stateCallback(m_state);
  }
  m_display.wake();
}

void CocoaToplevel::setMinSize(std::uint32_t minWidth, std::uint32_t minHeight) {
  m_window.contentMinSize = NSMakeSize(minWidth, minHeight);
}

void CocoaToplevel::setTitle(const std::string& title) {
  m_window.title = [NSString stringWithUTF8String:title.c_str()] ?: @"";
  setAccessibleTitle(title);
}

void CocoaToplevel::beginMove(std::uint32_t /*serial*/) {
  NSEvent* event = NSApp.currentEvent;
  if (event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeLeftMouseDragged) {
    [m_window performWindowDragWithEvent:event];
  }
}

void CocoaToplevel::setMaximized(bool maximized) {
  if (maximized != m_window.isZoomed) {
    [m_window zoom:nil];
  }
}

void CocoaToplevel::setFullscreen(bool fullscreen) {
  if (fullscreen != ((m_window.styleMask & NSWindowStyleMaskFullScreen) != 0)) {
    [m_window toggleFullScreen:nil];
  }
}

void CocoaToplevel::setMinimized() { [m_window miniaturize:nil]; }

bool CocoaToplevel::requestWindowSize(std::uint32_t width, std::uint32_t height) {
  if (m_window == nil || m_state.maximized || m_state.fullscreen) {
    return false;
  }
  [m_window setContentSize:NSMakeSize(std::max(1U, width), std::max(1U, height))];
  return true;
}

// Handles are "cocoa:<windowNumber>", as exportHandle() makes them.
void CocoaToplevel::setParentWindow(std::string_view handle) {
  [m_window.parentWindow removeChildWindow:m_window];
  constexpr std::string_view prefix = "cocoa:";
  if (!handle.starts_with(prefix)) {
    return;
  }
  NSInteger number = 0;
  const std::string_view digits = handle.substr(prefix.size());
  if (std::from_chars(digits.data(), digits.data() + digits.size(), number).ec != std::errc{}) {
    return;
  }
  if (NSWindow* parent = [NSApp windowWithWindowNumber:number]; parent != nil && parent != m_window) {
    [parent addChildWindow:m_window ordered:NSWindowAbove];
  }
}

void CocoaToplevel::exportHandle(std::function<void(const std::string& handle)> ready) {
  ready(m_window != nil ? "cocoa:" + std::to_string(m_window.windowNumber) : std::string());
}

bool CocoaToplevel::setCaptureSecure(bool secure) {
  m_window.sharingType = secure ? NSWindowSharingNone : NSWindowSharingReadOnly;
  return true;
}

CocoaPopup::~CocoaPopup() {
  if (m_clickMonitor != nil) {
    [NSEvent removeMonitor:m_clickMonitor];
    m_clickMonitor = nil;
  }
  if (m_resignObserver != nil) {
    [NSNotificationCenter.defaultCenter removeObserver:m_resignObserver];
    m_resignObserver = nil;
  }
  destroyView();
  if (m_window != nil) {
    [m_window.parentWindow removeChildWindow:m_window];
    [m_window orderOut:nil];
    [m_window close];
    m_window = nil;
  }
}

bool CocoaPopup::initialize(const PopupParent& parent, PopupSurfaceConfig config) {
  m_parent = dynamic_cast<CocoaSurface*>(parent.surface);
  if (m_parent == nullptr || m_parent->view() == nil || m_parent->window() == nil) {
    return false;
  }
  m_config = config;
  m_config.anchorWidth = std::max(m_config.anchorWidth, 1);
  m_config.anchorHeight = std::max(m_config.anchorHeight, 1);
  m_config.width = std::max(m_config.width, 1U);
  m_config.height = std::max(m_config.height, 1U);
  seedScale(m_parent->window().screen);

  const NSRect content = NSMakeRect(0, 0, m_config.width, m_config.height);
  AndromedaWindow* window = [[AndromedaWindow alloc] initWithContentRect:content
                                                               styleMask:NSWindowStyleMaskBorderless
                                                                 backing:NSBackingStoreBuffered
                                                                   defer:NO];
  window.keyable = m_config.grab;
  window.releasedWhenClosed = NO;
  window.opaque = NO;
  window.backgroundColor = NSColor.clearColor;
  window.hasShadow = NO;
  window.level = NSPopUpMenuWindowLevel;
  window.ignoresMouseEvents = !m_config.grab && !m_config.reactive;
  m_window = window;

  AndromedaContainerView* container = [[AndromedaContainerView alloc] initWithFrame:content];
  container.wantsLayer = YES;
  window.contentView = container;
  createView(container, container.bounds);
  m_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  m_view.takesKeyboard = m_config.grab;
  place();
  [m_parent->window() addChildWindow:window ordered:NSWindowAbove];
  onConfigure(m_config.width, m_config.height);
  setRunning(true);

  if (m_config.grab) {
    [window makeKeyAndOrderFront:nil];
    [window makeFirstResponder:m_view];
    NSWindow* own = window;
    m_clickMonitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown
                                     handler:^NSEvent*(NSEvent* event) {
                                       for (NSWindow* w = event.window; w != nil; w = w.parentWindow) {
                                         if (w == own) {
                                           return event;
                                         }
                                       }
                                       // The dismissing click goes to no surface, as with an xdg_popup grab.
                                       dismiss();
                                       return nil;
                                     }];
    m_resignObserver = [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidResignActiveNotification
                                                                       object:nil
                                                                        queue:nil
                                                                   usingBlock:^(NSNotification*) {
                                                                     dismiss();
                                                                   }];
  } else {
    [window orderFront:nil];
  }
  return true;
}

void CocoaPopup::place() {
  NSWindow* parentWindow = m_parent->window();
  NSScreen* screen = parentWindow.screen ?: NSScreen.mainScreen;
  // AppKit screen space has its origin at the bottom left of the primary screen; placement works top down.
  const CGFloat top = NSScreen.screens.firstObject.frame.size.height;
  const NSRect parent = [parentWindow convertRectToScreen:[m_parent->view() convertRect:m_parent->view().bounds toView:nil]];
  const NSRect visible = screen.visibleFrame;
  const auto parentX = static_cast<std::int32_t>(parent.origin.x);
  const auto parentY = static_cast<std::int32_t>(top - NSMaxY(parent));
  const PopupBox bounds{
      .x = static_cast<std::int32_t>(visible.origin.x),
      .y = static_cast<std::int32_t>(top - NSMaxY(visible)),
      .width = static_cast<std::int32_t>(visible.size.width),
      .height = static_cast<std::int32_t>(visible.size.height),
  };
  const PopupBox box = placePopup(m_config, parentX, parentY, bounds);
  m_configuredX = box.x - parentX;
  m_configuredY = box.y - parentY;
  [m_window setFrame:NSMakeRect(box.x, top - box.y - box.height, box.width, box.height) display:NO];
}

bool CocoaPopup::resize(std::uint32_t width, std::uint32_t height, bool /*commit*/) {
  if (m_window == nil) {
    return false;
  }
  m_config.width = std::max(width, 1U);
  m_config.height = std::max(height, 1U);
  place();
  onConfigure(m_config.width, m_config.height);
  return true;
}

bool CocoaPopup::repositionAnchor(const PopupSurfaceConfig& anchorConfig, bool /*commit*/) {
  if (m_window == nil) {
    return false;
  }
  const std::uint32_t width = m_config.width;
  const std::uint32_t height = m_config.height;
  const bool grab = m_config.grab;
  m_config = anchorConfig;
  m_config.grab = grab;
  m_config.anchorWidth = std::max(m_config.anchorWidth, 1);
  m_config.anchorHeight = std::max(m_config.anchorHeight, 1);
  m_config.width = std::max(m_config.width, 1U);
  m_config.height = std::max(m_config.height, 1U);
  place();
  if (m_config.width != width || m_config.height != height) {
    onConfigure(m_config.width, m_config.height);
  }
  return true;
}

void CocoaPopup::dismiss() {
  if (!isRunning()) {
    return;
  }
  setRunning(false);
  [m_window orderOut:nil];
  auto callback = std::move(m_dismissedCallback);
  if (callback) {
    callback();
  }
  m_display.wake();
}

bool CocoaSubsurface::initialize(const Surface& parent, SubsurfaceConfig config) {
  m_parent = dynamic_cast<CocoaSurface*>(const_cast<Surface*>(&parent));
  if (m_parent == nullptr || m_parent->view().superview == nil || config.width == 0 || config.height == 0) {
    return false;
  }
  m_config = config;
  seedScale(m_parent->window().screen);
  createView(m_parent->view().superview, frame());
  applyStacking();
  setRunning(true);
  onConfigure(m_config.width, m_config.height);
  return true;
}

NSRect CocoaSubsurface::frame() const {
  const NSPoint origin = m_parent->view().frame.origin;
  return NSMakeRect(origin.x + m_config.x, origin.y + m_config.y, m_config.width, m_config.height);
}

void CocoaSubsurface::applyStacking() {
  [m_view.superview addSubview:m_view
                    positioned:m_config.stacking == SubsurfaceStacking::BelowParent ? NSWindowBelow : NSWindowAbove
                    relativeTo:m_parent->view()];
  syncBlur();
}

void CocoaSubsurface::setPosition(std::int32_t x, std::int32_t y) {
  m_config.x = x;
  m_config.y = y;
  if (m_view != nil) {
    m_view.frame = frame();
    syncBlur();
  }
}

void CocoaSubsurface::resize(std::uint32_t width, std::uint32_t height) {
  m_config.width = width;
  m_config.height = height;
  if (m_view == nil || width == 0 || height == 0) {
    return;
  }
  m_view.frame = frame();
  syncBlur();
  if (isRunning() && (width != this->width() || height != this->height())) {
    onConfigure(width, height);
  }
}

void CocoaSubsurface::setStacking(SubsurfaceStacking stacking) {
  if (m_config.stacking == stacking) {
    return;
  }
  m_config.stacking = stacking;
  if (m_view != nil) {
    applyStacking();
  }
}
