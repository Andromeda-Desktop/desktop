// andromeda-askpass: SUDO_ASKPASS helper. Asks the running shell for the password over its IPC socket and prints
// it to stdout. Exit 1 with empty stdout on cancel, timeout or when no shell runs.

#include "protocols/ipc_channel.h"
#include "protocols/xdg_dirs.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr int kTimeoutMs = 120 * 1000;
  constexpr std::size_t kReplyMax = 4096;

  int noShell() {
    std::fputs("andromeda-askpass: no Andromeda shell session; run sudo in a terminal\n", stderr);
    return 1;
  }

} // namespace

int main() {
  // getenv (in xdg::runtimeDir), not secure_getenv, under setgid: the environment only picks the socket, and the shell checks the peer.
  const std::string runtime = xdg::runtimeDir();
  if (runtime.empty()) {
    return noShell();
  }
  const char* display = std::getenv("WAYLAND_DISPLAY");
  if (display == nullptr || display[0] == '\0') {
    display = "wayland-0";
  }
  const std::string path = runtime + "/noctalia-" + display + ".sock";

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const ipc::Fd connected = ipc::connectTo(path, deadline);
  // The prompt (argv[1]) is not sent: the shell reads the caller from /proc. The shell knows this helper by the
  // setgid group in SO_PEERCRED, then answers "verify".
  if (!connected || !ipc::writeAll(connected.get(), "askpass\n", deadline)) {
    return noShell();
  }
  const int fd = connected.get();

  // One page, never reallocated, kept out of core dumps and wiped before exit.
  char* reply = static_cast<char*>(::mmap(nullptr, kReplyMax, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (reply == MAP_FAILED) {
    return 1;
  }
  ::madvise(reply, kReplyMax, MADV_DONTDUMP);
  const auto finish = [reply](int code) {
    explicit_bzero(reply, kReplyMax);
    ::munmap(reply, kReplyMax);
    return code;
  };
  constexpr std::string_view kVerify = "verify\n";
  bool verified = false;
  std::size_t size = 0;
  pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
  for (;;) {
    const int ready = ::poll(&pfd, 1, kTimeoutMs);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0) {
      std::fputs("andromeda-askpass: no answer in 120 s\n", stderr);
      return finish(1);
    }
    if (size == kReplyMax) {
      return finish(1);
    }
    const auto n = ::read(fd, reply + size, kReplyMax - size);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    size += static_cast<std::size_t>(n);
    if (!verified && std::string_view(reply, size).starts_with(kVerify)) {
      // The setgid exec already made it non-dumpable; this keeps it so when installed without the setgid bit.
      if (::prctl(PR_SET_DUMPABLE, 0) != 0
          || !ipc::writeAll(fd, "ready\n", std::chrono::steady_clock::now() + std::chrono::seconds(5))) {
        return finish(1);
      }
      verified = true;
      std::memmove(reply, reply + kVerify.size(), size - kVerify.size());
      size -= kVerify.size();
    }
  }

  const std::string_view text(reply, size);
  constexpr std::string_view kPassword = "password\n";
  if (!verified || !text.starts_with(kPassword) || !text.ends_with('\n')) {
    if (text.starts_with("error: ") && text.ends_with('\n')) {
      std::fputs("andromeda-askpass: ", stderr);
      std::fwrite(text.data() + 7, 1, text.size() - 7, stderr);
    }
    return finish(1);
  }
  // write(2), not stdio: no copy of the password in a stdio buffer.
  std::string_view out = text.substr(kPassword.size());
  while (!out.empty()) {
    const auto n = ::write(STDOUT_FILENO, out.data(), out.size());
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return finish(1);
    }
    out.remove_prefix(static_cast<std::size_t>(n));
  }
  return finish(0);
}
