#pragma once

#include "ui/controls/image.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

class HttpClient;
class IconResolver;
class Renderer;
struct NotificationImageData;

// What a notification gives for its icon, in the order every surface tries it.
struct NotificationIconSource {
  const NotificationImageData* raw = nullptr;
  std::string_view icon;
  std::string_view desktopEntry;
};

// The media art download cache, for a surface that shows remote icons.
struct NotificationIconRemote {
  HttpClient* http = nullptr;
  std::unordered_set<std::string>* pending = nullptr;
  std::function<void()> onReady;
  std::weak_ptr<void> lifetime;
};

struct NotificationIconResult {
  bool loaded = false;
  bool pending = false; // a remote icon is still downloading: try again when it lands
  std::string glyph;    // the fallback glyph: `noctalia-glyph:<name>` (as Link sends) or "bell"
};

// Loads the first usable source into `image`: raw pixels, then the icon field, then the desktop entry (a path, a file
// URI, a remote URL when `remote` is given, a theme icon name at request.targetSize when `resolver` is given).
NotificationIconResult loadNotificationIcon(
    Image& image, Renderer& renderer, const NotificationIconSource& source, IconResolver* resolver,
    const ImageRequest& request, const NotificationIconRemote* remote = nullptr
);
