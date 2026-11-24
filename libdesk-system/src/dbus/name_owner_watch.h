#pragma once

#include <functional>
#include <memory>
#include <string>

namespace sdbus {
  class IConnection;
}

// The one follower of bus name owners (C13): a NameOwnerChanged match filtered by the bus daemon (arg0 or
// arg0namespace), so the process wakes only for the names it follows. A one-name watch also asks GetNameOwner at
// start, so an owner that came before the match is reported too. Destroying the watch stops every callback.
// Blocks for one bus round trip; a caller on the main thread prefers NameOwnerWatch::name.
[[nodiscard]] bool nameHasOwner(sdbus::IConnection& bus, const std::string& name);

class NameOwnerWatch {
public:
  // The NameOwnerChanged arguments; an owner is a unique name, "" when the name has none.
  using Callback = std::function<void(const std::string& name, const std::string& oldOwner, const std::string& newOwner)>;

  NameOwnerWatch();
  NameOwnerWatch(NameOwnerWatch&&) noexcept;
  NameOwnerWatch& operator=(NameOwnerWatch&&) noexcept;
  ~NameOwnerWatch();

  [[nodiscard]] explicit operator bool() const noexcept { return m_state != nullptr; }

  // One name: first its owner at start as ("", owner), also when it has none (old == new == ""), then every
  // change; no other report has old == new.
  [[nodiscard]] static NameOwnerWatch name(sdbus::IConnection& bus, std::string name, Callback callback);
  // One name: changes only.
  [[nodiscard]] static NameOwnerWatch changes(sdbus::IConnection& bus, std::string name, Callback callback);
  // Every name in a dot-separated namespace (the prefix itself included): changes only.
  [[nodiscard]] static NameOwnerWatch prefix(sdbus::IConnection& bus, std::string prefix, Callback callback);
  // Every name, unique names included: changes only.
  [[nodiscard]] static NameOwnerWatch all(sdbus::IConnection& bus, Callback callback);

  struct State;

private:
  std::unique_ptr<State> m_state;
};
