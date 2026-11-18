#include "net/url_open.h"

#include "core/log.h"

#include <gio/gio.h>

namespace {
  constexpr Logger kLog("url-open");
}

namespace net {

  bool openInBrowser(const std::string& url, const std::string& activationToken) {
    GAppLaunchContext* context = g_app_launch_context_new();
    if (!activationToken.empty()) {
      g_app_launch_context_setenv(context, "XDG_ACTIVATION_TOKEN", activationToken.c_str());
    }

    GError* error = nullptr;
    const bool launched = g_app_info_launch_default_for_uri(url.c_str(), context, &error) == TRUE;
    if (!launched) {
      kLog.warn("failed to open URL '{}': {}", url, error != nullptr ? error->message : "no default handler");
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    g_object_unref(context);
    return launched;
  }

  bool openFile(const std::string& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (uri == nullptr) {
      kLog.warn("failed to open '{}': not an absolute path", path);
      return false;
    }
    const bool opened = openInBrowser(uri);
    g_free(uri);
    return opened;
  }

} // namespace net
