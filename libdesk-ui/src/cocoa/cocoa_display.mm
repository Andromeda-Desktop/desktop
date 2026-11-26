#include "cocoa/cocoa_internal.h"

#include "core/deferred_call.h"
#include "protocols/uri.h"
#include "core/input/cursor_shape.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "core/sys/sys.h"
#include "platform/text_input_service.h"

#include <cstdlib>
#include <sys/event.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("cocoa");
  // NSEventTypeApplicationDefined subtype of the event that ends a blocking wait().
  constexpr short kWakeSubtype = 0x414e;

  CocoaDisplay* g_display = nullptr;

  class CocoaTextInputService final : public TextInputService {
  public:
    [[nodiscard]] bool isAvailable() const noexcept override { return false; }
    void setFocusedClient(Surface*, TextInputClient*, bool, Surface*) override {}
    void clearFocusedClient(TextInputClient*) override {}
    void notifyClientStateChanged(TextInputClient*, TextInputChangeCause) override {}
  };

  NSCursor* cursorFor(CursorShape shape) {
    auto frame = [](NSCursorFrameResizePosition position) {
      return [NSCursor frameResizeCursorFromPosition:position inDirections:NSCursorFrameResizeDirectionsAll];
    };
    switch (shape) {
    case CursorShape::ContextMenu:
      return NSCursor.contextualMenuCursor;
    case CursorShape::Pointer:
      return NSCursor.pointingHandCursor;
    case CursorShape::Cell:
    case CursorShape::Crosshair:
      return NSCursor.crosshairCursor;
    case CursorShape::Text:
      return NSCursor.IBeamCursor;
    case CursorShape::VerticalText:
      return NSCursor.IBeamCursorForVerticalLayout;
    case CursorShape::Alias:
      return NSCursor.dragLinkCursor;
    case CursorShape::Copy:
    case CursorShape::DndAsk:
      return NSCursor.dragCopyCursor;
    case CursorShape::NoDrop:
    case CursorShape::NotAllowed:
      return NSCursor.operationNotAllowedCursor;
    case CursorShape::Grab:
    case CursorShape::Move:
    case CursorShape::AllScroll:
    case CursorShape::AllResize:
      return NSCursor.openHandCursor;
    case CursorShape::Grabbing:
      return NSCursor.closedHandCursor;
    case CursorShape::EResize:
    case CursorShape::WResize:
    case CursorShape::EwResize:
    case CursorShape::ColResize:
      return frame(NSCursorFrameResizePositionRight);
    case CursorShape::NResize:
    case CursorShape::SResize:
    case CursorShape::NsResize:
    case CursorShape::RowResize:
      return frame(NSCursorFrameResizePositionBottom);
    case CursorShape::NeResize:
    case CursorShape::SwResize:
    case CursorShape::NeswResize:
      return frame(NSCursorFrameResizePositionTopRight);
    case CursorShape::NwResize:
    case CursorShape::SeResize:
    case CursorShape::NwseResize:
      return frame(NSCursorFrameResizePositionBottomRight);
    case CursorShape::ZoomIn:
    case CursorShape::ZoomOut:
    case CursorShape::Default:
    case CursorShape::Help:
    case CursorShape::Progress:
    case CursorShape::Wait:
      break;
    }
    return NSCursor.arrowCursor;
  }

  void kqueueReady(CFFileDescriptorRef /*fd*/, CFOptionFlags /*types*/, void* info) {
    static_cast<CocoaDisplay*>(info)->wake();
  }
} // namespace

std::uint32_t cocoaKeyMods(NSEventModifierFlags flags) noexcept {
  std::uint32_t mods = 0;
  if ((flags & NSEventModifierFlagShift) != 0) {
    mods |= KeyMod::Shift;
  }
  if ((flags & NSEventModifierFlagControl) != 0) {
    mods |= KeyMod::Ctrl;
  }
  if ((flags & NSEventModifierFlagOption) != 0) {
    mods |= KeyMod::Alt;
  }
  if ((flags & NSEventModifierFlagCommand) != 0) {
    mods |= KeyMod::Super;
  }
  return mods;
}

