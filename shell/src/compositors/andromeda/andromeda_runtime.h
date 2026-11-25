#pragma once

#include "protocols/backoff.h"
#include <chrono>
#include <nlohmann/json.hpp>
#include <expected>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::andromeda {

  class AndromedaEventHandler;

  // Owns the Andromeda IPC: stateless one-shot requests (fresh socket per call)
  // plus a single persistent event-stream socket whose messages are fanned out
  // to every registered AndromedaEventHandler. Backends are handlers and forward
  // their poll hooks here.
  class AndromedaRuntime {
  public:
    AndromedaRuntime() = default;
    ~AndromedaRuntime();

    AndromedaRuntime(const AndromedaRuntime&) = delete;
    AndromedaRuntime& operator=(const AndromedaRuntime&) = delete;

    [[nodiscard]] bool available() const;
    [[nodiscard]] const std::string& socketPath() const;
    [[nodiscard]] std::optional<nlohmann::json>
    requestCommand(std::string_view cmd, std::optional<std::string_view> arg = std::nullopt) const;
    // The reply's "ok" value, else its "err" text; an empty error when the compositor gave no reply.
    [[nodiscard]] std::expected<nlohmann::json, std::string>
    commandResult(std::string_view cmd, std::optional<std::string_view> arg = std::nullopt) const;
    [[nodiscard]] bool requestAction(std::string_view action) const;
    void refresh();
    void cleanup();

    // Event-stream dispatch. Handlers register on construction; the runtime owns
    // the socket and delivers each parsed event to every registered handler.
    void registerEventHandler(AndromedaEventHandler* handler);
    void unregisterEventHandler(AndromedaEventHandler* handler);
    [[nodiscard]] int pollFd() const noexcept { return m_eventSocketFd; }
    [[nodiscard]] short pollEvents() const noexcept { return POLLIN | POLLHUP | POLLERR; }
    [[nodiscard]] int pollTimeoutMs() const noexcept;
    void dispatchPoll(short revents);

  private:
    struct IpcReply;

    [[nodiscard]] IpcReply request(std::string_view payload) const;
    [[nodiscard]] std::optional<nlohmann::json> commandReply(std::string_view cmd, std::optional<std::string_view> arg) const;
    void ensureResolved() const;
    void resolveSocketPath() const;

    [[nodiscard]] std::vector<std::string_view> newFamilies(const AndromedaEventHandler* added) const;
    [[nodiscard]] static std::string subscribeRequests(const std::vector<std::string_view>& families);
    void connectIfNeeded();
    void closeSocket(bool scheduleReconnect);
    void scheduleReconnect();
    void readSocket();
    void parseMessages();
    [[nodiscard]] bool handleMessage(std::string_view line);
    void dispatchEvent(std::string_view event, const nlohmann::json& data) const;
    void notifyStreamReset() const;

    mutable bool m_resolved = false;
    mutable std::string m_socketPath;
    std::vector<AndromedaEventHandler*> m_eventHandlers;
    int m_eventSocketFd = -1;
    std::vector<char> m_readBuffer;
    std::chrono::steady_clock::time_point m_nextReconnectAt;
    retry::Backoff m_reconnectBackoff{std::chrono::seconds(2), std::chrono::seconds(30)};
    bool m_slideStreamUnsupportedLogged = false;
  };

} // namespace compositors::andromeda
