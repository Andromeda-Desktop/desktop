#pragma once

#include "input/input_capture_permissions.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

struct wlr_keyboard;

namespace andromeda {

  class Server;

  // The compositor owns portal input permission: capture sessions (local input to an EI receiver) and remote
  // sessions (RemoteDesktop: an EI sender through EisInput). The private Wayland bridges expose only
  // permission-scoped sessions and EI FDs.
  class InputCapture {
  public:
    // (session id, application label, persist mode, capabilities, kind): the user answers through authorizeSession.
    using AuthorizationRequest =
        std::function<void(uint64_t, std::string_view, uint32_t, uint32_t, InputPermissionKind)>;
    using AuthorizationCancelled = std::function<void(uint64_t)>;
    using ActiveChanged = std::function<void(bool)>;

    explicit InputCapture(Server& server);
    ~InputCapture();
    InputCapture(const InputCapture&) = delete;
    InputCapture& operator=(const InputCapture&) = delete;

    void setAuthorize(AuthorizationRequest request);
    void setAuthorizationCancelled(AuthorizationCancelled cancelled);
    void setActiveChanged(ActiveChanged changed);
    void authorizeSession(uint64_t id, bool allowed, bool remember = false);
    [[nodiscard]] std::vector<InputCapturePermission> permissions();
    [[nodiscard]] InputCapturePermissions& permissionStore();
    [[nodiscard]] bool revokePermission(std::string_view id);
    [[nodiscard]] bool active() const;
    [[nodiscard]] uint32_t capabilities() const;
    void addKeyboard(wlr_keyboard* keyboard);
    void removeKeyboard(wlr_keyboard* keyboard);
    void forgetAbsoluteDevice(const void* device);
    void updateKeymap(wlr_keyboard* keyboard);
    void updateOutputs();
    // Compositor escape, locking, and session deactivation stop capture and
    // disarm barriers. A client must explicitly enable its session again.
    void suspend();
    // Tells started remote input sessions when emulated input becomes blocked or unblocked. Call after any input of
    // the block (stopping, session lock, lock state, session activity) changes.
    void refreshBlocked();

    // True consumes the event before any local client or compositor binding.
    // canActivate prevents a barrier from taking an existing interactive grab.
    bool captureMotion(uint32_t timeMsec, double x, double y, double dx, double dy, bool canActivate = true);
    bool captureAbsoluteMotion(
        uint32_t timeMsec, double x, double y, double targetX, double targetY, bool canActivate = true,
        const void* device = nullptr
    );
    bool captureKey(uint32_t timeMsec, wlr_keyboard* keyboard, uint32_t keycode, bool pressed);
    bool captureModifiers(wlr_keyboard* keyboard);
    bool captureButton(uint32_t timeMsec, uint32_t button, bool pressed);
    bool captureAxis(uint32_t timeMsec, uint32_t orientation, double delta, int32_t discrete);
    bool captureFrame(uint32_t timeMsec);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace andromeda
