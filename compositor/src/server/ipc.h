#pragma once

#include "protocols/ipc_channel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct wl_event_source;

namespace andromeda {

  class Output;
  class Server;
  class WorkspaceGroup;

  class Ipc {
  public:
    // One bit per subscribable event name; the subscribe request ORs the bits it asked for into the connection. Keep
    // the names below in sync with the event builders in ipc.cpp.
    enum : uint16_t {
      kEventTheme = 1 << 0,
      kEventOverview = 1 << 1,
      kEventKeyboardLayout = 1 << 2,
      kEventWindows = 1 << 3,
      kEventWorkspaces = 1 << 4,
      kEventSubmap = 1 << 5,
      kEventScreenCast = 1 << 6,
      kEventLayers = 1 << 7,
      kEventOutputs = 1 << 8,
      kEventWorkspaceSlide = 1 << 9,
    };
    // Number of bits above; sizes the last-broadcast cache.
    static constexpr size_t kEventCount = 10;
    // Subscribable names in bit order: the subscribe handler matches against this table and `andromeda subscribe`
    // lists it, so a new family cannot be accepted by one and unknown to the other.
    static constexpr std::array<std::string_view, kEventCount> kEventNames = {
        "theme",  "overview",   "keyboard_layout", "windows", "workspaces",
        "submap", "screencast", "layers",          "outputs", "workspace_slide"
    };

    Ipc(Server& server, const std::string& waylandSocketName);
    ~Ipc();

    // Listens again at the socket path after its file was deleted; false when the path is taken or binding fails.
    bool rebind();

    Ipc(const Ipc&) = delete;
    Ipc& operator=(const Ipc&) = delete;

    void notifyThemeChanged();
    void notifyOverviewChanged();
    void notifyKeyboardLayoutChanged();
    void notifyWindowsChanged();
    void notifyWorkspacesChanged();
    void notifySubmapChanged();
    void notifyScreenCastChanged();
    void notifyLayersChanged();
    void notifyOutputsChanged();
    // One line per output frame while `group` slides, then one idle line when the slide ends. `nowMsec` is the
    // animation clock of the frame.
    void notifyWorkspaceSlide(const WorkspaceGroup& group, double nowMsec);
    // End of an output frame: answers frame waits (settle, clock-advance) once every output has drawn a frame.
    void notifyOutputFrame(const Output& output);
    // A hidden window's commit draws no frame, yet it can answer the last configure a settle waits for.
    void notifyViewCommitted();
    // A settle input cleared without a commit or a frame (the effect-selection idle, a window entering a settled
    // configure state such as suspended): recheck once the loop is idle,
    // so a waiter is not answered in the middle of the change that cleared it.
    void notifySettleInputCleared();
    // Configured output work ended on an output: answers the replies held until no output has any left.
    void notifyConfiguredWork();

  private:
    // Configured: the reply waits until no output has configured work left (its request started some). Job: the
    // reply waits on a deferred handler (a state save).
    enum class FrameWait : uint8_t { None, Drawn, Settled, Configured, Job };

    static constexpr size_t kMaxRequestSize = 65536;

    struct Connection {
      Ipc* owner = nullptr;
      int fd = -1;
      pid_t pid = 0;
      std::optional<ipc::ClientSlots::Slot> slot;
      ipc::LineReader input{kMaxRequestSize};
      std::string output;
      size_t writeOffset = 0;
      wl_event_source* fdSource = nullptr;
      wl_event_source* deadline = nullptr;
      bool responding = false;
      bool screenCastActive = false;
      uint16_t subscribedEvents = 0;
      // A frame wait holds its reply until these outputs have each drawn a frame, and for settle until the server is
      // settled as well.
      FrameWait frameWait = FrameWait::None;
      std::vector<std::string> waitOutputs;
      std::string waitReply;
      // Identifies the Job wait a deferred reply belongs to; a reply for an older one is dropped.
      uint64_t jobSeq = 0;
    };

    static int onListenReadable(int fd, uint32_t mask, void* data);
    static int onConnectionEvent(int fd, uint32_t mask, void* data);
    static int onConnectionTimeout(void* data);
    static void onSettleRecheck(void* data);

    void acceptConnections();
    void addConnection(int clientFd, pid_t pid, ipc::ClientSlots::Slot slot);
    bool readRequest(Connection& connection);
    bool writeResponse(Connection& connection);
    void prepareResponse(Connection& connection, std::string response);
    void removeConnection(Connection* connection);
    static void closeConnection(Connection& connection);
    // Empty when the reply is deferred, as for settle.
    std::optional<std::string> handleRequest(Connection& connection, std::string_view line);
    std::optional<std::string>
    dispatchRequest(Connection& connection, const nlohmann::json& request, size_t requestSize);
    static int waitTimeoutMs(FrameWait wait);
    void beginFrameWait(Connection& connection, FrameWait wait, std::string reply);
    void finishFrameWait(Connection& connection, std::string response);
    void answerRequest(Connection& connection, const std::string& line);
    void broadcastEvent(uint16_t event, const nlohmann::json& payload);
    // False clears the family's last payload: a later subscriber's initial line is not what `last` holds.
    bool hasSubscriber(uint16_t event);
    void refreshScreenCastActive();
    // Settled also needs the render thread idle: a capture copy in flight sends its ready after the frame. False
    // starts one kick that rechecks the waiters.
    bool renderSettled();
    static void onRenderSettleKick(void* data, bool ok);
    void dropAudioOwner();

    Server* m_server;
    std::string m_socketPath;
    Connection* m_audio = nullptr;
    int m_listenFd = -1;
    wl_event_source* m_eventSource = nullptr;
    wl_event_source* m_settleRecheck = nullptr;
    bool m_renderSettleKick = false;
    // Subscribers (shell, portal) stay connected; one-shot CLI requests close after their reply.
    ipc::ClientSlots m_slots{64};
    std::vector<std::unique_ptr<Connection>> m_connections;
    uint64_t m_nextJob = 0;
    // Deferred replies hold a weak reference: one that arrives after the Ipc is gone is dropped.
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
    // Last payload broadcast per family, indexed by the bit position of the event. A family that recomputes to the
    // same JSON is dropped rather than woken through to every subscriber; a fresh subscriber gets its own initial
    // line from the subscribe handler, so skipping a no-op broadcast can never leave anyone behind.
    std::array<std::string, kEventCount> m_lastBroadcast;
  };

} // namespace andromeda
