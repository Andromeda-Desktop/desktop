#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace andromeda {

  // What a client connection may bind among the privileged globals (capture, clipboard managers, input injection,
  // window lists, output control, the shell and lock protocols), decided once per connection from its binary.
  // Hygiene, not a boundary: a same-uid process can defeat any identity; sandboxed clients never get a grant.
  enum class Grant : uint8_t { None, Shell, Greeter, Portal, Link, User, Test };

  [[nodiscard]] bool privilegedGlobal(std::string_view interface);

  // A non-privileged global is always allowed.
  [[nodiscard]] bool grantAllows(Grant grant, std::string_view interface);

  struct TrustInputs {
    // The client's binary, " (deleted)" already stripped; empty when the identity could not be read.
    std::string_view exe;
    bool greeterMode = false;
    // Binaries the user granted in Settings > Privacy.
    std::span<const std::string> userBinaries;
    // `--trust-binary` paths; one ending in '/' trusts every binary under it.
    std::span<const std::string> testBinaries;
  };

  [[nodiscard]] Grant grantFor(const TrustInputs& in);

  // `andromeda --trust-binary <path>`: dev and test runs grant their own builds and tools; never config.
  void addTestBinary(std::string path);
  [[nodiscard]] std::span<const std::string> testBinaries();

  // /proc/<pid>/exe text of a process whose binary was replaced or deleted ends in " (deleted)".
  [[nodiscard]] std::string_view stripDeleted(std::string_view exe);

} // namespace andromeda
