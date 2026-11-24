#include "wayland/pipe_io.h"

#include "core/blocking_pool.h"
#include "core/deferred_call.h"

#include <cerrno>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace pipe_io {
ReadResult readPipe(int fd, std::size_t maxBytes,
                    std::chrono::milliseconds deadline) {
  ReadResult result;
  const auto until = std::chrono::steady_clock::now() + deadline;
  char buf[4096];
  while (true) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        until - std::chrono::steady_clock::now());
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    const int ready =
        left.count() > 0 ? ::poll(&pfd, 1, static_cast<int>(left.count())) : 0;
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0) {
      result.error = ready == 0 ? ReadError::TimedOut : ReadError::Failed;
      break;
    }
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      result.error = ReadError::Failed;
      break;
    }
    if (n == 0) {
      break;
    }
    result.bytes.append(buf, static_cast<std::size_t>(n));
    if (result.bytes.size() > maxBytes) {
      result.error = ReadError::TooLarge;
      break;
    }
  }
  if (!result.ok()) {
    result.bytes.clear();
  }
  ::close(fd);
  return result;
}

void readPipeAsync(int fd, std::size_t maxBytes, std::weak_ptr<void> alive,
                   std::function<void(ReadResult)> done) {
  BlockingPool::instance().run([fd, maxBytes, alive = std::move(alive),
               done = std::move(done)]() mutable {
    DeferredCall::callLater(
        [alive, done, result = readPipe(fd, maxBytes)]() mutable {
          if (!alive.expired()) {
            done(std::move(result));
          }
        });
  });
}

void writeAll(int fd, const std::string &bytes) {
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    done += static_cast<std::size_t>(n);
  }
}
} // namespace pipe_io
