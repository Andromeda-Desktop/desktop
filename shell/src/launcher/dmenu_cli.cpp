#include "launcher/dmenu_cli.h"

#include "cli/parse.h"
#include "cli/schema_dmenu.h"
#include "launcher/dmenu_socket_path.h"
#include "protocols/ipc_channel.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace noctalia::launcher {

  int runDmenuCli(int argc, char** argv) {
    auto parsed = cli::parseOrReport(
        cli::kDmenuCmd, "noctalia dmenu", std::span<char* const>{argv + 2, static_cast<std::size_t>(argc - 2)}
    );
    if (!parsed)
      return 1;
    if (parsed->helpRequested)
      return 0;
    const std::string prompt{parsed->value("--prompt")};

    // Read all of stdin.
    std::string candidates;
    {
      char buf[8192];
      for (;;) {
        const auto n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
        candidates.append(buf, static_cast<std::size_t>(n));
      }
    }

    const auto socketPath = resolveDmenuSocketPath();
    if (socketPath.path.empty()) {
      std::println(stderr, "error: {}", socketPath.error);
      return 1;
    }

    // Bound the connect, and the send by its size (the shell drains up to 16 MiB from its main loop), not the reply:
    // the user may take any time to pick.
    const auto now = std::chrono::steady_clock::now();
    const ipc::Fd fd = ipc::connectTo(socketPath.path, now + std::chrono::seconds(2));
    if (!fd) {
      std::println(stderr, "error: noctalia is not running");
      return 1;
    }
    // Header line is the prompt, then candidate bytes follow on the same connection.
    const auto deadline = now + std::chrono::seconds(2 + (candidates.size() >> 20));
    if (!ipc::writeAll(fd.get(), prompt, deadline) || !ipc::writeAll(fd.get(), "\n", deadline)
        || !ipc::writeAll(fd.get(), candidates, deadline)) {
      std::println(stderr, "error: write() failed: {}", std::strerror(errno));
      return 1;
    }
    ::shutdown(fd.get(), SHUT_WR); // signal end of candidates

    // The selection, until the daemon closes the connection.
    const ipc::Read reply = ipc::readAll(fd.get(), 16 << 20, ipc::Deadline::max());
    if (reply.status != ipc::Status::Ok) {
      std::println(stderr, "error: read() failed: {}", reply.status == ipc::Status::TooLong ? "reply too long" : std::strerror(errno));
      return 1;
    }
    const std::string& response = reply.data;
    if (response.empty()) {
      return 1; // cancelled
    }
    std::fputs(response.c_str(), stdout);
    return 0;
  }

} // namespace noctalia::launcher