NSString* cocoaPasteboardType(const std::string& mime) {
  if (mime.starts_with("text/plain") || mime == "UTF8_STRING" || mime == "STRING" || mime == "TEXT") {
    return NSPasteboardTypeString;
  }
  if (mime == uri::kUriListMime) {
    return NSPasteboardTypeFileURL;
  }
  if (mime == "text/html") {
    return NSPasteboardTypeHTML;
  }
  if (mime == "image/png") {
    return NSPasteboardTypePNG;
  }
  return nil;
}

std::optional<std::string> cocoaReadPasteboard(NSPasteboard* pasteboard, NSString* type) {
  if (type == NSPasteboardTypeFileURL) {
    NSArray<NSURL*>* urls = [pasteboard readObjectsForClasses:@[ NSURL.class ] options:@{}];
    if (urls.count == 0) {
      return std::nullopt;
    }
    std::string list;
    for (NSURL* url in urls) {
      list += url.absoluteString.UTF8String;
      list += "\r\n";
    }
    return list;
  }
  if (type == NSPasteboardTypeString || type == NSPasteboardTypeHTML) {
    NSString* text = [pasteboard stringForType:type];
    return text != nil ? std::optional<std::string>(text.UTF8String) : std::nullopt;
  }
  NSData* data = [pasteboard dataForType:type];
  return data != nil ? std::optional<std::string>(std::string(static_cast<const char*>(data.bytes), data.length))
                     : std::nullopt;
}

@interface AndromedaAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AndromedaAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  if (g_display != nullptr) {
    g_display->requestQuit();
  }
  return NSTerminateCancel;
}
@end

std::unique_ptr<Display> createDisplay() { return std::make_unique<CocoaDisplay>(); }

CocoaDisplay::CocoaDisplay() {
  g_display = this;
  // Homebrew's fontconfig has no usable built-in config path; a bundle carries its own (tools/macos/fonts.conf).
  if (const std::string exe = sys::executablePath(); !exe.empty() && std::getenv("FONTCONFIG_FILE") == nullptr) {
    const std::string conf = exe.substr(0, exe.rfind('/')) + "/../Resources/fonts/fonts.conf";
    if (::access(conf.c_str(), R_OK) == 0) {
      ::setenv("FONTCONFIG_FILE", conf.c_str(), 0);
    }
  }
}

CocoaDisplay::~CocoaDisplay() {
  disconnect();
  g_display = nullptr;
}

bool CocoaDisplay::connect(std::optional<std::chrono::steady_clock::time_point> /*deadline*/) {
  if (m_connected) {
    return true;
  }
  @autoreleasepool {
    [NSApplication sharedApplication];
    NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
    m_appDelegate = [[AndromedaAppDelegate alloc] init];
    NSApp.delegate = m_appDelegate;

    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Hide" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [bar addItem:appItem];
    NSApp.mainMenu = bar;
    [NSApp finishLaunching];

    m_kqueue = ::kqueue();
    if (m_kqueue < 0) {
      kLog.error("kqueue failed");
      return false;
    }
    CFFileDescriptorContext context{0, this, nullptr, nullptr, nullptr};
    m_kqueueSource = CFFileDescriptorCreate(kCFAllocatorDefault, m_kqueue, false, kqueueReady, &context);
    m_kqueueRunLoopSource = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, m_kqueueSource, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), m_kqueueRunLoopSource, kCFRunLoopCommonModes);

    m_clipboardChangeCount = NSPasteboard.generalPasteboard.changeCount;
    m_activeObserver = [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidBecomeActiveNotification
                                                                       object:nil
                                                                        queue:nil
                                                                   usingBlock:^(NSNotification*) {
                                                                     checkClipboard();
                                                                   }];
    [NSApp activate];
  }
  m_connected = true;
  return true;
}

void CocoaDisplay::disconnect() {
  if (m_activeObserver != nil) {
    [NSNotificationCenter.defaultCenter removeObserver:m_activeObserver];
    m_activeObserver = nil;
  }
  if (m_kqueueRunLoopSource != nullptr) {
    CFRunLoopRemoveSource(CFRunLoopGetMain(), m_kqueueRunLoopSource, kCFRunLoopCommonModes);
    CFRelease(m_kqueueRunLoopSource);
    m_kqueueRunLoopSource = nullptr;
  }
  if (m_kqueueSource != nullptr) {
    CFFileDescriptorInvalidate(m_kqueueSource);
    CFRelease(m_kqueueSource);
    m_kqueueSource = nullptr;
  }
  if (m_kqueue >= 0) {
    ::close(m_kqueue);
    m_kqueue = -1;
  }
  m_connected = false;
}

