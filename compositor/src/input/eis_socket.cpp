#include "input/eis_socket.h"

#include "core/log.h"
#include "protocols/ipc_channel.h"
#include "protocols/xdg_dirs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("eis");
    constexpr size_t kMaxConnections = 64;
    constexpr size_t kMaxQueuedBytes = 64 * 1024;
    constexpr size_t kMaxQueuedFds = 64;
    constexpr size_t kMaxQueuedPackets = 128;
    constexpr size_t kReadSize = 8192;
    constexpr unsigned kDispatchBudget = 32;

    struct Packet {
      std::vector<char> bytes;
      std::vector<ipc::Fd> fds;
      size_t offset = 0;
    };

    struct Channel {
      std::deque<std::unique_ptr<Packet>> packets;
      size_t queuedBytes = 0;
      size_t queuedFds = 0;
      bool eof = false;
      bool shutdown = false;

      [[nodiscard]] bool canRead() const {
        return !eof && queuedBytes < kMaxQueuedBytes && queuedFds < kMaxQueuedFds && packets.size() < kMaxQueuedPackets;
      }
    };
  } // namespace

  struct EisSocket::Impl {
    struct Relay {
      struct Endpoint {
        Relay* relay = nullptr;
        size_t index = 0;
        int fd = -1;
        wl_event_source* source = nullptr;
        uint32_t mask = 0;
      };

      Impl& owner;
      std::array<Endpoint, 2> endpoints;
      std::array<Channel, 2> channels;

      Relay(Impl& owner, int peer, int backend) : owner(owner) {
        endpoints[0] = {.relay = this, .index = 0, .fd = peer};
        endpoints[1] = {.relay = this, .index = 1, .fd = backend};
      }

      ~Relay() {
        for (auto& endpoint : endpoints) {
          if (endpoint.source != nullptr) {
            wl_event_source_remove(endpoint.source);
          }
          close(endpoint.fd);
        }
      }

      static int onReady(int, uint32_t mask, void* data) {
        auto& endpoint = *static_cast<Endpoint*>(data);
        Relay& relay = *endpoint.relay;
        if (!relay.dispatch(endpoint.index, mask)) {
          relay.owner.remove(&relay);
        }
        return 0;
      }

      bool watch() {
        for (size_t i = 0; i < endpoints.size(); ++i) {
          auto& endpoint = endpoints[i];
          const uint32_t mask = (channels[i].canRead() ? uint32_t{WL_EVENT_READABLE} : 0U)
              | (!channels[1 - i].packets.empty() ? uint32_t{WL_EVENT_WRITABLE} : 0U);
          // Removing a fully suspended source also suppresses unconditional
          // epoll hangup notifications while the other end is backpressured.
          if (mask == 0) {
            if (endpoint.source != nullptr) {
              wl_event_source_remove(endpoint.source);
              endpoint.source = nullptr;
            }
          } else if (endpoint.source == nullptr) {
            endpoint.source = wl_event_loop_add_fd(owner.loop, endpoint.fd, mask, onReady, &endpoint);
            if (endpoint.source == nullptr) {
              return false;
            }
          } else if (mask != endpoint.mask && wl_event_source_fd_update(endpoint.source, mask) != 0) {
            return false;
          }
          endpoint.mask = mask;
        }
        return true;
      }

      bool read(size_t index) {
        auto& channel = channels[index];
        for (unsigned attempt = 0; attempt < kDispatchBudget && channel.canRead(); ++attempt) {
          std::array<char, kReadSize> bytes;
          const size_t room = std::min(bytes.size(), kMaxQueuedBytes - channel.queuedBytes);
          ipc::Received received =
              ipc::recvFds(endpoints[index].fd, std::as_writable_bytes(std::span(bytes)).first(room), kMaxQueuedFds);
          if (received.kind == ipc::Received::Kind::WouldBlock) {
            return true;
          }
          if (received.kind == ipc::Received::Kind::Failed || received.kind == ipc::Received::Kind::Truncated
              || channel.queuedFds + received.fds.size() > kMaxQueuedFds) {
            return false;
          }
          const auto count = static_cast<ptrdiff_t>(received.size);
          auto packet = std::make_unique<Packet>();
          packet->fds = std::move(received.fds);
          if (count == 0) {
            channel.eof = true;
            break;
          }
          packet->bytes.assign(bytes.begin(), bytes.begin() + count);
          channel.queuedBytes += static_cast<size_t>(count);
          channel.queuedFds += packet->fds.size();
          channel.packets.push_back(std::move(packet));
        }
        return true;
      }

      bool write(size_t index) {
        auto& channel = channels[index];
        for (unsigned attempt = 0; attempt < kDispatchBudget && !channel.packets.empty(); ++attempt) {
          Packet& packet = *channel.packets.front();
          std::vector<int> fds;
          for (const ipc::Fd& fd : packet.fds) {
            fds.push_back(fd.get());
          }
          const std::string_view rest(packet.bytes.data() + packet.offset, packet.bytes.size() - packet.offset);
          const ssize_t count = ipc::sendFds(endpoints[1 - index].fd, rest, fds, ipc::Deadline{});
          if (count < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK;
          }
          if (count == 0) {
            return false;
          }
          // SCM_RIGHTS is transferred with the first successfully written byte,
          // including when the stream write only accepts part of the packet.
          channel.queuedFds -= packet.fds.size();
          packet.fds.clear();
          channel.queuedBytes -= static_cast<size_t>(count);
          packet.offset += static_cast<size_t>(count);
          if (packet.offset == packet.bytes.size()) {
            channel.packets.pop_front();
          }
        }
        if (channel.eof && channel.packets.empty() && !channel.shutdown) {
          if (shutdown(endpoints[1 - index].fd, SHUT_WR) != 0 && errno != ENOTCONN) {
            return false;
          }
          channel.shutdown = true;
        }
        return true;
      }

      bool dispatch(size_t index, uint32_t mask) {
        if ((mask & WL_EVENT_ERROR) != 0) {
          return false;
        }
        if ((mask & WL_EVENT_WRITABLE) != 0 && !write(1 - index)) {
          return false;
        }
        if ((mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP)) != 0 && !read(index)) {
          return false;
        }
        if (!write(index)) {
          return false;
        }
        return !(channels[0].shutdown && channels[1].shutdown) && watch();
      }
    };

    wl_event_loop* loop;
    Authenticate authenticate;
    OpenClient openClient;
    int listener = -1;
    int endpoint = -1;
    wl_event_source* source = nullptr;
    std::vector<std::unique_ptr<Relay>> relays;

    Impl(wl_event_loop* loop, Authenticate authenticate, OpenClient openClient)
        : loop(loop), authenticate(std::move(authenticate)), openClient(std::move(openClient)) {}

    ~Impl() {
      if (source != nullptr) {
        wl_event_source_remove(source);
      }
      relays.clear();
      if (listener >= 0) {
        close(listener);
      }
      if (endpoint >= 0) {
        close(endpoint);
      }
    }

    void initialize() {
      const std::string runtime = xdg::runtimeDir();
      if (runtime.empty()) {
        kLog.error("XDG_RUNTIME_DIR is unavailable for input emulation");
        return;
      }
      std::string directory = runtime + "/andromeda-eis.XXXXXX";
      if (mkdtemp(directory.data()) == nullptr) {
        kLog.error("failed to create private input socket directory: {}", std::strerror(errno));
        return;
      }
      const std::string path = directory + "/socket";
      listener = ipc::listenUnix(path, SOCK_STREAM | SOCK_NONBLOCK, 16, 0, ipc::Stale::Keep).release();
      int failure = listener < 0 ? errno : 0;
      if (failure == 0) {
        endpoint = open(path.c_str(), O_PATH | O_CLOEXEC);
        if (endpoint < 0) {
          failure = errno;
        }
      }
      unlink(path.c_str());
      rmdir(directory.c_str());
      if (failure == 0) {
        source = wl_event_loop_add_fd(
            loop, listener, WL_EVENT_READABLE,
            [](int, uint32_t, void* data) -> int {
              static_cast<Impl*>(data)->accept();
              return 0;
            },
            this
        );
        if (source == nullptr) {
          failure = errno != 0 ? errno : ENOMEM;
        }
      }
      if (failure != 0) {
        kLog.error("failed to create private input socket: {}", std::strerror(failure));
        if (endpoint >= 0) {
          close(endpoint);
          endpoint = -1;
        }
        if (listener >= 0) {
          close(listener);
          listener = -1;
        }
      }
    }

    void accept() {
      for (unsigned attempt = 0; attempt < kDispatchBudget; ++attempt) {
        auto accepted = ipc::acceptPeer(listener, SOCK_NONBLOCK);
        if (!accepted) {
          if (errno == EINTR || errno == EACCES) {
            continue;
          }
          return;
        }
        // The pidfd confirms that the authenticated pid still names the peer.
        if (relays.size() >= kMaxConnections || accepted->pid <= 0 || !authenticate
            || !authenticate(accepted->pid) || !ipc::running(accepted->pidfd.get())) {
          continue;
        }
        const int peer = accepted->conn.release();
        const int backend = openClient ? openClient() : -1;
        if (backend < 0) {
          close(peer);
          continue;
        }
        // libeis owns the other end of this socketpair. These flags also avoid
        // leaking its returned endpoint into subsequently spawned applications.
        const int flags = fcntl(backend, F_GETFL);
        if (flags < 0 || fcntl(backend, F_SETFL, flags | O_NONBLOCK) != 0 || fcntl(backend, F_SETFD, FD_CLOEXEC) != 0) {
          close(backend);
          close(peer);
          continue;
        }
        auto relay = std::make_unique<Relay>(*this, peer, backend);
        if (relay->watch()) {
          relays.push_back(std::move(relay));
        }
      }
    }

    void remove(Relay* relay) {
      const auto found =
          std::ranges::find_if(relays, [relay](const auto& candidate) { return candidate.get() == relay; });
      if (found != relays.end()) {
        relays.erase(found);
      }
    }
  };

  EisSocket::EisSocket(wl_event_loop* loop, Authenticate authenticate, OpenClient openClient)
      : m_impl(std::make_unique<Impl>(loop, std::move(authenticate), std::move(openClient))) {
    m_impl->initialize();
  }
  EisSocket::~EisSocket() = default;
  int EisSocket::fd() const { return m_impl->endpoint; }

} // namespace andromeda
