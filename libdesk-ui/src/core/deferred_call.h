#pragma once

#include <functional>
#include <memory>
#include <vector>

/// Schedule a callback to run at the top of the next main loop iteration.
/// Safe to call from animation callbacks, event handlers, or anywhere else
/// where destroying the caller would be unsafe.
class DeferredCall {
public:
  static void callLater(std::function<void()> fn);
  // Runs `fn` on the main loop only while `alive` (the owner's token) still lives.
  static void callLaterIfAlive(std::weak_ptr<const void> alive, std::function<void()> fn) {
    callLater([alive = std::move(alive), fn = std::move(fn)] {
      if (!alive.expired()) {
        fn();
      }
    });
  }
  static std::vector<std::function<void()>> takePending();
  static void drainWakeFd();
  [[nodiscard]] static int wakeFd();

private:
  friend class MainLoop;
  static std::vector<std::function<void()>>& queue();
};
