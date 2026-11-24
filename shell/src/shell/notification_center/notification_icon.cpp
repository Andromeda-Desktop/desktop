#include "shell/notification_center/notification_icon.h"

#include "dbus/mpris/mpris_art.h"
#include "net/uri.h"
#include "notification/notification.h"
#include "render/core/texture_manager.h"
#include "system/icon_resolver.h"

NotificationIconResult loadNotificationIcon(
    Image& image, Renderer& renderer, const NotificationIconSource& source, IconResolver* resolver,
    const ImageRequest& request, const NotificationIconRemote* remote
) {
  constexpr std::string_view kGlyphPrefix = "noctalia-glyph:";
  const bool glyph = source.icon.starts_with(kGlyphPrefix);
  NotificationIconResult result{.glyph = glyph ? std::string(source.icon.substr(kGlyphPrefix.size())) : "bell"};

  const NotificationImageData* raw = source.raw;
  if (raw != nullptr && raw->width > 0 && raw->height > 0 && !raw->data.empty() && raw->bitsPerSample == 8
      && (raw->channels == 3 || raw->channels == 4)) {
    result.loaded = image.setSourceRaw(
        renderer, raw->data.data(), raw->data.size(), raw->width, raw->height, raw->rowStride,
        raw->channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA, true
    );
  }
  for (const std::string_view name : {glyph ? std::string_view{} : source.icon, source.desktopEntry}) {
    if (result.loaded || name.empty()) {
      continue;
    }
    std::string path;
    if (uri::isRemoteUrl(name)) {
      if (remote == nullptr) {
        continue;
      }
      path = mpris::resolveArtworkSource(remote->http, *remote->pending, name, remote->onReady, remote->lifetime);
      result.pending = result.pending || path.empty();
    } else {
      path = uri::normalizeFileUrl(name);
      if (path.empty() || path.front() != '/') {
        if (resolver == nullptr) {
          continue;
        }
        path = resolver->resolve(path, request.targetSize);
      }
    }
    result.loaded = !path.empty() && image.load(renderer, path, request) != ImageLoad::Failed;
  }
  return result;
}
