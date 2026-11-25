#include "wayland/wayland.h"

#include "core/fd_watch.h"
#include "core/poll_loop.h"

#include <algorithm>
#include <andromeda-remote-input-v1-client-protocol.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <drm_fourcc.h>
#include <ext-data-control-v1-client-protocol.h>
#include <ext-foreign-toplevel-list-v1-client-protocol.h>
#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <fcntl.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <wayland-client.h>
#include <xdg-output-unstable-v1-client-protocol.h>

namespace xdpu {

  namespace {

    constexpr uint32_t kSourceMonitor = 1;
    constexpr uint32_t kSourceWindow = 2;

    WaylandContext* g_defaultContext = nullptr;

  } // namespace

  struct WaylandContext::Impl {
    struct OutputState {
      wl_output* output = nullptr;
      zxdg_output_v1* xdgOutput = nullptr;
      Impl* owner = nullptr;
      OutputInfo info;
      uint32_t registryName = 0;
      bool hasName = false;
      uint32_t transform = 0;
      uint32_t pendingTransform = 0;
    };

    struct ToplevelState {
      ext_foreign_toplevel_handle_v1* handle = nullptr;
      ToplevelInfo info;
      ToplevelInfo pending;
      bool closed = false;
    };

    explicit Impl(WaylandContext& owner, std::function<void()> onDisconnected)
        : owner(owner), onDisconnected(std::move(onDisconnected)) {}

    WaylandContext& owner;
    std::function<void()> onDisconnected;
    std::unique_ptr<FdWatch> displayWatch; // null until connected
    wl_display* display = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    uint32_t seatRegistryName = 0;
    wl_registry* registry = nullptr;
    wl_shm* shm = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    ext_output_image_capture_source_manager_v1* outputSourceManager = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* toplevelSourceManager = nullptr;
    ext_image_copy_capture_manager_v1* captureManager = nullptr;
    andromeda_remote_input_manager_v1* remoteInputManager = nullptr;
    ext_data_control_manager_v1* dataControlManager = nullptr;
    ext_foreign_toplevel_list_v1* toplevelList = nullptr;
    zxdg_output_manager_v1* xdgOutputManager = nullptr;

    bool preparedRead = false;
    bool wantsWrite = false;
    bool disconnected = false;
    uint32_t nextSyntheticOutput = 1;

    std::vector<CaptureSession*> captures;
    std::vector<OutputInfo> outputs;
    std::vector<ToplevelInfo> toplevels;
    std::optional<std::string> focusedOutput;
    std::set<std::string> captureBlocked;
    std::unordered_map<wl_output*, std::unique_ptr<OutputState>> outputStates;
    std::unordered_map<ext_foreign_toplevel_handle_v1*, std::unique_ptr<ToplevelState>> toplevelStates;

    void rebuildOutputs() {
      outputs.clear();
      outputs.reserve(outputStates.size());
      for (const auto& [object, state] : outputStates) {
        (void)object;
        OutputInfo info = state->info;
        if (info.name.empty()) {
          info.name = "output-" + std::to_string(state->registryName == 0 ? nextSyntheticOutput : state->registryName);
        }
        outputs.push_back(std::move(info));
      }
      std::ranges::sort(outputs, {}, &OutputInfo::name);
    }

    void rebuildToplevels() {
      toplevels.clear();
      toplevels.reserve(toplevelStates.size());
      for (const auto& [handle, state] : toplevelStates) {
        (void)handle;
        if (!state->closed && !state->info.identifier.empty()) {
          toplevels.push_back(state->info);
        }
      }
      std::ranges::sort(toplevels, {}, &ToplevelInfo::title);
    }

    void cancelPreparedRead() {
      if (preparedRead) {
        wl_display_cancel_read(display);
        preparedRead = false;
      }
    }

    void markDisconnected() {
      if (disconnected) {
        return;
      }
      disconnected = true;
      cancelPreparedRead();
      fprintf(stderr, "wayland: compositor connection closed\n");
      if (displayWatch) {
        displayWatch->setEvents(0);
      }
      if (onDisconnected) {
        onDisconnected();
      }
    }

    [[nodiscard]] short displayEvents() const {
      if (display == nullptr || disconnected) {
        return 0;
      }
      return static_cast<short>(POLLIN | (wantsWrite ? POLLOUT : 0));
    }

    void updateDisplayFdEvents() {
      if (displayWatch) {
        displayWatch->setEvents(displayEvents());
      }
    }

    void flushDisplay() {
      while (true) {
        const int rc = wl_display_flush(display);
        if (rc >= 0) {
          wantsWrite = false;
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          wantsWrite = true;
          break;
        }
        markDisconnected();
        break;
      }
      updateDisplayFdEvents();
    }

    void prepareRead() {
      if (disconnected || preparedRead) {
        return;
      }

      while (wl_display_prepare_read(display) < 0) {
        if (wl_display_dispatch_pending(display) < 0) {
          markDisconnected();
          return;
        }
      }
      preparedRead = true;
      flushDisplay();
    }

    void handleDisplayEvents(short events) {
      if (disconnected) {
        return;
      }

      if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        markDisconnected();
        return;
      }

      if ((events & POLLOUT) != 0) {
        flushDisplay();
      }

      if ((events & POLLIN) != 0) {
        if (preparedRead) {
          if (wl_display_read_events(display) < 0) {
            preparedRead = false;
            markDisconnected();
            return;
          }
          preparedRead = false;
        }
        if (wl_display_dispatch_pending(display) < 0) {
          markDisconnected();
          return;
        }
      }

      prepareRead();
    }

    OutputState* findOutput(wl_output* output) const {
      const auto iter = outputStates.find(output);
      return iter == outputStates.end() ? nullptr : iter->second.get();
    }

    OutputState* findOutput(const std::string& name) const {
      for (const auto& [object, state] : outputStates) {
        (void)object;
        const std::string& stateName = state->info.name;
        if (stateName == name || (stateName.empty() && ("output-" + std::to_string(state->registryName)) == name)) {
          return state.get();
        }
      }
      return nullptr;
    }

    ToplevelState* findToplevel(const std::string& identifier) const {
      for (const auto& [handle, state] : toplevelStates) {
        (void)handle;
        if (!state->closed && state->info.identifier == identifier) {
          return state.get();
        }
      }
      return nullptr;
    }

