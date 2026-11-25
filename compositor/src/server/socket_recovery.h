#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // Recreates the compositor's sockets when something deletes their files in the runtime dir: an inotify fd on the
  // event loop (no timer, no idle work). `recover` returns false when it gave up; the name is then no longer watched.
  class SocketRecovery {
  public:
    // At most this many recoveries per name: each Wayland recovery leaves libwayland one dead listening fd.
    static constexpr int kMaxRecoveries = 8;

    SocketRecovery(wl_event_loop* loop, std::string runtimeDir);
    ~SocketRecovery();

    SocketRecovery(const SocketRecovery&) = delete;
    SocketRecovery& operator=(const SocketRecovery&) = delete;

    void watch(const std::string& name, std::function<bool()> recover);

  private:
    struct Entry {
      std::function<bool()> recover;
      int recoveries = 0;
    };

    static int onReadable(int fd, uint32_t mask, void* data);
    void handle(const std::string& name);
    void stop();

    std::string m_runtimeDir;
    int m_fd = -1;
    wl_event_source* m_source = nullptr;
    std::unordered_map<std::string, Entry> m_entries;
  };

} // namespace andromeda
