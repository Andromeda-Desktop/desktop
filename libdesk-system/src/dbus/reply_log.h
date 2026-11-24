#pragma once

#include "core/log.h"

#include <optional>
#include <sdbus-c++/Error.h>
#include <string>
#include <utility>

// The reply handler for an async call whose result nothing reads: a failure is logged, never dropped (C13). Teardown
// calls (Release, Cancel, Unsubscribe) fail when the peer is already gone, so they log at debug level.
enum class ReplyFailure : bool { Warn, Debug };

[[nodiscard]] inline auto logReply(const Logger& log, std::string method, ReplyFailure level = ReplyFailure::Warn) {
  return [&log, method = std::move(method), level](std::optional<sdbus::Error> error) {
    if (!error.has_value()) {
      return;
    }
    if (level == ReplyFailure::Debug) {
      log.debug("{} failed: {}", method, error->getMessage());
    } else {
      log.warn("{} failed: {}", method, error->getMessage());
    }
  };
}