    void destroyObjects() {
      for (auto& [handle, state] : toplevelStates) {
        (void)state;
        ext_foreign_toplevel_handle_v1_destroy(handle);
      }
      toplevelStates.clear();

      if (toplevelList != nullptr) {
        ext_foreign_toplevel_list_v1_destroy(toplevelList);
        toplevelList = nullptr;
      }
      if (pointer != nullptr) {
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(pointer)) >= WL_POINTER_RELEASE_SINCE_VERSION) {
          wl_pointer_release(pointer);
        } else {
          wl_pointer_destroy(pointer);
        }
        pointer = nullptr;
      }
      if (seat != nullptr) {
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(seat)) >= WL_SEAT_RELEASE_SINCE_VERSION) {
          wl_seat_release(seat);
        } else {
          wl_seat_destroy(seat);
        }
        seat = nullptr;
      }
      seatRegistryName = 0;
      if (captureManager != nullptr) {
        ext_image_copy_capture_manager_v1_destroy(captureManager);
        captureManager = nullptr;
      }
      if (remoteInputManager != nullptr) {
        andromeda_remote_input_manager_v1_destroy(remoteInputManager);
        remoteInputManager = nullptr;
      }
      if (dataControlManager != nullptr) {
        ext_data_control_manager_v1_destroy(dataControlManager);
        dataControlManager = nullptr;
      }
      if (toplevelSourceManager != nullptr) {
        ext_foreign_toplevel_image_capture_source_manager_v1_destroy(toplevelSourceManager);
        toplevelSourceManager = nullptr;
      }
      if (outputSourceManager != nullptr) {
        ext_output_image_capture_source_manager_v1_destroy(outputSourceManager);
        outputSourceManager = nullptr;
      }
      if (dmabuf != nullptr) {
        zwp_linux_dmabuf_v1_destroy(dmabuf);
        dmabuf = nullptr;
      }
      if (shm != nullptr) {
        wl_shm_destroy(shm);
        shm = nullptr;
      }
      for (auto& [output, state] : outputStates) {
        if (state->xdgOutput != nullptr) {
          zxdg_output_v1_destroy(state->xdgOutput);
        }
        wl_output_destroy(output);
      }
      if (xdgOutputManager != nullptr) {
        zxdg_output_manager_v1_destroy(xdgOutputManager);
        xdgOutputManager = nullptr;
      }
      outputStates.clear();
      if (registry != nullptr) {
        wl_registry_destroy(registry);
        registry = nullptr;
      }
    }
  };

  struct WaylandContext::CursorCapture {
    explicit CursorCapture(CaptureSession& owner) : owner(owner) {}
    ~CursorCapture() {
      pendingFrame.reset();
      imageCapture.reset();
      if (session != nullptr) {
        ext_image_copy_capture_cursor_session_v1_destroy(session);
      }
      if (buffer != nullptr) {
        wl_buffer_destroy(buffer);
      }
      if (mapping != nullptr && mapping != MAP_FAILED) {
        munmap(mapping, mapSize);
      }
      if (fd >= 0) {
        close(fd);
      }
    }

    CaptureSession& owner;
    ext_image_copy_capture_cursor_session_v1* session = nullptr;
    std::unique_ptr<CaptureSession> imageCapture;
    std::unique_ptr<CaptureFrame> pendingFrame;
    wl_buffer* buffer = nullptr;
    int fd = -1;
    void* mapping = nullptr;
    size_t mapSize = 0;
    uint32_t format = 0;
    uint32_t stride = 0;
    bool requestPending = false;
    uint32_t consecutiveFailures = 0;
    bool pendingDamage = true;
    CursorMetadata metadata;
    BufferPoint pendingHotspot;
  };

  WaylandContext::CaptureSession::CaptureSession(
      WaylandContext::Impl& impl, ext_image_capture_source_v1* source, ext_image_copy_capture_session_v1* session,
      ConstraintsCallback constraintsCb
  )
      : impl(impl), source(source), session(session), constraintsCb(std::move(constraintsCb)) {
    impl.captures.push_back(this);
  }

  WaylandContext::CaptureSession::~CaptureSession() {
    frameLifetime.reset();
    std::erase(impl.captures, this);
    cursor.reset();
    if (session != nullptr) {
      ext_image_copy_capture_session_v1_destroy(session);
    }
    if (source != nullptr) {
      ext_image_capture_source_v1_destroy(source);
    }
  }

  bool WaylandContext::CaptureSession::hasCursorMetadata() const { return cursor != nullptr; }

  const CursorMetadata* WaylandContext::CaptureSession::cursorMetadata() const {
    return cursor != nullptr ? &cursor->metadata : nullptr;
  }

  WaylandContext::CaptureFrame::~CaptureFrame() {
    if (frame != nullptr) {
      ext_image_copy_capture_frame_v1_destroy(frame);
    }
  }

  namespace {

    void onOutputGeometry(
        void* data, wl_output* output, int32_t x, int32_t y, int32_t physicalWidth, int32_t physicalHeight,
        int32_t subpixel, const char* make, const char* model, int32_t transform
    ) {
      (void)physicalWidth;
      (void)physicalHeight;
      (void)subpixel;
      (void)make;
      (void)model;
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto* state = impl->findOutput(output);
      if (state == nullptr) {
        return;
      }
      state->info.x = x;
      state->info.y = y;
      if (transform >= 0 && transform <= WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        state->pendingTransform = static_cast<uint32_t>(transform);
      }
    }

    void onOutputMode(void* data, wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
      (void)refresh;
      if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) {
        return;
      }
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto* state = impl->findOutput(output);
      if (state == nullptr) {
        return;
      }
      state->info.width = width;
      state->info.height = height;
    }

    void onOutputDone(void* data, wl_output* output) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto* state = impl->findOutput(output);
      if (state == nullptr) {
        return;
      }
      state->info.name = state->info.name.empty() ? "output-" + std::to_string(state->registryName) : state->info.name;
      impl->rebuildOutputs();
      if (state->transform == state->pendingTransform) {
        return;
      }
      state->transform = state->pendingTransform;
      const uint32_t transform = state->transform;
      // A constraints callback may destroy its capture or another capture.
      // Recheck registry membership before using each pointer from the snapshot.
      const auto captures = impl->captures;
      for (auto* capture : captures) {
        if (std::ranges::find(impl->captures, capture) == impl->captures.end() || capture->output != output) {
          continue;
        }
        ++capture->transformRevision;
        capture->pendingConstraints.transform = transform;
        if (capture->constraints.transform == transform) {
          continue;
        }
        capture->constraints.transform = transform;
        if (capture->constraints.bufferWidth != 0 && capture->constraints.bufferHeight != 0 && capture->constraintsCb) {
          const auto constraints = capture->constraints;
          const auto callback = capture->constraintsCb;
          callback(constraints);
        }
      }
    }

    void onOutputScale(void* data, wl_output* output, int32_t factor) {
      (void)data;
      (void)output;
      (void)factor;
    }

    void onOutputName(void* data, wl_output* output, const char* name) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto* state = impl->findOutput(output);
      if (state == nullptr) {
        return;
      }
      state->info.name = name == nullptr ? std::string{} : name;
      state->hasName = true;
    }

    void onOutputDescription(void* data, wl_output* output, const char* description) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto* state = impl->findOutput(output);
      if (state == nullptr) {
        return;
      }
      state->info.description = description == nullptr ? std::string{} : description;
    }

    constexpr wl_output_listener kOutputListener = {
        .geometry = onOutputGeometry,
        .mode = onOutputMode,
        .done = onOutputDone,
        .scale = onOutputScale,
        .name = onOutputName,
        .description = onOutputDescription,
    };

    // Layout geometry: wl_output.geometry carries no compositor position (wlroots
    // always sends 0,0) and the mode size ignores fractional scale, so logical
    // coordinates need xdg-output to map onto a capture.
    void onXdgOutputLogicalPosition(void* data, zxdg_output_v1* /*xdgOutput*/, int32_t x, int32_t y) {
      auto* state = static_cast<WaylandContext::Impl::OutputState*>(data);
      state->info.logicalX = x;
      state->info.logicalY = y;
    }

    void onXdgOutputLogicalSize(void* data, zxdg_output_v1* /*xdgOutput*/, int32_t width, int32_t height) {
      auto* state = static_cast<WaylandContext::Impl::OutputState*>(data);
      state->info.logicalWidth = width;
      state->info.logicalHeight = height;
    }

    // Versions before 3 apply xdg-output state here instead of on wl_output.done.
    void onXdgOutputDone(void* data, zxdg_output_v1* /*xdgOutput*/) {
      static_cast<WaylandContext::Impl::OutputState*>(data)->owner->rebuildOutputs();
    }

    void onXdgOutputName(void* /*data*/, zxdg_output_v1* /*xdgOutput*/, const char* /*name*/) {}

    void onXdgOutputDescription(void* /*data*/, zxdg_output_v1* /*xdgOutput*/, const char* /*description*/) {}

    constexpr zxdg_output_v1_listener kXdgOutputListener = {
        .logical_position = onXdgOutputLogicalPosition,
        .logical_size = onXdgOutputLogicalSize,
        .done = onXdgOutputDone,
        .name = onXdgOutputName,
        .description = onXdgOutputDescription,
    };

    void attachXdgOutput(WaylandContext::Impl& impl, WaylandContext::Impl::OutputState& state) {
      if (impl.xdgOutputManager == nullptr || state.xdgOutput != nullptr) {
        return;
      }
      state.xdgOutput = zxdg_output_manager_v1_get_xdg_output(impl.xdgOutputManager, state.output);
      zxdg_output_v1_add_listener(state.xdgOutput, &kXdgOutputListener, &state);
    }

    // ext_image_copy_capture_session_v1.shm_format carries a wl_shm.format value,
    // not a DRM fourcc. The two agree for every format except the two wl_shm
    // defines itself: ARGB8888 is 0 and XRGB8888 is 1, where the fourcc would be
    // 'AR24' and 'XR24'. Everything past the listener speaks DRM fourcc (the
    // SPA table, bytes-per-pixel, the dmabuf constraints), so the enum is
    // converted away here and converted back only where wl_shm itself is
    // spoken to, in createShmBuffer().
    uint32_t drmFormatFromWlShm(uint32_t wlShmFormat) {
      switch (wlShmFormat) {
      case WL_SHM_FORMAT_ARGB8888:
        return DRM_FORMAT_ARGB8888;
      case WL_SHM_FORMAT_XRGB8888:
        return DRM_FORMAT_XRGB8888;
      default:
        return wlShmFormat;
      }
    }

    uint32_t wlShmFormatFromDrm(uint32_t drmFormat) {
      switch (drmFormat) {
      case DRM_FORMAT_ARGB8888:
        return WL_SHM_FORMAT_ARGB8888;
      case DRM_FORMAT_XRGB8888:
        return WL_SHM_FORMAT_XRGB8888;
      default:
        return drmFormat;
      }
    }

    void onSessionBufferSize(void* data, ext_image_copy_capture_session_v1* session, uint32_t width, uint32_t height) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      capture->pendingConstraints.bufferWidth = width;
      capture->pendingConstraints.bufferHeight = height;
    }

    void onSessionShmFormat(void* data, ext_image_copy_capture_session_v1* session, uint32_t format) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      capture->pendingConstraints.shmFormats.push_back(drmFormatFromWlShm(format));
    }

    void onSessionDmabufDevice(void* data, ext_image_copy_capture_session_v1* session, wl_array* device) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      if (device != nullptr && device->size >= sizeof(dev_t)) {
        dev_t dev = 0;
        std::memcpy(&dev, device->data, sizeof(dev));
        capture->pendingConstraints.dmabufDevice = dev;
      }
    }

    void onSessionDmabufFormat(
        void* data, ext_image_copy_capture_session_v1* session, uint32_t format, wl_array* modifiers
    ) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      if (modifiers == nullptr || modifiers->size == 0) {
        capture->pendingConstraints.dmabufFormats.push_back({format, DRM_FORMAT_MOD_INVALID});
        return;
      }

      const auto* begin = static_cast<const uint64_t*>(modifiers->data);
      const auto* end = reinterpret_cast<const uint64_t*>(static_cast<const char*>(modifiers->data) + modifiers->size);
      for (const uint64_t* modifier = begin; modifier < end; ++modifier) {
        capture->pendingConstraints.dmabufFormats.push_back({format, *modifier});
      }
    }

    void onSessionDone(void* data, ext_image_copy_capture_session_v1* session) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      capture->constraints = std::move(capture->pendingConstraints);
      capture->pendingConstraints = {};
      capture->pendingConstraints.transform = capture->constraints.transform;
      if (capture->constraintsCb) {
        capture->constraintsCb(capture->constraints);
      }
    }

    void onSessionStopped(void* data, ext_image_copy_capture_session_v1* session) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      capture->stopped = true;
      auto callback = std::move(capture->stoppedCb);
      if (callback) {
        callback();
      }
    }

    constexpr ext_image_copy_capture_session_v1_listener kSessionListener = {
        .buffer_size = onSessionBufferSize,
        .shm_format = onSessionShmFormat,
        .dmabuf_device = onSessionDmabufDevice,
        .dmabuf_format = onSessionDmabufFormat,
        .done = onSessionDone,
        .stopped = onSessionStopped,
    };

    void onFrameTransform(void* data, ext_image_copy_capture_frame_v1* /*frame*/, uint32_t transform) {
      auto* state = static_cast<WaylandContext::CaptureFrame*>(data);
      state->buffer.transform = transform;
      // wl_output is an initial hint. The captured frame is authoritative,
      // except when a newer output update already invalidated this request.
      if (!state->ownerLifetime.expired() && state->owner->transformRevision == state->transformRevision) {
        state->owner->constraints.transform = transform;
        state->owner->pendingConstraints.transform = transform;
      }
    }

    void onFrameDamage(
        void* /*data*/, ext_image_copy_capture_frame_v1* /*frame*/, int32_t /*x*/, int32_t /*y*/, int32_t /*width*/,
        int32_t /*height*/
    ) {}

    void onFramePresentationTime(
        void* data, ext_image_copy_capture_frame_v1* /*frame*/, uint32_t tvSecHi, uint32_t tvSecLo, uint32_t tvNsec
    ) {
      auto* state = static_cast<WaylandContext::CaptureFrame*>(data);
      state->presentationSec = (static_cast<uint64_t>(tvSecHi) << 32U) | tvSecLo;
      state->presentationNsec = tvNsec;
    }

    void onFrameReady(void* data, ext_image_copy_capture_frame_v1* /*frame*/) {
      auto* state = static_cast<WaylandContext::CaptureFrame*>(data);
      // Grab everything we need before touching state, the callback may delete it.
      auto* proxy = state->frame;
      auto cb = std::move(state->onReady);
      auto buf = state->buffer;
      const uint64_t ptsSec = state->presentationSec;
      const uint32_t ptsNsec = state->presentationNsec;
      state->frame = nullptr;
      // Destroy the Wayland proxy (frees the protocol object).
      ext_image_copy_capture_frame_v1_destroy(proxy);
      if (cb) {
        cb(buf, ptsSec, ptsNsec);
      }
    }

    void onFrameFailed(void* data, ext_image_copy_capture_frame_v1* /*frame*/, uint32_t reason) {
      auto* state = static_cast<WaylandContext::CaptureFrame*>(data);
      auto* proxy = state->frame;
      auto cb = std::move(state->onFailed);
      CaptureFailureReason failureReason = CaptureFailureReason::Retry;
      if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
        failureReason = CaptureFailureReason::ConstraintsChanged;
      } else if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
        failureReason = CaptureFailureReason::Stopped;
      }
      state->frame = nullptr;
      ext_image_copy_capture_frame_v1_destroy(proxy);
      if (cb) {
        cb(failureReason);
      }
    }

    constexpr ext_image_copy_capture_frame_v1_listener kFrameListener = {
        .transform = onFrameTransform,
        .damage = onFrameDamage,
        .presentation_time = onFramePresentationTime,
        .ready = onFrameReady,
        .failed = onFrameFailed,
    };

    uint32_t preferredCursorFormat(const std::vector<uint32_t>& formats) {
      const auto has = [&](uint32_t format) { return std::ranges::find(formats, format) != formats.end(); };
      if (has(DRM_FORMAT_ARGB8888)) {
        return DRM_FORMAT_ARGB8888;
      }
      if (has(DRM_FORMAT_ABGR8888)) {
        return DRM_FORMAT_ABGR8888;
      }
      if (has(DRM_FORMAT_XRGB8888)) {
        return DRM_FORMAT_XRGB8888;
      }
      if (has(DRM_FORMAT_XBGR8888)) {
        return DRM_FORMAT_XBGR8888;
      }
      return 0;
    }

    void resetCursorBuffer(WaylandContext::CursorCapture& cursor) {
      cursor.pendingFrame.reset();
      if (cursor.buffer != nullptr) {
        wl_buffer_destroy(cursor.buffer);
        cursor.buffer = nullptr;
      }
      if (cursor.mapping != nullptr && cursor.mapping != MAP_FAILED) {
        munmap(cursor.mapping, cursor.mapSize);
        cursor.mapping = nullptr;
      }
      if (cursor.fd >= 0) {
        close(cursor.fd);
        cursor.fd = -1;
      }
      cursor.mapSize = 0;
      cursor.format = 0;
      cursor.stride = 0;
    }

    void requestCursorFrame(WaylandContext::CursorCapture& cursor, bool damage = true);

    bool configureCursorBuffer(WaylandContext::CursorCapture& cursor, const CaptureConstraints& constraints) {
      resetCursorBuffer(cursor);
      // wlroots cursor sources may initially have no buffer constraints (for
      // example, before the first hardware-cursor buffer exists).  A later
      // constraints update will call us again, so this is not a negotiation
      // failure and must not be treated as one.
      if (constraints.shmFormats.empty() || constraints.bufferWidth == 0 || constraints.bufferHeight == 0) {
        return false;
      }

      const uint32_t format = preferredCursorFormat(constraints.shmFormats);
      if (format == 0) {
        std::fprintf(stderr, "wayland: cursor capture has no supported 32-bit SHM format\n");
        return false;
      }

      const uint64_t stride = static_cast<uint64_t>(constraints.bufferWidth) * 4U;
      const uint64_t size = stride * constraints.bufferHeight;
      if (stride > INT32_MAX || size > INT32_MAX) {
        std::fprintf(stderr, "wayland: cursor capture buffer is too large\n");
        return false;
      }

      const int fd = memfd_create("andromeda-cursor", MFD_CLOEXEC | MFD_ALLOW_SEALING);
      if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
        if (fd >= 0) {
          close(fd);
        }
        std::fprintf(stderr, "wayland: unable to allocate cursor capture buffer: %s\n", std::strerror(errno));
        return false;
      }

      void* mapping = mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (mapping == MAP_FAILED) {
        close(fd);
        std::fprintf(stderr, "wayland: unable to map cursor capture buffer: %s\n", std::strerror(errno));
        return false;
      }

      wl_buffer* buffer = cursor.owner.impl.owner.createShmBuffer(
          constraints.bufferWidth, constraints.bufferHeight, format, static_cast<uint32_t>(stride), fd,
          static_cast<size_t>(size)
      );
      if (buffer == nullptr) {
        munmap(mapping, static_cast<size_t>(size));
        close(fd);
        return false;
      }

      cursor.fd = fd;
      cursor.mapping = mapping;
      cursor.mapSize = static_cast<size_t>(size);
      cursor.buffer = buffer;
      cursor.format = format;
      cursor.stride = static_cast<uint32_t>(stride);
      requestCursorFrame(cursor);
      return true;
    }

    void cursorFrameReady(WaylandContext::CursorCapture& cursor, const CaptureBuffer& buffer) {
      cursor.pendingFrame.reset();
      CursorMetadata& metadata = cursor.metadata;
      const auto dimensions = displaySize(buffer.width, buffer.height, buffer.transform);
      metadata.width = dimensions.width;
      metadata.height = dimensions.height;
      metadata.stride = dimensions.width * 4;
      metadata.format = cursor.format;
      metadata.pixels.resize(static_cast<size_t>(metadata.stride) * metadata.height);
      if (!transformPacked32(
              cursor.mapping, cursor.mapSize, buffer.width, buffer.height, cursor.stride, buffer.transform,
              metadata.pixels.data(), metadata.pixels.size(), metadata.stride
          )) {
        metadata.visible = false;
        return;
      }
      const auto hotspot = bufferToDisplay(
          cursor.pendingHotspot.x, cursor.pendingHotspot.y, static_cast<int32_t>(buffer.width),
          static_cast<int32_t>(buffer.height), buffer.transform
      );
      if (hotspot.x < INT32_MIN || hotspot.x > INT32_MAX || hotspot.y < INT32_MIN || hotspot.y > INT32_MAX) {
        metadata.visible = false;
        return;
      }
      metadata.hotspotX = static_cast<int32_t>(hotspot.x);
      metadata.hotspotY = static_cast<int32_t>(hotspot.y);

      // Keep one damage-less frame pending: fires when the cursor image
      // changes, without forcing output redraws like damaged frames do.
      cursor.consecutiveFailures = 0;
      requestCursorFrame(cursor, /*damage=*/false);
    }

    // Retry transient failures briefly, then wait for the next pointer event.
    constexpr uint32_t kMaxCursorRetries = 3;

    void cursorFrameFailed(WaylandContext::CursorCapture& cursor, CaptureFailureReason reason) {
      cursor.pendingFrame.reset();
      if (reason == CaptureFailureReason::Stopped) {
        cursor.metadata.visible = false;
        return;
      }
      if (reason == CaptureFailureReason::ConstraintsChanged) {
        configureCursorBuffer(cursor, cursor.imageCapture->constraints);
        return;
      }
      if (cursor.consecutiveFailures >= kMaxCursorRetries) {
        return;
      }
      ++cursor.consecutiveFailures;
      requestCursorFrame(cursor, cursor.pendingDamage);
    }

    void requestCursorFrame(WaylandContext::CursorCapture& cursor, bool damage) {
      if (cursor.imageCapture == nullptr || cursor.imageCapture->stopped || cursor.buffer == nullptr) {
        cursor.requestPending = true;
        return;
      }
      if (cursor.pendingFrame != nullptr) {
        cursor.requestPending = true;
        return;
      }

      cursor.requestPending = false;
      cursor.pendingDamage = damage;
      cursor.pendingFrame = cursor.owner.impl.owner.captureFrame(
          *cursor.imageCapture, cursor.buffer, damage,
          [&cursor](CaptureBuffer& buffer, uint64_t, uint32_t) { cursorFrameReady(cursor, buffer); },
          [&cursor](CaptureFailureReason reason) { cursorFrameFailed(cursor, reason); }
      );
    }

    void onCursorEnter(void* data, ext_image_copy_capture_cursor_session_v1*) {
      auto* cursor = static_cast<WaylandContext::CursorCapture*>(data);
      cursor->metadata.visible = true;
      requestCursorFrame(*cursor);
    }

    void onCursorLeave(void* data, ext_image_copy_capture_cursor_session_v1*) {
      auto* cursor = static_cast<WaylandContext::CursorCapture*>(data);
      cursor->metadata.visible = false;
    }

    void onCursorPosition(void* data, ext_image_copy_capture_cursor_session_v1*, int32_t x, int32_t y) {
      auto* cursor = static_cast<WaylandContext::CursorCapture*>(data);
      cursor->metadata.x = x;
      cursor->metadata.y = y;
      requestCursorFrame(*cursor);
    }

    void onCursorHotspot(void* data, ext_image_copy_capture_cursor_session_v1*, int32_t x, int32_t y) {
      auto* cursor = static_cast<WaylandContext::CursorCapture*>(data);
      cursor->pendingHotspot = {x, y};
      requestCursorFrame(*cursor);
    }

    constexpr ext_image_copy_capture_cursor_session_v1_listener kCursorSessionListener = {
        .enter = onCursorEnter,
        .leave = onCursorLeave,
        .position = onCursorPosition,
        .hotspot = onCursorHotspot,
    };

    bool setupCursorCapture(WaylandContext::CaptureSession& capture) {
      WaylandContext::Impl& impl = capture.impl;
      if (impl.captureManager == nullptr || impl.pointer == nullptr) {
        return false;
      }

      auto cursor = std::make_unique<WaylandContext::CursorCapture>(capture);
      cursor->session = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
          impl.captureManager, capture.source, impl.pointer
      );
      if (cursor->session == nullptr) {
        return false;
      }

      auto* imageSession = ext_image_copy_capture_cursor_session_v1_get_capture_session(cursor->session);
      if (imageSession == nullptr) {
        return false;
      }
      cursor->imageCapture =
          std::make_unique<WaylandContext::CaptureSession>(impl, nullptr, imageSession, ConstraintsCallback{});

      auto* state = cursor.get();
      state->imageCapture->constraintsCb = [state](const CaptureConstraints& constraints) {
        configureCursorBuffer(*state, constraints);
      };
      state->imageCapture->stoppedCb = [state]() { state->metadata.visible = false; };
      ext_image_copy_capture_cursor_session_v1_add_listener(state->session, &kCursorSessionListener, state);
      ext_image_copy_capture_session_v1_add_listener(imageSession, &kSessionListener, state->imageCapture.get());
      capture.cursor = std::move(cursor);
      return true;
    }

    void onToplevelClosed(void* data, ext_foreign_toplevel_handle_v1* handle) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const auto iter = impl->toplevelStates.find(handle);
      if (iter == impl->toplevelStates.end()) {
        return;
      }
      ext_foreign_toplevel_handle_v1_destroy(handle);
      impl->toplevelStates.erase(iter);
      impl->rebuildToplevels();
    }

    void onToplevelDone(void* data, ext_foreign_toplevel_handle_v1* handle) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const auto iter = impl->toplevelStates.find(handle);
      if (iter == impl->toplevelStates.end()) {
        return;
      }
      iter->second->info = iter->second->pending;
      impl->rebuildToplevels();
    }

    void onToplevelTitle(void* data, ext_foreign_toplevel_handle_v1* handle, const char* title) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const auto iter = impl->toplevelStates.find(handle);
      if (iter != impl->toplevelStates.end()) {
        iter->second->pending.title = title == nullptr ? std::string{} : title;
      }
    }

    void onToplevelAppId(void* data, ext_foreign_toplevel_handle_v1* handle, const char* appId) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const auto iter = impl->toplevelStates.find(handle);
      if (iter != impl->toplevelStates.end()) {
        iter->second->pending.appId = appId == nullptr ? std::string{} : appId;
      }
    }

    void onToplevelIdentifier(void* data, ext_foreign_toplevel_handle_v1* handle, const char* identifier) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const auto iter = impl->toplevelStates.find(handle);
      if (iter != impl->toplevelStates.end()) {
        iter->second->pending.identifier = identifier == nullptr ? std::string{} : identifier;
      }
    }

    constexpr ext_foreign_toplevel_handle_v1_listener kToplevelHandleListener = {
        .closed = onToplevelClosed,
        .done = onToplevelDone,
        .title = onToplevelTitle,
        .app_id = onToplevelAppId,
        .identifier = onToplevelIdentifier,
    };

    void
    onToplevelListToplevel(void* data, ext_foreign_toplevel_list_v1* list, ext_foreign_toplevel_handle_v1* handle) {
      (void)list;
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto state = std::make_unique<WaylandContext::Impl::ToplevelState>();
      state->handle = handle;
      const auto [iter, inserted] = impl->toplevelStates.emplace(handle, std::move(state));
      (void)iter;
      (void)inserted;
      ext_foreign_toplevel_handle_v1_add_listener(handle, &kToplevelHandleListener, impl);
    }

    void onToplevelListFinished(void* data, ext_foreign_toplevel_list_v1* list) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      if (impl->toplevelList == list) {
        ext_foreign_toplevel_list_v1_destroy(list);
        impl->toplevelList = nullptr;
      }
    }

    constexpr ext_foreign_toplevel_list_v1_listener kToplevelListListener = {
        .toplevel = onToplevelListToplevel,
        .finished = onToplevelListFinished,
    };

    void onSeatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const bool hasPointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
      if (hasPointer && impl->pointer == nullptr) {
        impl->pointer = wl_seat_get_pointer(seat);
      } else if (!hasPointer && impl->pointer != nullptr) {
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(impl->pointer)) >= WL_POINTER_RELEASE_SINCE_VERSION) {
          wl_pointer_release(impl->pointer);
        } else {
          wl_pointer_destroy(impl->pointer);
        }
        impl->pointer = nullptr;
      }
    }

    void onSeatName(void*, wl_seat*, const char*) {}

    constexpr wl_seat_listener kSeatListener = {
        .capabilities = onSeatCapabilities,
        .name = onSeatName,
    };

    void onRegistryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const std::string_view iface = interface == nullptr ? std::string_view{} : std::string_view(interface);
      if (iface == wl_seat_interface.name && impl->seat == nullptr) {
        impl->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 9U)));
        impl->seatRegistryName = name;
        wl_seat_add_listener(impl->seat, &kSeatListener, impl);
        return;
      }

      if (iface == wl_shm_interface.name) {
        impl->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, std::min(version, 1U)));
        return;
      }
      if (iface == zwp_linux_dmabuf_v1_interface.name) {
        impl->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, std::min(version, 4U))
        );
        return;
      }
      if (iface == wl_output_interface.name) {
        auto* output =
            static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4U)));
        auto state = std::make_unique<WaylandContext::Impl::OutputState>();
        state->output = output;
        state->owner = impl;
        state->registryName = name;
        wl_output_add_listener(output, &kOutputListener, impl);
        attachXdgOutput(*impl, *state);
        impl->outputStates.emplace(output, std::move(state));
        return;
      }
      if (iface == zxdg_output_manager_v1_interface.name && impl->xdgOutputManager == nullptr) {
        // Version 3 applies xdg-output state on wl_output.done, which already
        // rebuilds the output list; older versions rebuild on zxdg_output_v1.done.
        impl->xdgOutputManager = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, std::min(version, 3U))
        );
        for (auto& [output, state] : impl->outputStates) {
          (void)output;
          attachXdgOutput(*impl, *state);
        }
        return;
      }
      if (iface == ext_output_image_capture_source_manager_v1_interface.name) {
        impl->outputSourceManager = static_cast<ext_output_image_capture_source_manager_v1*>(wl_registry_bind(
            registry, name, &ext_output_image_capture_source_manager_v1_interface, std::min(version, 1U)
        ));
        return;
      }
      if (iface == ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) {
        impl->toplevelSourceManager =
            static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(wl_registry_bind(
                registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, std::min(version, 1U)
            ));
        return;
      }
      if (iface == ext_image_copy_capture_manager_v1_interface.name) {
        impl->captureManager = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, std::min(version, 1U))
        );
        return;
      }
      if (iface == andromeda_remote_input_manager_v1_interface.name) {
        impl->remoteInputManager = static_cast<andromeda_remote_input_manager_v1*>(
            wl_registry_bind(registry, name, &andromeda_remote_input_manager_v1_interface, std::min(version, 1U))
        );
        return;
      }
      if (iface == ext_data_control_manager_v1_interface.name && impl->dataControlManager == nullptr) {
        impl->dataControlManager = static_cast<ext_data_control_manager_v1*>(
            wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, std::min(version, 1U))
        );
        return;
      }
      if (iface == ext_foreign_toplevel_list_v1_interface.name) {
        impl->toplevelList = static_cast<ext_foreign_toplevel_list_v1*>(
            wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, std::min(version, 1U))
        );
        ext_foreign_toplevel_list_v1_add_listener(impl->toplevelList, &kToplevelListListener, impl);
      }
    }

    void onRegistryRemove(void* data, wl_registry* registry, uint32_t name) {
      (void)registry;
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      if (name == impl->seatRegistryName) {
        if (impl->pointer != nullptr) {
          if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(impl->pointer)) >= WL_POINTER_RELEASE_SINCE_VERSION) {
            wl_pointer_release(impl->pointer);
          } else {
            wl_pointer_destroy(impl->pointer);
          }
          impl->pointer = nullptr;
        }
        if (impl->seat != nullptr) {
          if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(impl->seat)) >= WL_SEAT_RELEASE_SINCE_VERSION) {
            wl_seat_release(impl->seat);
          } else {
            wl_seat_destroy(impl->seat);
          }
          impl->seat = nullptr;
        }
        impl->seatRegistryName = 0;
        return;
      }
      for (auto iter = impl->outputStates.begin(); iter != impl->outputStates.end(); ++iter) {
        if (iter->second->registryName == name) {
          if (iter->second->xdgOutput != nullptr) {
            zxdg_output_v1_destroy(iter->second->xdgOutput);
          }
          for (auto* capture : impl->captures) {
            if (capture->output == iter->first) {
              capture->output = nullptr;
            }
          }
          wl_output_destroy(iter->first);
          impl->outputStates.erase(iter);
          impl->rebuildOutputs();
          return;
        }
      }
    }

    constexpr wl_registry_listener kRegistryListener = {
        .global = onRegistryGlobal,
        .global_remove = onRegistryRemove,
    };

    std::unique_ptr<WaylandContext::CaptureSession> makeCaptureSession(
        WaylandContext::Impl& impl, ext_image_capture_source_v1* source, CaptureCursorMode cursorMode,
        ConstraintsCallback constraintsCb
    ) {
      if (impl.captureManager == nullptr || source == nullptr) {
        if (source != nullptr) {
          ext_image_capture_source_v1_destroy(source);
        }
        return nullptr;
      }

      const bool metadata = cursorMode == CaptureCursorMode::Metadata;
      const bool paintCursors = cursorMode == CaptureCursorMode::Embedded;
      const uint32_t options = paintCursors ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
      auto* session = ext_image_copy_capture_manager_v1_create_session(impl.captureManager, source, options);
      if (session == nullptr) {
        ext_image_capture_source_v1_destroy(source);
        return nullptr;
      }

      auto capture = std::make_unique<WaylandContext::CaptureSession>(impl, source, session, std::move(constraintsCb));
      ext_image_copy_capture_session_v1_add_listener(session, &kSessionListener, capture.get());
      if (metadata && !setupCursorCapture(*capture)) {
        // A metadata request must never change the contents of the video
        // frames.  If the seat has no pointer (or allocation failed), keep the
        // original cursor-free session instead of recreating it with
        // PAINT_CURSORS.  This degrades to a hidden cursor rather than
        // irreversibly baking one into every frame.
        std::fprintf(stderr, "wayland: cursor metadata capture is unavailable; hiding cursor\n");
      }
      impl.flushDisplay();
      return capture;
    }

  } // namespace

  WaylandContext::WaylandContext(PollLoop& loop) : WaylandContext() {
    if (m_impl->display == nullptr) {
      return;
    }
    if (m_impl->disconnected) {
      loop.quit();
      return;
    }
    m_impl->onDisconnected = [&loop] { loop.quit(); };
    m_impl->displayWatch =
        std::make_unique<FdWatch>(loop, wl_display_get_fd(m_impl->display), m_impl->displayEvents(),
                                  [impl = m_impl.get()](short revents) { impl->handleDisplayEvents(revents); });
  }

  WaylandContext::WaylandContext() : m_impl(std::make_unique<Impl>(*this, nullptr)) {
    m_impl->display = wl_display_connect(nullptr);
    if (m_impl->display == nullptr) {
      fprintf(stderr, "wayland: unable to connect to compositor\n");
      return;
    }

    m_impl->registry = wl_display_get_registry(m_impl->display);
    wl_registry_add_listener(m_impl->registry, &kRegistryListener, m_impl.get());
    // First roundtrip: discover and bind globals (wl_output, managers, etc.).
    roundtrip();
    // Second roundtrip: receive initial events from newly-bound globals
    // (wl_output.name/geometry/mode/done, toplevel handle events, etc.).
    roundtrip();

    m_impl->prepareRead();

    g_defaultContext = this;
  }

  WaylandContext::~WaylandContext() {
    if (g_defaultContext == this) {
      g_defaultContext = nullptr;
    }
    m_impl->displayWatch.reset();
    if (m_impl->display != nullptr) {
      m_impl->cancelPreparedRead();
      m_impl->destroyObjects();
      wl_display_disconnect(m_impl->display);
      m_impl->display = nullptr;
    }
  }

  wl_display* WaylandContext::display() const { return m_impl->display; }

  bool WaylandContext::connected() const { return m_impl->display != nullptr && !m_impl->disconnected; }

  const std::vector<OutputInfo>& WaylandContext::outputs() const { return m_impl->outputs; }

  const std::vector<ToplevelInfo>& WaylandContext::toplevels() const { return m_impl->toplevels; }

  void WaylandContext::setFocusedOutput(std::optional<std::string> name) { m_impl->focusedOutput = std::move(name); }

  void WaylandContext::setCaptureBlocked(std::set<std::string> identifiers) {
    m_impl->captureBlocked = std::move(identifiers);
  }

  std::string WaylandContext::buildChooserJson(uint32_t sourceTypes, bool multiple, const ChooserCaller& caller) const {
    nlohmann::json request;
    request["multiple"] = multiple;
    if (!caller.appId.empty()) {
      request["app_id"] = caller.appId;
    }
    if (!caller.parentWindow.empty()) {
      request["parent_window"] = caller.parentWindow;
    }
    if (!caller.cursorMode.empty()) {
      request["cursor_mode"] = caller.cursorMode;
    }
    request["can_remember"] = caller.canRemember;
    request["types"] = nlohmann::json::array();
    request["outputs"] = nlohmann::json::array();
    request["windows"] = nlohmann::json::array();

    if ((sourceTypes & kSourceMonitor) != 0) {
      request["types"].push_back("monitor");
      for (const OutputInfo& output : m_impl->outputs) {
        request["outputs"].push_back({
            {"name", output.name},
            {"title", output.name},
            {"description", output.description},
            {"width", output.width},
            {"height", output.height},
            {"focused", m_impl->focusedOutput == output.name},
        });
      }
    }

    if ((sourceTypes & kSourceWindow) != 0) {
      request["types"].push_back("window");
      for (const ToplevelInfo& toplevel : m_impl->toplevels) {
        request["windows"].push_back({
            {"identifier", toplevel.identifier},
            {"app_id", toplevel.appId},
            {"title", toplevel.title},
            {"capture_blocked", m_impl->captureBlocked.contains(toplevel.identifier)},
        });
      }
    }

    return request.dump();
  }

  std::unique_ptr<WaylandContext::CaptureSession> WaylandContext::createOutputCapture(
      const std::string& outputName, CaptureCursorMode cursorMode, ConstraintsCallback constraintsCb
  ) {
    if (m_impl->outputSourceManager == nullptr) {
      fprintf(stderr, "wayland: output capture source manager is unavailable\n");
      return nullptr;
    }
    Impl::OutputState* output = m_impl->findOutput(outputName);
    if (output == nullptr) {
      fprintf(stderr, "wayland: output '%s' is unavailable\n", outputName.c_str());
      return nullptr;
    }

    auto* source =
        ext_output_image_capture_source_manager_v1_create_source(m_impl->outputSourceManager, output->output);
    auto capture = makeCaptureSession(*m_impl, source, cursorMode, std::move(constraintsCb));
    if (capture != nullptr) {
      capture->output = output->output;
      capture->constraints.transform = output->transform;
      capture->pendingConstraints.transform = output->transform;
    }
    return capture;
  }

  std::unique_ptr<WaylandContext::CaptureSession> WaylandContext::createToplevelCapture(
      const std::string& identifier, CaptureCursorMode cursorMode, ConstraintsCallback constraintsCb
  ) {
    if (m_impl->toplevelSourceManager == nullptr) {
      fprintf(stderr, "wayland: toplevel capture source manager is unavailable\n");
      return nullptr;
    }
    Impl::ToplevelState* toplevel = m_impl->findToplevel(identifier);
    if (toplevel == nullptr) {
      fprintf(stderr, "wayland: toplevel '%s' is unavailable\n", identifier.c_str());
      return nullptr;
    }

    auto* source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
        m_impl->toplevelSourceManager, toplevel->handle
    );
    return makeCaptureSession(*m_impl, source, cursorMode, std::move(constraintsCb));
  }

  std::unique_ptr<WaylandContext::CaptureFrame> WaylandContext::captureFrame(
      CaptureSession& session, wl_buffer* buffer, bool damageBuffer, FrameReadyCallback onReady,
      FrameFailedCallback onFailed
  ) {
    if (session.session == nullptr || session.stopped) {
      if (onFailed) {
        onFailed(CaptureFailureReason::Stopped);
      }
      return nullptr;
    }
    if (buffer == nullptr) {
      if (onFailed) {
        onFailed(CaptureFailureReason::Retry);
      }
      return nullptr;
    }

    auto* frame = ext_image_copy_capture_session_v1_create_frame(session.session);
    if (frame == nullptr) {
      if (onFailed) {
        onFailed(CaptureFailureReason::Retry);
      }
      return nullptr;
    }

    auto state = std::make_unique<CaptureFrame>();
    state->frame = frame;
    state->owner = &session;
    state->ownerLifetime = session.frameLifetime;
    state->transformRevision = session.transformRevision;
    state->buffer.wlBuffer = buffer;
    state->buffer.width = session.constraints.bufferWidth;
    state->buffer.height = session.constraints.bufferHeight;
    state->buffer.transform = session.constraints.transform;
    state->onReady = std::move(onReady);
    state->onFailed = std::move(onFailed);

    ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, state.get());
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    if (damageBuffer) {
      ext_image_copy_capture_frame_v1_damage_buffer(
          frame, 0, 0, static_cast<int32_t>(session.constraints.bufferWidth),
          static_cast<int32_t>(session.constraints.bufferHeight)
      );
    }
    ext_image_copy_capture_frame_v1_capture(frame);
    m_impl->flushDisplay();
    return state;
  }

  void WaylandContext::requestCursorFrame(CaptureSession& session) {
    if (session.cursor != nullptr) {
      xdpu::requestCursorFrame(*session.cursor);
    }
  }

  wl_buffer* WaylandContext::createDmabufBuffer(
      uint32_t width, uint32_t height, uint32_t format, uint64_t modifier, const std::vector<DmabufPlane>& planes
  ) {
    if (m_impl->dmabuf == nullptr || planes.empty() || width == 0 || height == 0) {
      return nullptr;
    }

    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(m_impl->dmabuf);
    if (params == nullptr) {
      return nullptr;
    }
    for (size_t index = 0; index < planes.size(); ++index) {
      const DmabufPlane& plane = planes[index];
      if (plane.fd < 0) {
        zwp_linux_buffer_params_v1_destroy(params);
        return nullptr;
      }
      zwp_linux_buffer_params_v1_add(
          params, plane.fd, static_cast<uint32_t>(index), plane.offset, plane.stride,
          static_cast<uint32_t>(modifier >> 32U), static_cast<uint32_t>(modifier & 0xffffffffU)
      );
    }
    wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(
        params, static_cast<int32_t>(width), static_cast<int32_t>(height), format, 0
    );
    zwp_linux_buffer_params_v1_destroy(params);
    m_impl->flushDisplay();
    return buffer;
  }

  wl_buffer* WaylandContext::createShmBuffer(
      uint32_t width, uint32_t height, uint32_t format, uint32_t stride, int fd, size_t size
  ) {
    if (m_impl->shm == nullptr || fd < 0 || width == 0 || height == 0 || size == 0) {
      return nullptr;
    }

    wl_shm_pool* pool = wl_shm_create_pool(m_impl->shm, fd, static_cast<int32_t>(size));
    if (pool == nullptr) {
      return nullptr;
    }
    wl_buffer* buffer = wl_shm_pool_create_buffer(
        pool, 0, static_cast<int32_t>(width), static_cast<int32_t>(height), static_cast<int32_t>(stride),
        wlShmFormatFromDrm(format)
    );
    wl_shm_pool_destroy(pool);
    m_impl->flushDisplay();
    return buffer;
  }

  namespace {

    using RemoteInputSession = WaylandContext::RemoteInputSession;

    RemoteInputSession& remoteSession(void* data) { return *static_cast<RemoteInputSession*>(data); }

    const andromeda_remote_input_session_v1_listener kRemoteInputListener = {
        .grant =
            [](void* data, andromeda_remote_input_session_v1*, uint32_t, uint32_t persistMode, const char* token) {
              if (auto& handler = remoteSession(data).events.grant) {
                handler(persistMode, token == nullptr ? std::string() : std::string(token));
              }
            },
        .started =
            [](void* data, andromeda_remote_input_session_v1*, uint32_t, uint32_t status, uint32_t capabilities) {
              if (auto& handler = remoteSession(data).events.started) {
                handler(status, capabilities);
              }
            },
        .eis_fd =
            [](void* data, andromeda_remote_input_session_v1*, uint32_t request, int32_t fd) {
              if (auto& handler = remoteSession(data).events.eisFd) {
                handler(request, fd);
              } else {
                close(fd);
              }
            },
        .result =
            [](void* data, andromeda_remote_input_session_v1*, uint32_t request, uint32_t status) {
              if (auto& handler = remoteSession(data).events.result) {
                handler(request, status);
              }
            },
        .closed =
            [](void* data, andromeda_remote_input_session_v1*) {
              if (auto& handler = remoteSession(data).events.closed) {
                handler();
              }
            },
        .paused =
            [](void* data, andromeda_remote_input_session_v1*, uint32_t paused) {
              if (auto& handler = remoteSession(data).events.paused) {
                handler(paused != 0);
              }
            },
    };

  } // namespace

  WaylandContext::RemoteInputSession::RemoteInputSession(
      WaylandContext& context, andromeda_remote_input_session_v1* proxy, Events events
  )
      : context(context), proxy(proxy), events(std::move(events)) {
    andromeda_remote_input_session_v1_add_listener(proxy, &kRemoteInputListener, this);
  }

  WaylandContext::RemoteInputSession::~RemoteInputSession() {
    if (context.connected()) {
      andromeda_remote_input_session_v1_destroy(proxy);
      context.flush();
    }
  }

  void WaylandContext::RemoteInputSession::setPersistence(
      uint32_t persistMode, const std::string& restoreToken, const std::string& instance
  ) {
    andromeda_remote_input_session_v1_set_persistence(proxy, persistMode, restoreToken.c_str(), instance.c_str());
    context.flush();
  }

  void WaylandContext::RemoteInputSession::start(const std::string& application, uint32_t capabilities) {
    andromeda_remote_input_session_v1_start(proxy, nextRequest++, application.c_str(), capabilities);
    context.flush();
  }

  uint32_t WaylandContext::RemoteInputSession::connectEis() {
    const uint32_t request = nextRequest++;
    andromeda_remote_input_session_v1_connect_eis(proxy, request);
    context.flush();
    return request;
  }

  void WaylandContext::expireRemoteInputScope(const std::string& instance) {
    if (connected() && m_impl->remoteInputManager != nullptr) {
      andromeda_remote_input_manager_v1_expire_scope(m_impl->remoteInputManager, instance.c_str());
      flush();
    }
  }

  std::unique_ptr<WaylandContext::RemoteInputSession>
  WaylandContext::createRemoteInputSession(RemoteInputSession::Events events) {
    if (!connected() || m_impl->remoteInputManager == nullptr) {
      return nullptr;
    }
    return std::make_unique<RemoteInputSession>(
        *this, andromeda_remote_input_manager_v1_create_session(m_impl->remoteInputManager), std::move(events)
    );
  }

  namespace {

    // Bounds what another client's offer may make the portal store and forward.
    constexpr size_t kMaxMimeTypes = 256;
    constexpr size_t kMaxMimeTypeLength = 4095;

  } // namespace

  struct WaylandContext::Clipboard::State {
    WaylandContext* context = nullptr;
    ext_data_control_manager_v1* manager = nullptr;
    ext_data_control_device_v1* device = nullptr;
    Events events;
    // Offers announced by data_offer and not yet named selection or primary selection.
    std::unordered_map<ext_data_control_offer_v1*, std::vector<std::string>> offers;
    ext_data_control_offer_v1* current = nullptr;
    std::vector<std::string> mimeTypes;
    std::unordered_map<ext_data_control_source_v1*, uint64_t> sources;
    uint64_t nextSource = 1;
    // The own source last set and not yet cancelled or withdrawn; it holds the selection once its sync is done.
    uint64_t lastSet = 0;
    uint64_t owner = 0;
    std::vector<wl_callback*> syncs;
    bool finished = false;

    ~State() {
      for (wl_callback* sync : syncs) {
        wl_callback_destroy(sync);
      }
      for (const auto& [offer, types] : offers) {
        (void)types;
        ext_data_control_offer_v1_destroy(offer);
      }
      if (current != nullptr) {
        ext_data_control_offer_v1_destroy(current);
      }
      for (const auto& [source, id] : sources) {
        (void)id;
        ext_data_control_source_v1_destroy(source);
      }
      if (device != nullptr) {
        ext_data_control_device_v1_destroy(device);
      }
    }

    void announce() {
      if (syncs.empty() && events.selection) {
        events.selection(mimeTypes, owner);
      }
    }

    // The selection event that follows a cancel or withdraw announces the change.
    void forget(uint64_t id) {
      if (lastSet == id) {
        lastSet = 0;
      }
      if (owner == id) {
        owner = 0;
      }
    }
  };

  namespace {

    using ClipboardState = WaylandContext::Clipboard::State;

    const ext_data_control_offer_v1_listener kOfferListener = {
        .offer = [](void* data, ext_data_control_offer_v1* offer, const char* mimeType) {
          auto& state = *static_cast<ClipboardState*>(data);
          const auto found = state.offers.find(offer);
          if (found == state.offers.end() || mimeType == nullptr || found->second.size() >= kMaxMimeTypes) {
            return;
          }
          const std::string_view type(mimeType);
          if (!type.empty() && type.size() <= kMaxMimeTypeLength) {
            found->second.emplace_back(type);
          }
        },
    };

    const ext_data_control_device_v1_listener kDeviceListener = {
        .data_offer =
            [](void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
              auto& state = *static_cast<ClipboardState*>(data);
              state.offers.emplace(offer, std::vector<std::string>{});
              ext_data_control_offer_v1_add_listener(offer, &kOfferListener, &state);
            },
        .selection =
            [](void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
              auto& state = *static_cast<ClipboardState*>(data);
              if (state.current != nullptr) {
                ext_data_control_offer_v1_destroy(state.current);
              }
              state.current = offer;
              state.mimeTypes.clear();
              if (const auto found = state.offers.find(offer); found != state.offers.end()) {
                state.mimeTypes = std::move(found->second);
                state.offers.erase(found);
              }
              // Without a pending set, the change came from another client.
              if (state.syncs.empty()) {
                state.owner = 0;
              }
              state.announce();
            },
        .finished =
            [](void* data, ext_data_control_device_v1* device) {
              auto& state = *static_cast<ClipboardState*>(data);
              ext_data_control_device_v1_destroy(device);
              state.device = nullptr;
              state.finished = true;
              state.mimeTypes.clear();
              state.owner = 0;
              state.announce();
              if (state.events.finished) {
                state.events.finished();
              }
            },
        .primary_selection =
            [](void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
              auto& state = *static_cast<ClipboardState*>(data);
              if (offer != nullptr) {
                state.offers.erase(offer);
                ext_data_control_offer_v1_destroy(offer);
              }
            },
    };

    ClipboardState& sourceState(void* data) { return *static_cast<ClipboardState*>(data); }

    const ext_data_control_source_v1_listener kSourceListener = {
        .send =
            [](void* data, ext_data_control_source_v1* source, const char* mimeType, int32_t fd) {
              auto& state = sourceState(data);
              const auto found = state.sources.find(source);
              if (found == state.sources.end() || !state.events.send || mimeType == nullptr) {
                close(fd);
                return;
              }
              state.events.send(found->second, mimeType, fd);
            },
        .cancelled =
            [](void* data, ext_data_control_source_v1* source) {
              auto& state = sourceState(data);
              const auto found = state.sources.find(source);
              if (found == state.sources.end()) {
                return;
              }
              const uint64_t id = found->second;
              state.sources.erase(found);
              ext_data_control_source_v1_destroy(source);
              if (state.events.cancelled) {
                state.events.cancelled(id);
              }
              state.forget(id);
            },
    };

    const wl_callback_listener kSetDoneListener = {
        .done = [](void* data, wl_callback* callback, uint32_t) {
          auto& state = sourceState(data);
          std::erase(state.syncs, callback);
          wl_callback_destroy(callback);
          if (state.syncs.empty()) {
            // The compositor applied every set: the last live one holds the selection unless another client
            // replaced it, which cancelled that source.
            state.owner = state.lastSet;
            state.announce();
          }
        },
    };

  } // namespace

  WaylandContext::Clipboard::Clipboard(WaylandContext& context, std::unique_ptr<State> state)
      : m_context(context), m_state(std::move(state)) {
    ext_data_control_device_v1_add_listener(m_state->device, &kDeviceListener, m_state.get());
  }

  WaylandContext::Clipboard::~Clipboard() {
    if (m_context.connected()) {
      m_state.reset();
      m_context.flush();
    } else {
      // The proxies died with the display.
      m_state->syncs.clear();
      m_state->offers.clear();
      m_state->current = nullptr;
      m_state->sources.clear();
      m_state->device = nullptr;
    }
  }

  uint64_t WaylandContext::Clipboard::offer(const std::vector<std::string>& mimeTypes) {
    State& state = *m_state;
    if (state.finished || !m_context.connected()) {
      return 0;
    }
    ext_data_control_source_v1* source = ext_data_control_manager_v1_create_data_source(state.manager);
    ext_data_control_source_v1_add_listener(source, &kSourceListener, &state);
    for (const std::string& type : mimeTypes) {
      ext_data_control_source_v1_offer(source, type.c_str());
    }
    const uint64_t id = state.nextSource++;
    state.sources.emplace(source, id);
    state.lastSet = id;
    ext_data_control_device_v1_set_selection(state.device, source);
    wl_callback* sync = wl_display_sync(m_context.m_impl->display);
    wl_callback_add_listener(sync, &kSetDoneListener, &state);
    state.syncs.push_back(sync);
    m_context.flush();
    return id;
  }

  void WaylandContext::Clipboard::withdraw(uint64_t source) {
    State& state = *m_state;
    const auto found =
        std::ranges::find_if(state.sources, [source](const auto& entry) { return entry.second == source; });
    if (found == state.sources.end()) {
      return;
    }
    ext_data_control_source_v1_destroy(found->first);
    state.sources.erase(found);
    m_context.flush();
    state.forget(source);
  }

  int WaylandContext::Clipboard::receive(const std::string& mimeType) {
    State& state = *m_state;
    if (state.current == nullptr || !m_context.connected() || !std::ranges::contains(state.mimeTypes, mimeType)) {
      return -1;
    }
    int ends[2];
    if (pipe2(ends, O_CLOEXEC) != 0) {
      return -1;
    }
    ext_data_control_offer_v1_receive(state.current, mimeType.c_str(), ends[1]);
    m_context.flush();
    close(ends[1]);
    return ends[0];
  }

  const std::vector<std::string>& WaylandContext::Clipboard::mimeTypes() const { return m_state->mimeTypes; }

  uint64_t WaylandContext::Clipboard::owner() const { return m_state->syncs.empty() ? m_state->owner : 0; }

  bool WaylandContext::Clipboard::ended() const { return m_state->finished; }

  std::unique_ptr<WaylandContext::Clipboard> WaylandContext::createClipboard(Clipboard::Events events) {
    if (!connected() || m_impl->dataControlManager == nullptr || m_impl->seat == nullptr) {
      return nullptr;
    }
    auto state = std::make_unique<Clipboard::State>();
    state->context = this;
    state->manager = m_impl->dataControlManager;
    state->device = ext_data_control_manager_v1_get_data_device(m_impl->dataControlManager, m_impl->seat);
    state->events = std::move(events);
    auto clipboard = std::make_unique<Clipboard>(*this, std::move(state));
    flush();
    return clipboard;
  }

  void WaylandContext::flush() {
    if (connected()) {
      m_impl->flushDisplay();
    }
  }

  void WaylandContext::roundtrip() {
    if (m_impl->display == nullptr || m_impl->disconnected) {
      return;
    }
    m_impl->cancelPreparedRead();
    if (wl_display_roundtrip(m_impl->display) < 0) {
      m_impl->markDisconnected();
      return;
    }
    m_impl->rebuildOutputs();
    m_impl->rebuildToplevels();
    m_impl->prepareRead();
  }

  WaylandContext* WaylandContext::defaultContext() { return g_defaultContext; }

  uint32_t WaylandContext::preferredShmFormat(const std::vector<uint32_t>& formats) {
    // Constraints hold DRM fourcc values; the session listener has already
    // converted the wl_shm.format the protocol event carries.
    const auto has = [&](uint32_t fmt) { return std::ranges::find(formats, fmt) != formats.end(); };
    if (has(DRM_FORMAT_XRGB8888)) {
      return DRM_FORMAT_XRGB8888;
    }
    if (has(DRM_FORMAT_ARGB8888)) {
      return DRM_FORMAT_ARGB8888;
    }
    if (has(DRM_FORMAT_XBGR8888)) {
      return DRM_FORMAT_XBGR8888;
    }
    if (has(DRM_FORMAT_ABGR8888)) {
      return DRM_FORMAT_ABGR8888;
    }
    return formats.empty() ? 0 : formats.front();
  }

} // namespace xdpu
