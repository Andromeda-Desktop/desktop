#pragma once

#include <string>

namespace net {

  // Open a URL in the user's default browser via GIO's app-info registry. Returns true if the launch succeeded.
  bool openInBrowser(const std::string& url, const std::string& activationToken = {});
  // Opens a local file in the default app for its type.
  bool openFile(const std::string& path);

} // namespace net
