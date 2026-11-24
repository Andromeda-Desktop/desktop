#include "core/process/app_zygote.h"
#include "core/sys/sys.h"

#include <dlfcn.h>
#include <glib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

  // Pango 1.50+ runs fontconfig jobs on one worker that waits on a GAsyncQueue (its thread data). fork() copies the queue but not the
  // thread, so the child starts the worker again with the same function and queue.
  GThreadFunc g_pangoWorkerFunc = nullptr;
  gpointer g_pangoWorkerData = nullptr;
  constexpr std::string_view kPangoWorker = "[pango] fontconfig";

  int threadCount() {
    int count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
      ++count;
    }
    return count;
  }

  // A package upgrade replaces the zygote or libdesk; a child of the old version would load a newer app library.
  bool selfReplaced() {
    std::error_code ec;
    if (std::filesystem::read_symlink("/proc/self/exe", ec).native().ends_with(" (deleted)")) {
      return true;
    }
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);) {
      if (line.find("/libandromeda-") != std::string::npos && line.ends_with(" (deleted)")) {
        return true;
      }
    }
    return false;
  }

  [[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "zygote: %s: %s\n", what, std::strerror(errno));
    std::exit(1);
  }

} // namespace

// libandromeda-desk.so exports this symbol ahead of GLib in the load order, so Pango's call resolves here.
extern "C" __attribute__((visibility("default"))) GThread* g_thread_new(const gchar* name, GThreadFunc func, gpointer data) {
  using Fn = GThread* (*)(const gchar*, GThreadFunc, gpointer);
  static const auto real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "g_thread_new"));
  if (name != nullptr && name == kPangoWorker) {
    g_pangoWorkerFunc = func;
    g_pangoWorkerData = data;
  }
  return real(name, func, data);
}

namespace process::zygote {

  Request serve(
      std::string_view appId, const std::function<void()>& preload, const std::function<bool(const Request&)>& accept
  ) {
    const auto path = socketPath(appId);
    sockaddr_un addr{};
    if (path.empty() || path.native().size() >= sizeof(addr.sun_path)) {
      errno = ENAMETOOLONG;
      fail("socket path");
    }
    preload();
    const int expectedThreads = g_pangoWorkerFunc != nullptr ? 2 : 1;
    if (threadCount() != expectedThreads) {
      errno = EBUSY;
      fail("preload left threads running");
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    unlink(path.c_str());
    const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.native().size());
    if (listener < 0 || bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0
        || chmod(path.c_str(), 0600) != 0 || listen(listener, 16) != 0) {
      fail("listen");
    }
    struct sigaction reap{};
    reap.sa_handler = SIG_IGN;
    reap.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &reap, nullptr);
    std::fprintf(stderr, "zygote: %.*s ready\n", static_cast<int>(appId.size()), appId.data());

    std::array<char, 64 * 1024> buffer{};
    for (;;) {
      const int conn = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (conn < 0) {
        continue;
      }
      // One thread serves every launch: a client that connects and stays silent must not block the next one.
      const timeval timeout{.tv_sec = 1, .tv_usec = 0};
      setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      ucred peer{};
      socklen_t peerSize = sizeof(peer);
      const ssize_t size = getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) == 0 && peer.uid == getuid()
          ? recv(conn, buffer.data(), buffer.size(), 0)
          : -1;
      auto request = size > 0 ? decode(std::span(buffer.data(), static_cast<std::size_t>(size))) : std::nullopt;
      if (!request || !accept(*request) || std::chrono::steady_clock::now() > request->deadline) {
        const std::int32_t refused = -1;
        send(conn, &refused, sizeof(refused), MSG_NOSIGNAL);
        close(conn);
        continue;
      }
      if (selfReplaced()) {
        std::fprintf(stderr, "zygote: replaced by an upgrade; exiting\n");
        std::exit(0);
      }
      const pid_t pid = fork();
      if (pid == 0) {
        close(listener);
        close(conn);
        sys::resetChildSignals();
        setsid();
        if (!request->workingDir.empty() && chdir(request->workingDir.c_str()) != 0) {
          std::perror("zygote: chdir");
        }
        if (!request->activationToken.empty()) {
          setenv("XDG_ACTIVATION_TOKEN", request->activationToken.c_str(), 1);
          setenv("DESKTOP_STARTUP_ID", request->activationToken.c_str(), 1);
        }
        if (g_pangoWorkerFunc != nullptr) {
          // The copied queue still counts the zygote's waiting worker, so its last unref would fail GLib's
          // waiting_threads check. This reference is never dropped; the queue lives until exit.
          g_async_queue_ref(static_cast<GAsyncQueue*>(g_pangoWorkerData));
          g_thread_unref(g_thread_new(kPangoWorker.data(), g_pangoWorkerFunc, g_pangoWorkerData));
        }
        return std::move(*request);
      }
      const std::int32_t reply = pid > 0 ? pid : -1;
      send(conn, &reply, sizeof(reply), MSG_NOSIGNAL);
      close(conn);
    }
  }

} // namespace process::zygote
