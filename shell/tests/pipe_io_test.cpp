// A pipe read ends at its total deadline even while the writer keeps it busy, fails past its byte cap, and tells an
// empty transfer from a failed one.
#include "tests/test_check.h"
#include "wayland/pipe_io.h"

#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <unistd.h>

int main() {
  using namespace std::chrono_literals;
  // The timed-out read closes its end while the writer still writes.
  std::signal(SIGPIPE, SIG_IGN);
  {
    int fds[2];
    TEST_CHECK(pipe(fds) == 0);
    std::thread writer([fd = fds[1]] {
      for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(100ms);
        if (write(fd, "x", 1) != 1) {
          break;
        }
      }
      close(fd);
    });
    const auto start = std::chrono::steady_clock::now();
    const pipe_io::ReadResult read = pipe_io::readPipe(fds[0], static_cast<std::size_t>(-1), 350ms);
    const auto took = std::chrono::steady_clock::now() - start;
    writer.join();
    TEST_CHECK(read.error == pipe_io::ReadError::TimedOut);
    TEST_CHECK(read.bytes.empty());
    TEST_CHECK(took < 800ms);
  }
  {
    int fds[2];
    TEST_CHECK(pipe(fds) == 0);
    TEST_CHECK(write(fds[1], "abcdef", 6) == 6);
    close(fds[1]);
    const pipe_io::ReadResult read = pipe_io::readPipe(fds[0], 4);
    TEST_CHECK(read.error == pipe_io::ReadError::TooLarge && read.bytes.empty());
  }
  {
    int fds[2];
    TEST_CHECK(pipe(fds) == 0);
    close(fds[1]);
    const pipe_io::ReadResult read = pipe_io::readPipe(fds[0]);
    TEST_CHECK(read.ok() && read.bytes.empty());
  }
  {
    int fds[2];
    TEST_CHECK(pipe(fds) == 0);
    TEST_CHECK(write(fds[1], "text", 4) == 4);
    close(fds[1]);
    const pipe_io::ReadResult read = pipe_io::readPipe(fds[0]);
    TEST_CHECK(read.ok() && read.bytes == "text");
  }
  return 0;
}
