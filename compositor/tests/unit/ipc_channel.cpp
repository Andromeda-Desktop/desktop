// protocol/ipc_channel against random byte streams: the line reader never holds more than its cap, delivers every
// line that fits in order, and ends the stream at the first line that does not.
#include "protocols/ipc_channel.h"

#include "check.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {

  using namespace std::chrono_literals;

  ipc::Deadline in(std::chrono::milliseconds ms) { return std::chrono::steady_clock::now() + ms; }

  std::string randomLine(std::mt19937& rng, std::size_t maxLength) {
    std::string line(std::uniform_int_distribution<std::size_t>(0, maxLength)(rng), '\0');
    for (char& c : line) {
      do {
        c = static_cast<char>(std::uniform_int_distribution<int>(0, 255)(rng));
      } while (c == '\n');
    }
    return line;
  }

  ANDROMEDA_TEST(lineReaderRandomStreams) {
    std::mt19937 rng(20261124);
    for (int round = 0; round < 300; ++round) {
      const std::size_t cap = std::uniform_int_distribution<std::size_t>(1, 300)(rng);
      std::vector<std::string> lines(std::uniform_int_distribution<std::size_t>(0, 40)(rng));
      // Mostly lines that fit, sometimes one that does not.
      const std::size_t maxLength = std::bernoulli_distribution(0.3)(rng) ? 2 * cap : cap - 1;
      std::string stream;
      for (auto& line : lines) {
        line = randomLine(rng, maxLength);
        stream += line + '\n';
      }
      // Sometimes the last line ends with end of file instead of '\n'.
      const bool unterminated = !lines.empty() && std::bernoulli_distribution(0.3)(rng);
      if (unterminated) {
        stream.pop_back();
      }
      std::size_t fitting = 0;
      while (fitting < lines.size() && lines[fitting].size() + 1 <= cap) {
        ++fitting;
      }

      int fds[2];
      CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
      ipc::LineReader reader(cap);
      std::vector<std::string> got;
      const auto readSome = [&] {
        reader.readFrom(fds[0]);
        CHECK(reader.buffered() <= cap);
        while (auto line = reader.nextLine()) {
          got.push_back(std::move(*line));
        }
      };
      std::string_view rest = stream;
      while (!rest.empty() && reader.state() == ipc::LineReader::State::Reading) {
        const std::size_t chunk = std::uniform_int_distribution<std::size_t>(1, 2 * cap + 8)(rng);
        const ssize_t sent = send(fds[1], rest.data(), std::min(chunk, rest.size()), MSG_NOSIGNAL);
        if (sent > 0) {
          rest.remove_prefix(static_cast<std::size_t>(sent));
        }
        for (int reads = std::uniform_int_distribution<int>(1, 3)(rng); reads > 0; --reads) {
          readSome();
        }
      }
      close(fds[1]);
      while (reader.state() == ipc::LineReader::State::Reading) {
        readSome();
      }
      close(fds[0]);

      const bool tooLong = fitting < lines.size();
      CHECK(reader.state() == (tooLong ? ipc::LineReader::State::TooLong : ipc::LineReader::State::Closed));
      if (unterminated && !tooLong) {
        got.push_back(reader.takeRest());
      }
      CHECK_EQ(got.size(), fitting);
      for (std::size_t i = 0; i < std::min(got.size(), fitting); ++i) {
        CHECK(got[i] == lines[i]);
      }
    }
  }

  ANDROMEDA_TEST(readAllCapsAndDeadline) {
    std::mt19937 rng(7);
    for (int round = 0; round < 100; ++round) {
      const std::size_t cap = std::uniform_int_distribution<std::size_t>(0, 20000)(rng);
      const std::string data = randomLine(rng, 20000);
      int fds[2];
      CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
      bool written = false;
      std::thread writer([&] {
        written = ipc::writeAll(fds[1], data, in(2s));
        close(fds[1]);
      });
      const auto read = ipc::readAll(fds[0], cap, in(2s));
      close(fds[0]);
      writer.join();
      if (data.size() <= cap) {
        CHECK(written);
        CHECK(read.status == ipc::Status::Ok);
        CHECK(read.data == data);
      } else {
        CHECK(read.status == ipc::Status::TooLong);
        CHECK(read.data == data.substr(0, cap));
      }
    }

    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    const auto start = std::chrono::steady_clock::now();
    CHECK(ipc::readAll(fds[0], 100, in(150ms)).status == ipc::Status::TimedOut);
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    CHECK(ipc::writeAll(fds[1], "askpass\nrest", in(1s)));
    const auto line = ipc::readAll(fds[0], 100, in(1s), [](std::string_view data) { return data.contains('\n'); });
    CHECK(line.status == ipc::Status::Ok);
    CHECK(line.data.starts_with("askpass\n"));
    // A peer that never reads: the write stops at the deadline, and errno says why.
    errno = 0;
    CHECK(!ipc::writeAll(fds[1], std::string(8 << 20, 'x'), in(100ms)));
    CHECK_EQ(errno, ETIMEDOUT);
    close(fds[0]);
    close(fds[1]);
  }

  ANDROMEDA_TEST(clientSlotsModel) {
    std::mt19937 rng(3);
    ipc::ClientSlots slots(5);
    std::vector<ipc::ClientSlots::Slot> held;
    for (int step = 0; step < 2000; ++step) {
      if (std::bernoulli_distribution(0.55)(rng)) {
        auto slot = slots.acquire();
        CHECK(slot.has_value() == (held.size() < 5));
        if (slot) {
          held.push_back(std::move(*slot));
        }
      } else if (!held.empty()) {
        held.erase(held.begin() + std::uniform_int_distribution<std::ptrdiff_t>(0, std::ssize(held) - 1)(rng));
      }
      CHECK_EQ(slots.used(), held.size());
    }
  }

  ANDROMEDA_TEST(acceptPeerAndRequest) {
    const std::string path = (std::filesystem::temp_directory_path() / std::format("ipc-channel-{}.sock", getpid())).string();
    CHECK(ipc::request(path, "x", ipc::EndOfRequest::HalfClose, 64, in(200ms)).status == ipc::Status::NoServer);

    const ipc::Fd listener(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_un addr{.sun_family = AF_UNIX, .sun_path = {}};
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    unlink(path.c_str());
    CHECK(bind(listener.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(listen(listener.get(), 4) == 0);
    std::thread server([&] {
      auto peer = ipc::acceptPeer(listener.get(), 0);
      CHECK(peer.has_value());
      if (!peer) {
        return;
      }
      CHECK_EQ(peer->pid, getpid());
      CHECK(peer->pidfd.get() >= 0);
      const auto query = ipc::readAll(peer->conn.get(), 64, in(2s));
      CHECK(query.status == ipc::Status::Ok);
      CHECK(ipc::writeAll(peer->conn.get(), "reply to " + query.data, in(2s)));
    });
    const auto reply = ipc::request(path, "ping", ipc::EndOfRequest::HalfClose, 64, in(2s));
    server.join();
    CHECK(reply.status == ipc::Status::Ok);
    CHECK(reply.data == "reply to ping");
    unlink(path.c_str());
  }

  ANDROMEDA_TEST(listenUnixStalePolicy) {
    const std::string path = (std::filesystem::temp_directory_path() / std::format("ipc-listen-{}.sock", getpid())).string();
    unlink(path.c_str());
    ipc::Fd live = ipc::listenUnix(path, SOCK_STREAM, 4, 0600, ipc::Stale::Keep);
    CHECK(static_cast<bool>(live));
    struct stat st{};
    CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);

    // A live server's socket survives Keep; a connect still reaches it.
    errno = 0;
    CHECK(!ipc::listenUnix(path, SOCK_STREAM, 4, 0, ipc::Stale::Keep));
    CHECK_EQ(errno, EADDRINUSE);
    CHECK(static_cast<bool>(ipc::connectTo(path, in(200ms))));
    // Replace keeps a live server's path too, whatever socket type asks for it.
    for (const int replaceType : {SOCK_STREAM, SOCK_SEQPACKET}) {
      errno = 0;
      CHECK(!ipc::listenUnix(path, replaceType, 4, 0, ipc::Stale::Replace));
      CHECK_EQ(errno, EADDRINUSE);
      CHECK(static_cast<bool>(ipc::connectTo(path, in(200ms))));
    }

    // A stale file (its server gone) still blocks Keep; Replace takes the path.
    live = ipc::Fd();
    CHECK(!ipc::listenUnix(path, SOCK_STREAM, 4, 0, ipc::Stale::Keep));
    const ipc::Fd replaced = ipc::listenUnix(path, SOCK_SEQPACKET | SOCK_NONBLOCK, 4, 0, ipc::Stale::Replace);
    CHECK(static_cast<bool>(replaced));
    int type = 0;
    socklen_t length = sizeof(type);
    CHECK(getsockopt(replaced.get(), SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_SEQPACKET);
    CHECK((fcntl(replaced.get(), F_GETFD) & FD_CLOEXEC) != 0 && (fcntl(replaced.get(), F_GETFL) & O_NONBLOCK) != 0);

    // connectTo: the socket type must match the listener's; SOCK_NONBLOCK applies to the connected fd.
    CHECK(!ipc::connectTo(path, in(200ms)));
    const ipc::Fd client = ipc::connectTo(path, in(200ms), SOCK_SEQPACKET | SOCK_NONBLOCK);
    CHECK(static_cast<bool>(client));
    CHECK((fcntl(client.get(), F_GETFL) & O_NONBLOCK) != 0 && (fcntl(client.get(), F_GETFD) & FD_CLOEXEC) != 0);
    errno = 0;
    CHECK(!ipc::connectTo(path + ".missing", in(200ms)));
    CHECK_EQ(errno, ENOENT);

    errno = 0;
    CHECK(!ipc::listenUnix(std::string(200, 'x'), SOCK_STREAM, 4, 0, ipc::Stale::Replace));
    CHECK_EQ(errno, ENAMETOOLONG);
    unlink(path.c_str());
  }

  // Servers that start together on one stale path: exactly one gets it, and the path reaches that one. Without a
  // lock over probe, unlink and bind, a later server unlinks an earlier one's fresh socket and both report success.
  ANDROMEDA_TEST(listenUnixReplaceRace) {
    const std::string path = (std::filesystem::temp_directory_path() / std::format("ipc-race-{}.sock", getpid())).string();
    constexpr int kServers = 12;
    for (int round = 0; round < 40; ++round) {
      unlink(path.c_str());
      ipc::Fd stale = ipc::listenUnix(path, SOCK_STREAM, 4, 0, ipc::Stale::Keep);
      CHECK(static_cast<bool>(stale));
      stale = ipc::Fd();
      std::array<int, 2> go{};
      std::array<int, 2> results{};
      std::array<int, 2> release{};
      CHECK(pipe(go.data()) == 0 && pipe(results.data()) == 0 && pipe(release.data()) == 0);
      std::vector<pid_t> children;
      for (int i = 0; i < kServers; ++i) {
        const pid_t pid = fork();
        if (pid == 0) {
          close(go[1]);
          close(results[0]);
          close(release[1]);
          char byte = 0;
          (void)!read(go[0], &byte, 1);
          const ipc::Fd fd = ipc::listenUnix(path, SOCK_STREAM, kServers * 2, 0, ipc::Stale::Replace);
          const char won = fd ? 1 : 0;
          (void)!write(results[1], &won, 1);
          (void)!read(release[0], &byte, 1);
          _exit(0);
        }
        children.push_back(pid);
      }
      close(go[0]);
      close(results[1]);
      close(release[0]);
      close(go[1]);
      int winners = 0;
      for (int i = 0; i < kServers; ++i) {
        char won = 0;
        CHECK(read(results[0], &won, 1) == 1);
        winners += won;
      }
      CHECK_EQ(winners, 1);
      CHECK(static_cast<bool>(ipc::connectTo(path, in(200ms))));
      close(release[1]);
      close(results[0]);
      for (const pid_t pid : children) {
        waitpid(pid, nullptr, 0);
      }
      if (winners != 1) {
        break;
      }
    }
    unlink(path.c_str());
  }

  ANDROMEDA_TEST(fdPassing) {
    const auto openFds = [] {
      return std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{});
    };
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    const ipc::Fd reader(pair[0]);
    const ipc::Fd writer(pair[1]);
    std::array<std::byte, 8> buffer{};
    CHECK(ipc::recvFds(reader.get(), buffer, 2).kind == ipc::Received::Kind::WouldBlock);

    int pipes[2];
    CHECK(pipe(pipes) == 0);
    const ipc::Fd pipeRead(pipes[0]);
    const ipc::Fd pipeWrite(pipes[1]);
    const std::array<int, 2> two{pipes[0], pipes[1]};
    CHECK(ipc::sendFds(writer.get(), "abc", two, in(1s)) == 3);
    const ipc::Received got = ipc::recvFds(reader.get(), buffer, 2);
    CHECK(got.kind == ipc::Received::Kind::Data && got.size == 3 && got.fds.size() == 2);
    CHECK((fcntl(got.fds[0].get(), F_GETFD) & FD_CLOEXEC) != 0);
    char byte = 0;
    CHECK(write(got.fds[1].get(), "x", 1) == 1 && read(pipes[0], &byte, 1) == 1 && byte == 'x');

    // More fds than room, or more bytes: Truncated, and no received fd stays open.
    const auto before = openFds();
    CHECK(ipc::sendFds(writer.get(), "abc", two, in(1s)) == 3);
    CHECK(ipc::recvFds(reader.get(), buffer, 1).kind == ipc::Received::Kind::Truncated);
    CHECK(ipc::sendFds(writer.get(), std::string(20, 'y'), two, in(1s)) == 20);
    CHECK(ipc::recvFds(reader.get(), buffer, 2).kind == ipc::Received::Kind::Truncated);
    CHECK_EQ(openFds(), before);

    // A full socket: a past deadline fails at once with EAGAIN, a near one with ETIMEDOUT.
    while (ipc::sendFds(writer.get(), "z", {}, ipc::Deadline{}) == 1) {
    }
    CHECK_EQ(errno, EAGAIN);
    CHECK(ipc::sendFds(writer.get(), "z", {}, in(50ms)) == -1);
    CHECK_EQ(errno, ETIMEDOUT);

    int closing[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, closing) == 0);
    const ipc::Fd open(closing[0]);
    close(closing[1]);
    CHECK(ipc::recvFds(open.get(), buffer, 2).kind == ipc::Received::Kind::Closed);
  }

  ANDROMEDA_TEST(refuseAPeerOfAnotherUid) {
    const std::string name = std::format("ipc-channel-uid-{}", getpid());
    const ipc::Fd listener(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_un addr{.sun_family = AF_UNIX, .sun_path = {}};
    std::memcpy(addr.sun_path + 1, name.c_str(), name.size());
    const auto size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    CHECK(bind(listener.get(), reinterpret_cast<const sockaddr*>(&addr), size) == 0);
    CHECK(listen(listener.get(), 4) == 0);
    // A subordinate uid (newuidmap, /etc/subuid) connects without root, through the shared network namespace.
    const char* script = "import socket, sys\nc = socket.socket(socket.AF_UNIX)\nc.connect('\\0' + sys.argv[1])\nc.recv(1)\n";
    const char* argv[] = {"unshare", "--map-auto", "--map-root-user", "setpriv", "--reuid=1", "--regid=1",
                          "--clear-groups", "python3", "-I", "-c", script, name.c_str(), nullptr};
    pid_t child = 0;
    CHECK(posix_spawnp(&child, "unshare", nullptr, nullptr, const_cast<char**>(argv), environ) == 0);
    pollfd waiting{.fd = listener.get(), .events = POLLIN, .revents = 0};
    if (poll(&waiting, 1, 5000) != 1) {
      kill(child, SIGKILL);
      waitpid(child, nullptr, 0);
      std::fprintf(stderr, "refuseAPeerOfAnotherUid skipped: no subordinate uid peer\n");
      return;
    }
    errno = 0;
    CHECK(!ipc::acceptPeer(listener.get(), 0).has_value());
    CHECK_EQ(errno, EACCES);
    int status = 0;
    CHECK_EQ(waitpid(child, &status, 0), child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }

} // namespace

int main() { return RUN_TESTS(); }