void CocoaDisplay::wake() const {
  if (m_wakePosted) {
    return;
  }
  m_wakePosted = true;
  NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                       subtype:kWakeSubtype
                                         data1:0
                                         data2:0];
  [NSApp postEvent:event atStart:NO];
}

// The app's fds go into a kqueue that a CFFileDescriptor watches, so one AppKit event wait covers both.
int CocoaDisplay::wait(pollfd* fds, nfds_t count, int timeoutMs) {
  @autoreleasepool {
    // An AppKit tracking loop may have dequeued the last wake event.
    m_wakePosted = false;
    auto dispatch = [this](NSEvent* event) {
      if (event.type == NSEventTypeApplicationDefined && event.subtype == kWakeSubtype) {
        m_wakePosted = false;
        return;
      }
      [NSApp sendEvent:event];
    };
    auto drain = [&] {
      while (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                 untilDate:NSDate.distantPast
                                                    inMode:NSDefaultRunLoopMode
                                                   dequeue:YES]) {
        dispatch(event);
      }
      [NSApp updateWindows];
    };

    if (const int ready = ::poll(fds, count, 0); ready != 0 || timeoutMs == 0 || m_kqueue < 0) {
      drain();
      return ready;
    }

    std::vector<struct kevent> changes;
    changes.reserve(count);
    for (nfds_t i = 0; i < count; ++i) {
      if (fds[i].fd < 0) {
        continue;
      }
      const auto fd = static_cast<uintptr_t>(fds[i].fd);
      if ((fds[i].events & POLLIN) != 0) {
        changes.push_back({fd, EVFILT_READ, EV_ADD | EV_ONESHOT | EV_RECEIPT, 0, 0, nullptr});
      }
      if ((fds[i].events & POLLOUT) != 0) {
        changes.push_back({fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT | EV_RECEIPT, 0, 0, nullptr});
      }
    }
    std::vector<struct kevent> receipts(changes.size());
    const timespec zero{0, 0};
    const auto n = static_cast<int>(changes.size());
    ::kevent(m_kqueue, changes.data(), n, receipts.data(), n, &zero);
    CFFileDescriptorEnableCallBacks(m_kqueueSource, kCFFileDescriptorReadCallBack);

    NSDate* until = timeoutMs < 0 ? NSDate.distantFuture : [NSDate dateWithTimeIntervalSinceNow:timeoutMs / 1000.0];
    if (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES]) {
      dispatch(event);
    }
    drain();

    for (auto& change : changes) {
      change.flags = EV_DELETE | EV_RECEIPT;
    }
    ::kevent(m_kqueue, changes.data(), n, receipts.data(), n, &zero);
    return ::poll(fds, count, 0);
  }
}

std::unique_ptr<Toplevel> CocoaDisplay::createToplevel() { return std::make_unique<CocoaToplevel>(*this); }

std::unique_ptr<Popup> CocoaDisplay::createPopup() { return std::make_unique<CocoaPopup>(*this); }

std::unique_ptr<Subsurface> CocoaDisplay::createSubsurface() { return std::make_unique<CocoaSubsurface>(*this); }

void CocoaDisplay::setCursorShape(std::uint32_t /*serial*/, std::uint32_t shape) {
  [cursorFor(static_cast<CursorShape>(shape)) set];
}

std::uint32_t CocoaDisplay::keyboardModifiers() const noexcept { return cocoaKeyMods(NSEvent.modifierFlags); }

OutputId CocoaDisplay::outputOf(NSScreen* screen) {
  if (screen == nil) {
    return OutputId::None;
  }
  NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
  return static_cast<OutputId>(number.unsignedIntValue);
}

NSScreen* CocoaDisplay::screenOf(OutputId output) {
  for (NSScreen* screen in NSScreen.screens) {
    if (outputOf(screen) == output) {
      return screen;
    }
  }
  return nil;
}

OutputId CocoaDisplay::lastPointerOutputId() const noexcept {
  const NSPoint at = NSEvent.mouseLocation;
  for (NSScreen* screen in NSScreen.screens) {
    if (NSPointInRect(at, screen.frame)) {
      return outputOf(screen);
    }
  }
  return firstOutputId();
}

