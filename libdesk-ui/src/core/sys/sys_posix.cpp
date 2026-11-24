#include "core/sys/sys.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sys {

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
