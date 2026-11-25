#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <sys/types.h>

struct wlr_input_device;
struct wlr_keyboard;
struct wlr_keyboard_modifiers;

namespace andromeda {

  class Server;

  class EisInput {
  public:
    using PeerAuthentication = std::function<bool(pid_t)>;
    using AuthorizationRequest = std::function<void(uint64_t, std::string_view)>;
    using AuthorizationCancelled = std::function<void(uint64_t)>;

    explicit EisInput(Server& server);
    ~EisInput();
    EisInput(const EisInput&) = delete;
    EisInput& operator=(const EisInput&) = delete;

    [[nodiscard]] int socketFd() const;
    [[nodiscard]] bool ownsDevice(const wlr_input_device* device) const;
    void setAuthenticate(PeerAuthentication authenticate);
    void setAuthorize(AuthorizationRequest authorize);
    void setAuthorizationCancelled(AuthorizationCancelled cancelled);
    void authorizeClient(uint64_t id, bool allowed);
    // An EIS connection for a session the user already granted: no authorize step, a seat limited to
    // `capabilities` (portal types: bit 0 keyboard, bit 1 pointer). Returns an owned fd, or -1 while locked.
    [[nodiscard]] int openGrantedClient(uint64_t grant, uint32_t capabilities);
    // Disconnects every client opened for `grant`.
    void closeGrantedClients(uint64_t grant);
    void applyConfig();
    void updateOutputs();
    void updateModifiers(const wlr_keyboard_modifiers& modifiers);
    void setSessionLocked(bool locked);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    void addKeyboard(wlr_input_device* device);
    void addPointer(wlr_input_device* device);
    [[nodiscard]] wlr_keyboard* sourceKeyboard() const;
  };

} // namespace andromeda
