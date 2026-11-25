#pragma once

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

// A bus peer that busCaller sees as the Flatpak app `appId`: `python3 -I -c script args...` in bwrap with a
// /.flatpak-info. Exits the test with 77 (meson skip) when bwrap is not installed.
class SandboxedPeer {
public:
  SandboxedPeer(const std::string& appId, const char* script, const std::vector<std::string>& args = {})
      : m_info("/tmp/sandboxed-peer-" + std::to_string(::getpid()) + ".flatpak-info") {
    std::ofstream(m_info) << "[Context]\nshared=network\n\n[Application]\nname=" << appId << "\nruntime=x\n";
    std::vector<const char*> argv{"bwrap", "--ro-bind", "/usr", "/usr", "--symlink", "usr/bin", "/bin",
                                  "--symlink", "usr/lib", "/lib", "--symlink", "usr/lib", "/lib64", "--proc", "/proc",
                                  "--dev", "/dev", "--bind", "/tmp", "/tmp", "--ro-bind", m_info.c_str(),
                                  "/.flatpak-info", "--", "python3", "-I", "-c", script};
    for (const std::string& arg : args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    if (::posix_spawnp(&m_pid, "bwrap", nullptr, nullptr, const_cast<char**>(argv.data()), environ) != 0) {
      std::remove(m_info.c_str());
      std::fprintf(stderr, "bwrap not available: skipped\n");
      std::exit(77);
    }
  }

  ~SandboxedPeer() {
    if (m_pid > 0) {
      ::kill(m_pid, SIGTERM);
      ::waitpid(m_pid, nullptr, 0);
    }
    std::remove(m_info.c_str());
  }

  SandboxedPeer(const SandboxedPeer&) = delete;
  SandboxedPeer& operator=(const SandboxedPeer&) = delete;

  // The exit code once the peer has exited.
  std::optional<int> exitCode() {
    int status = 0;
    if (m_pid > 0 && ::waitpid(m_pid, &status, WNOHANG) == m_pid) {
      m_pid = 0;
      m_exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    return m_exit;
  }

private:
  std::string m_info;
  pid_t m_pid = 0;
  std::optional<int> m_exit;
};
