// The control transport on macOS: a Unix socket in the user's temp dir, one JSON object per line each way.
// Request {"method": M, "args": [...]} as the D-Bus method M takes them; reply {"result": R} or
// {"error": NAME, "message": TEXT}. Signals have no socket form.
#include "terminal_app.h"
#include "terminal_control.h"

#include "core/log.h"
#include "core/poll_source.h"
#include "core/sys/sys.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace term {

  namespace {

    constexpr Logger kLog("terminal-control");

    std::string socketPath() { return sys::userRuntimeDir() + "/dev.andromeda.Terminal.sock"; }

    bool toAddress(const std::string& path, sockaddr_un& addr) {
      addr = {};
      addr.sun_family = AF_UNIX;
      if (path.size() >= sizeof addr.sun_path) {
        return false;
      }
      std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
      return true;
    }

    bool setNonblocking(int fd) { return ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) == 0; }

    class SocketServer final : public control::Server, public PollSource {
    public:
      using Handler = std::function<nlohmann::json(const std::string& method, const nlohmann::json& args)>;

      SocketServer(AppLoop& loop, int listenFd, std::string path, Handler handler)
          : m_loop(loop), m_listen(listenFd), m_path(std::move(path)), m_handler(std::move(handler)) {
        m_loop.addSource(this);
      }

      ~SocketServer() override {
        m_loop.removeSource(this);
        for (const Client& c : m_clients) {
          ::close(c.fd);
        }
        ::close(m_listen);
        ::unlink(m_path.c_str());
      }

      // No socket client subscribes to SessionsChanged or WorkspacesChanged.
      void emitSignal(const char* /*name*/) override {}

      void dispatch(const std::vector<pollfd>& fds, std::size_t start) override {
        const std::size_t count = m_clients.size();
        for (std::size_t i = 0; i < count; ++i) {
          Client& c = m_clients[i];
          const short revents = fds[start + 1 + i].revents;
          if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            readFrom(c);
          }
          if (!c.out.empty()) {
            writeTo(c);
          }
        }
        std::erase_if(m_clients, [](const Client& c) {
          if (c.closed || (c.peerDone && c.out.empty())) {
            ::close(c.fd);
            return true;
          }
          return false;
        });
        if ((fds[start].revents & POLLIN) != 0) {
          accept();
        }
      }

    protected:
      void doAddPollFds(std::vector<pollfd>& fds) override {
        fds.push_back({m_listen, POLLIN, 0});
        for (const Client& c : m_clients) {
          const short events = static_cast<short>((c.peerDone ? 0 : POLLIN) | (c.out.empty() ? 0 : POLLOUT));
          fds.push_back({c.fd, events, 0});
        }
      }

    private:
      struct Client {
        int fd = -1;
        std::string in;
        std::string out;
        bool peerDone = false; // read EOF: flush the replies, then close
        bool closed = false;
      };

      void accept() {
        for (;;) {
          const int fd = ::accept(m_listen, nullptr, nullptr);
          if (fd < 0) {
            return;
          }
          if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || !setNonblocking(fd)) {
            ::close(fd);
            continue;
          }
          m_clients.push_back(Client{.fd = fd});
        }
      }

      void readFrom(Client& c) {
        char buffer[16 * 1024];
        for (;;) {
          const ssize_t n = ::read(c.fd, buffer, sizeof buffer);
          if (n > 0) {
            c.in.append(buffer, static_cast<std::size_t>(n));
          } else if (n == 0) {
            c.peerDone = true;
            break;
          } else {
            c.closed = errno != EAGAIN && errno != EINTR;
            break;
          }
        }
        for (std::size_t eol; (eol = c.in.find('\n')) != std::string::npos;) {
          c.out += answer(c.in.substr(0, eol)).dump() + '\n';
          c.in.erase(0, eol + 1);
        }
      }

      void writeTo(Client& c) {
        const ssize_t n = sys::sendNoSignal(c.fd, c.out.data(), c.out.size(), 0);
        if (n > 0) {
          c.out.erase(0, static_cast<std::size_t>(n));
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
          c.closed = true;
        }
      }

      nlohmann::json answer(const std::string& line) {
        try {
          const nlohmann::json request = nlohmann::json::parse(line);
          return {{"result", m_handler(request.at("method").get<std::string>(), request.value("args", nlohmann::json::array()))}};
        } catch (const control::Error& e) {
          return {{"error", e.name}, {"message", e.what()}};
        } catch (const nlohmann::json::exception& e) {
          return {{"error", control::kInvalidArgs}, {"message", e.what()}};
        }
      }

      AppLoop& m_loop;
      int m_listen;
      std::string m_path;
      Handler m_handler;
      std::vector<Client> m_clients;
    };

  } // namespace

  void TerminalApp::exportControl() {
    const std::string path = socketPath();
    sockaddr_un addr;
    const int fd = sys::socketCloexec(AF_UNIX, SOCK_STREAM);
    if (fd < 0 || !toAddress(path, addr) || !setNonblocking(fd)) {
      kLog.warn("control socket {}: {}", path, std::strerror(errno));
      if (fd >= 0) ::close(fd);
      return;
    }
    // One app per user: a socket left by a crashed run goes.
    ::unlink(path.c_str());
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 8) != 0) {
      kLog.warn("control socket {}: {}", path, std::strerror(errno));
      ::close(fd);
      return;
    }
    m_control = std::make_unique<SocketServer>(loop(), fd, path, [this](const std::string& method, const nlohmann::json& args) {
      return controlCall(method, args);
    });
  }

  nlohmann::json control::call(const std::string& method, const nlohmann::json& args) {
    const std::string path = socketPath();
    sockaddr_un addr;
    if (!toAddress(path, addr)) {
      throw Error(kNotRunning, "socket path too long: " + path);
    }
    const int fd = sys::socketCloexec(AF_UNIX, SOCK_STREAM);
    if (fd < 0) {
      throw Error(kNotRunning, std::strerror(errno));
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
      ::close(fd);
      throw Error(kNotRunning, "no terminal is running");
    }
    const std::string request = nlohmann::json{{"method", method}, {"args", args}}.dump() + '\n';
    std::string reply;
    bool ok = true;
    for (std::size_t done = 0; ok && done < request.size();) {
      const ssize_t n = sys::sendNoSignal(fd, request.data() + done, request.size() - done, 0);
      ok = n > 0 || (n < 0 && errno == EINTR);
      done += n > 0 ? static_cast<std::size_t>(n) : 0;
    }
    char buffer[16 * 1024];
    while (ok && reply.find('\n') == std::string::npos) {
      const ssize_t n = ::read(fd, buffer, sizeof buffer);
      ok = n > 0 || (n < 0 && errno == EINTR);
      reply.append(buffer, n > 0 ? static_cast<std::size_t>(n) : 0);
    }
    ::close(fd);
    if (!ok) {
      throw Error(kNotRunning, "the terminal closed the control connection");
    }
    const nlohmann::json answer = nlohmann::json::parse(reply.substr(0, reply.find('\n')));
    if (answer.contains("error")) {
      throw Error(answer.at("error").get<std::string>(), answer.value("message", std::string{}));
    }
    return answer.at("result");
  }

} // namespace term
