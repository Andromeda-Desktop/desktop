#pragma once

#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

namespace compositors::andromeda {

  class AndromedaRuntime;

  // Base for objects that consume Andromeda event-stream messages. Handlers
  // register themselves with the runtime on construction and unregister on
  // destruction. The socket owner (AndromedaWorkspaceBackend) reads the stream
  // and fans each parsed event out through the runtime to every registered
  // handler.
  class AndromedaEventHandler {
  public:
    // `families` are the subscription event names the handler consumes; the runtime subscribes to their union.
    AndromedaEventHandler(AndromedaRuntime& runtime, std::vector<std::string_view> families);
    virtual ~AndromedaEventHandler();

    AndromedaEventHandler(const AndromedaEventHandler&) = delete;
    AndromedaEventHandler& operator=(const AndromedaEventHandler&) = delete;

    [[nodiscard]] const std::vector<std::string_view>& families() const noexcept { return m_families; }

    virtual void handleEvent(std::string_view event, const nlohmann::json& data) = 0;
    // Called when the runtime tears the event stream down (e.g. cleanup): the
    // handler's cached state should be considered stale.
    virtual void handleStreamReset() {}

  protected:
    AndromedaRuntime& m_runtime;

  private:
    std::vector<std::string_view> m_families;
  };

} // namespace compositors::andromeda
