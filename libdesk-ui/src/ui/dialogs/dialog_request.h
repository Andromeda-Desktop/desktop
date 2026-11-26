#pragma once

#include "core/deferred_call.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

// The one pending request of a picker facade: its options and its callback. A request is pending while it holds a
// callback; a new open ends the old one with no result.
template <class Options, class Result>
class DialogRequest {
public:
  using Callback = std::function<void(std::optional<Result>)>;
  // Deferred: settle and cancel hand the result over on the next loop turn, not inside the caller's stack.
  enum class Delivery : std::uint8_t { Now, Deferred };

  explicit DialogRequest(Delivery delivery = Delivery::Now) : m_delivery(delivery) {}

  // Ends a pending request with no result, then stores this one and calls `present`. When `present` fails, the
  // request ends at once with no result and open returns false.
  template <class Present>
  bool open(Options options, Callback callback, Present&& present) {
    if (m_callback) {
      std::exchange(m_callback, {})(std::nullopt);
    }
    m_options = std::move(options);
    m_callback = std::move(callback);
    if (!std::forward<Present>(present)()) {
      if (auto pending = take()) {
        pending(std::nullopt);
      }
      return false;
    }
    return true;
  }

  // Ends the request now and returns the call that hands `result` to its callback.
  [[nodiscard]] std::function<void()> settle(std::optional<Result> result) {
    return [callback = take(), result = std::move(result), delivery = m_delivery]() mutable {
      deliver(delivery, std::move(callback), std::move(result));
    };
  }

  void cancel() {
    if (m_callback) {
      deliver(m_delivery, take(), std::nullopt);
    }
  }

  [[nodiscard]] const Options& options() const noexcept { return m_options; }

private:
  Callback take() {
    m_options = {};
    return std::exchange(m_callback, {});
  }

  static void deliver(Delivery delivery, Callback callback, std::optional<Result> result) {
    if (!callback) {
      return;
    }
    if (delivery == Delivery::Deferred) {
      DeferredCall::callLater([callback = std::move(callback), result = std::move(result)] { callback(result); });
    } else {
      callback(std::move(result));
    }
  }

  Options m_options;
  Callback m_callback;
  Delivery m_delivery;
};
