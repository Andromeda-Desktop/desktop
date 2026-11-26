#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>
#include <wayland-server-core.h>

namespace test {

  inline wl_event_loop* eventLoop() {
    static wl_event_loop* const loop = wl_event_loop_create();
    return loop;
  }

  // Dispatches the event loop until `ready()` holds; throws after 10 s.
  template <class Ready> void pumpUntil(Ready ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready()) {
      if (std::chrono::steady_clock::now() > deadline) {
        throw std::runtime_error("timed out waiting for the event loop");
      }
      wl_event_loop_dispatch(eventLoop(), 100);
    }
  }

  // Starts an asynchronous call with a `done(Result)` callback and returns the result it delivers on the loop.
  template <class Result, class Start> Result await(Start start) {
    std::optional<Result> out;
    start([&out](Result result) { out.emplace(std::move(result)); });
    pumpUntil([&out] { return out.has_value(); });
    return std::move(*out);
  }

} // namespace test