OutputId CocoaDisplay::firstOutputId() const noexcept { return outputOf(NSScreen.screens.firstObject); }

std::optional<OutputInfo> CocoaDisplay::outputInfo(OutputId output) const {
  NSScreen* screen = screenOf(output);
  if (screen == nil) {
    return std::nullopt;
  }
  return OutputInfo{
      .logicalWidth = static_cast<std::int32_t>(screen.frame.size.width),
      .logicalHeight = static_cast<std::int32_t>(screen.frame.size.height),
      .scale = static_cast<float>(screen.backingScaleFactor),
  };
}

OutputId CocoaDisplay::outputForSurface(const Surface& surface) const noexcept {
  const auto* cocoa = dynamic_cast<const CocoaSurface*>(&surface);
  return cocoa != nullptr ? outputOf(cocoa->window().screen) : OutputId::None;
}

bool CocoaDisplay::isLive(const Surface* surface) const noexcept {
  const auto* cocoa = dynamic_cast<const CocoaSurface*>(surface);
  return cocoa != nullptr && m_surfaces.contains(const_cast<CocoaSurface*>(cocoa));
}

void CocoaDisplay::registerSurface(CocoaSurface* surface) { m_surfaces.insert(surface); }

void CocoaDisplay::unregisterSurface(CocoaSurface* surface) {
  m_surfaces.erase(surface);
  if (m_pointerSurface == &surface->surface()) {
    m_pointerSurface = nullptr;
  }
  if (m_keyboardSurface == &surface->surface()) {
    m_keyboardSurface = nullptr;
  }
}

void CocoaDisplay::pointer(PointerEvent event) {
  event.serial = ++m_serial;
  m_lastInputSource = InputSource::Pointer;
  if (event.type == PointerEvent::Type::Leave) {
    if (m_pointerSurface == event.surface) {
      m_pointerSurface = nullptr;
    }
  } else {
    m_pointerSurface = event.surface;
    m_pointerX = event.sx;
    m_pointerY = event.sy;
  }
  if (m_pointerCallback) {
    m_pointerCallback(event);
  }
  wake();
}

void CocoaDisplay::pinch(const PinchEvent& event) {
  m_lastInputSource = InputSource::Pointer;
  if (m_pinchCallback) {
    m_pinchCallback(event);
  }
  wake();
}

void CocoaDisplay::axisStop(Surface* surface, std::uint32_t axis) {
  if (m_axisStopCallback) {
    m_axisStopCallback(surface, axis);
  }
  wake();
}

void CocoaDisplay::keyboard(const KeyboardEvent& event, Surface* surface) {
  ++m_serial;
  m_lastInputSource = InputSource::Keyboard;
  m_keyboardSurface = surface;
  if (m_keyboardCallback) {
    m_keyboardCallback(event);
  }
  wake();
}

void CocoaDisplay::keyboardFocus(Surface* surface, bool focused) {
  if (focused) {
    m_keyboardSurface = surface;
  } else if (m_keyboardSurface == surface) {
    m_keyboardSurface = nullptr;
  }
  wake();
}

// macOS sends no clipboard change event: the change count is checked when the app becomes active.
void CocoaDisplay::checkClipboard() {
  const NSInteger count = NSPasteboard.generalPasteboard.changeCount;
  if (count == m_clipboardChangeCount) {
    return;
  }
  m_clipboardChangeCount = count;
  if (m_clipboardChanged) {
    m_clipboardChanged();
  }
  wake();
}

void CocoaDisplay::readClipboard(
    const std::vector<std::string>& accepted, std::function<void(const std::string& mime, std::string bytes)> done,
    std::size_t maxBytes
) {
  std::string mime;
  std::string bytes;
  @autoreleasepool {
    NSPasteboard* pasteboard = NSPasteboard.generalPasteboard;
    for (const std::string& candidate : accepted) {
      NSString* type = cocoaPasteboardType(candidate);
      if (type == nil) {
        continue;
      }
      if (auto data = cocoaReadPasteboard(pasteboard, type)) {
        mime = candidate;
        if (data->size() <= maxBytes) {
          bytes = std::move(*data);
        }
        break;
      }
    }
  }
  DeferredCall::callLater([done = std::move(done), mime = std::move(mime), bytes = std::move(bytes)]() mutable {
    done(mime, std::move(bytes));
  });
}

