#include "core/sys/sys.h"
#include "core/sys/poll.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <iterator>

namespace sys {

  int pollFds(pollfd* fds, nfds_t count, int timeoutMs) { return ::poll(fds, count, timeoutMs); }

  void resetChildSignals() {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (int sig = 1; sig < NSIG; ++sig) {
      ::sigaction(sig, &action, nullptr);
    }
    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
  }

  void resetChildSignals(posix_spawnattr_t* attr) {
    short flags = 0;
    posix_spawnattr_getflags(attr, &flags);
    posix_spawnattr_setflags(attr, static_cast<short>(flags | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK));
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGKILL);
    sigdelset(&all, SIGSTOP);
    posix_spawnattr_setsigdefault(attr, &all);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(attr, &none);
  }

  void ignoreSigpipe() { ::signal(SIGPIPE, SIG_IGN); }

  int spawnInGroup(const std::string& program, const std::vector<std::string>& argv,
                   const std::vector<std::string>& env, std::array<int, 3> stdio) {
    std::vector<char*> args;
    for (const std::string& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    std::vector<char*> envp;
    for (const std::string& entry : env) envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd = 0; fd < 3; ++fd) posix_spawn_file_actions_adddup2(&actions, stdio[fd], fd);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    resetChildSignals(&attr);
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, program.c_str(), &actions, &attr, args.data(), envp.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    return rc == 0 ? pid : -rc;
  }

  std::vector<std::string> shellArgv(std::string command, std::vector<std::string> args) {
    std::vector<std::string> argv{"/bin/sh", "-c", std::move(command)};
    if (!args.empty()) {
      argv.emplace_back("sh");
      std::ranges::move(args, std::back_inserter(argv));
    }
    return argv;
  }

  bool spawnDetached(const std::string& program, const std::vector<std::string>& args) {
    std::vector<char*> argv{const_cast<char*>(program.c_str())};
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    int report[2];
    if (!pipeCloexec(report)) return false;
    const pid_t child = ::fork();
    if (child == 0) {
      // Async-signal-safe calls only until exec: the fork copied locks that other threads may hold.
      ::close(report[0]);
      const pid_t grandchild = ::setsid() < 0 ? -1 : ::fork();
      if (grandchild != 0) {
        const int error = errno;
        if (grandchild < 0) (void)!::write(report[1], &error, sizeof error);
        ::_exit(0);
      }
      resetChildSignals();
      const int null = ::open("/dev/null", O_RDWR);
      for (const int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) ::dup2(null, fd);
      if (null > STDERR_FILENO) ::close(null);
      ::execv(argv[0], argv.data());
      const int error = errno;
      (void)!::write(report[1], &error, sizeof error);
      ::_exit(127);
    }
    ::close(report[1]);
    if (child < 0) {
      ::close(report[0]);
      return false;
    }
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
    // End of file without an errno: the program runs (the write end closed on exec).
    int error = 0;
    long got = 0;
    while ((got = ::read(report[0], &error, sizeof error)) < 0 && errno == EINTR) {
    }
    ::close(report[0]);
    return got == 0;
  }

} // namespace sys
