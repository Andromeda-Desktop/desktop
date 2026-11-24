#include "net/uri.h"

namespace uri {

  bool isRemoteUrl(std::string_view url) { return url.starts_with("https://") || url.starts_with("http://"); }

  std::string normalizeFileUrl(std::string_view url) {
    if (url.empty() || isRemoteUrl(url)) {
      return {};
    }
    if (url.starts_with("file://")) {
      return pathFromFileUri(url).value_or(std::string{});
    }
    return std::string(url);
  }

} // namespace uri
