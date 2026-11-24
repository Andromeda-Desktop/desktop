// Children of a parent that ignores SIGPIPE and blocks signals start with default dispositions and an empty mask,
// for fork() and posix_spawn() alike; spawnDetached leaves no child of the caller behind.
#include "core/sys/sys.h"
#include "tests/test_check.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace {

  std::string field(const std::string& status, const std::string& name) {
    std::istringstream lines(status);
    for (std::string line; std::getline(lines, line);) {
      if (line.starts_with(name + ":")) {
        return line.substr(line.find_first_not_of(" \t", name.size() + 1));
      }
    }
    return {};
  }

  void checkClean(const std::string& status) {
    TEST_CHECK(std::strtoull(field(status, "SigIgn").c_str(), nullptr, 16) == 0);
    TEST_CHECK(std::strtoull(field(status, "SigBlk").c_str(), nullptr, 16) == 0);
  }

} // namespace

int main() {
  using namespace std::chrono_literals;
  sys::ignoreSigpipe();
  ::signal(SIGHUP, SIG_IGN);
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGUSR1);
  ::sigprocmask(SIG_BLOCK, &blocked, nullptr);

  {
    const pid_t pid = ::fork();
    TEST_CHECK(pid >= 0);
    if (pid == 0) {
      sys::resetChildSignals();
      struct sigaction pipeAction{};
      ::sigaction(SIGPIPE, nullptr, &pipeAction);
      sigset_t mask;
      ::sigprocmask(SIG_SETMASK, nullptr, &mask);
      ::_exit(pipeAction.sa_handler == SIG_DFL && sigismember(&mask, SIGUSR1) == 0 ? 0 : 1);
    }
    int status = 0;
    TEST_CHECK(::waitpid(pid, &status, 0) == pid);
    TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }

  {
    int out[2];
    TEST_CHECK(::pipe(out) == 0);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sys::resetChildSignals(&attr);
    char* argv[] = {const_cast<char*>("/bin/cat"), const_cast<char*>("/proc/self/status"), nullptr};
    pid_t pid = -1;
    TEST_CHECK(::posix_spawn(&pid, argv[0], &actions, &attr, argv, environ) == 0);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    std::string status;
    char buf[4096];
    for (long n; (n = ::read(out[0], buf, sizeof buf)) > 0;) status.append(buf, static_cast<std::size_t>(n));
    ::close(out[0]);
    TEST_CHECK(::waitpid(pid, nullptr, 0) == pid);
    checkClean(status);
  }

  {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("child_signals_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path copy = dir / "status";
    TEST_CHECK(sys::spawnDetached("/bin/cp", {"/proc/self/status", copy.string()}));
    TEST_CHECK(!sys::spawnDetached("/nonexistent/program", {}));
    std::string status;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (status.find("\nCapInh:") == std::string::npos && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(10ms);
      std::ifstream in(copy);
      status.assign(std::istreambuf_iterator<char>(in), {});
    }
    std::filesystem::remove_all(dir);
    checkClean(status);
    TEST_CHECK(std::stoi(field(status, "PPid")) != ::getpid());
    TEST_CHECK(::waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
  }
  return 0;
}
