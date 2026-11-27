#include "core/deferred_call.h"
#include "core/sys/sys.h"
#include "protocols/async_file_writer.h"

#include <mutex>

namespace {

  // Every process that runs DeferredCall gets file-write completions on its main loop.
  [[maybe_unused]] const bool kFileWriteDonePoster =
      (andromeda::setFileWriteDonePoster([](std::function<void()> fn) { DeferredCall::callLater(std::move(fn)); }), true);

  // Never destroyed: worker threads may post while static destructors run at exit.
  std::mutex& deferredMutex() {
    static auto* mutex = new std::mutex;
    return *mutex;
  }

  const sys::WakeFd& wake() {
    static const auto* wake = new sys::WakeFd;
    return *wake;
  }

} // namespace

std::vector<std::function<void()>>& DeferredCall::queue() {
  static auto* q = new std::vector<std::function<void()>>;
  return *q;
}

void DeferredCall::callLater(std::function<void()> fn) {
  {
    std::scoped_lock lock(deferredMutex());
    queue().push_back(std::move(fn));
  }
  (void)wake().signal();
}

std::vector<std::function<void()>> DeferredCall::takePending() {
  std::scoped_lock lock(deferredMutex());
  auto pending = std::move(queue());
  queue().clear();
  return pending;
}

void DeferredCall::drainWakeFd() { wake().drain(); }

int DeferredCall::wakeFd() { return wake().pollFd(); }
