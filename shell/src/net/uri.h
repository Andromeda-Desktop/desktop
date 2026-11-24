#pragma once

#include "protocols/uri.h"

#include <string>
#include <string_view>

namespace uri {

  [[nodiscard]] bool isRemoteUrl(std::string_view url);
  // The local path of a file URI, or `url` itself when it has no scheme; empty for a remote URL or a remote host.
  [[nodiscard]] std::string normalizeFileUrl(std::string_view url);

} // namespace uri