bool CocoaDisplay::setClipboard(
    std::uint32_t /*serial*/, std::vector<std::string> mimeTypes, std::function<std::string(const std::string& mime)> data
) {
  @autoreleasepool {
    NSPasteboard* pasteboard = NSPasteboard.generalPasteboard;
    [pasteboard clearContents];
    NSMutableSet<NSString*>* written = [NSMutableSet set];
    for (const std::string& mime : mimeTypes) {
      NSString* type = cocoaPasteboardType(mime);
      if (type == nil || [written containsObject:type]) {
        continue;
      }
      [written addObject:type];
      const std::string bytes = data(mime);
      NSString* text = [[NSString alloc] initWithBytes:bytes.data() length:bytes.size() encoding:NSUTF8StringEncoding];
      if (type == NSPasteboardTypeFileURL) {
        NSMutableArray<NSURL*>* urls = [NSMutableArray array];
        for (NSString* line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
          NSURL* url = line.length > 0 && ![line hasPrefix:@"#"] ? [NSURL URLWithString:line] : nil;
          if (url != nil) {
            [urls addObject:url];
          }
        }
        [pasteboard writeObjects:urls];
      } else if (type == NSPasteboardTypeString || type == NSPasteboardTypeHTML) {
        [pasteboard setString:text ?: @"" forType:type];
      } else {
        [pasteboard setData:[NSData dataWithBytes:bytes.data() length:bytes.size()] forType:type];
      }
    }
    m_clipboardChangeCount = pasteboard.changeCount;
  }
  return true;
}

void CocoaDisplay::ringBell(const Surface& /*surface*/) const { NSBeep(); }

std::string CocoaDisplay::requestActivationToken(const Surface& /*surface*/, const std::string& /*appId*/) const {
  return {};
}

void CocoaDisplay::activateSurface(const Surface& surface, const std::string& /*token*/) const {
  [NSApp activate];
  if (const auto* cocoa = dynamic_cast<const CocoaSurface*>(&surface); cocoa != nullptr) {
    [cocoa->window() makeKeyAndOrderFront:nil];
  }
}

void CocoaDisplay::requestAttentionTokenAsync(const Surface& /*surface*/, std::function<void(std::string token)> done) const {
  [NSApp requestUserAttention:NSInformationalRequest];
  done({});
}

bool CocoaDisplay::setDropTarget(const Surface& surface, std::vector<std::string> acceptedTypes, DropTarget target) {
  auto* cocoa = dynamic_cast<CocoaSurface*>(const_cast<Surface*>(&surface));
  if (cocoa == nullptr || cocoa->view() == nil) {
    return false;
  }
  cocoa->setDrop(CocoaSurface::Drop{std::move(acceptedTypes), std::move(target)});
  return true;
}

void CocoaDisplay::clearDropTarget(const Surface& surface) {
  if (auto* cocoa = dynamic_cast<CocoaSurface*>(const_cast<Surface*>(&surface)); cocoa != nullptr) {
    cocoa->setDrop(std::nullopt);
  }
}

// No NSDraggingSession source yet: a drag stays inside its window.
bool CocoaDisplay::startDrag(const Surface& /*origin*/, DragOffer /*source*/) { return false; }

std::unique_ptr<Surface> CocoaDisplay::createDragIcon(std::uint32_t /*width*/, std::uint32_t /*height*/, std::int32_t /*bufferScale*/) {
  return nullptr;
}

std::unique_ptr<TextInputService> CocoaDisplay::createTextInputService() {
  return std::make_unique<CocoaTextInputService>();
}

void CocoaDisplay::requestQuit() {
  std::vector<CocoaToplevel*> toplevels;
  for (CocoaSurface* surface : m_surfaces) {
    if (auto* toplevel = dynamic_cast<CocoaToplevel*>(surface); toplevel != nullptr) {
      toplevels.push_back(toplevel);
    }
  }
  for (CocoaToplevel* toplevel : toplevels) {
    if (m_surfaces.contains(toplevel)) {
      toplevel->closeRequested();
    }
  }
  wake();
}
