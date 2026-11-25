// protocol/ipc_channel against random byte streams: the line reader never holds more than its cap, delivers every
// line that fits in order, and ends the stream at the first line that does not.
#include "protocols/ipc_channel.h"

#include "check.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
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
