#include "ui/app_icon.h"

#include "render/core/renderer.h"
#include "ui/controls/image.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <vector>

namespace fs = std::filesystem;

std::string iconFileFor(const std::string& icon) {
  if (icon.empty()) {
    return {};
  }
  std::error_code ec;
  if (icon.starts_with('/')) {
    return fs::exists(icon, ec) ? icon : std::string();
  }
  // The sizes a 24-48 px row looks sharp with at 1x-2x, then any scalable art, then pixmaps.
  std::vector<fs::path> roots;
  if (const char* home = g_get_user_data_dir()) {
    roots.emplace_back(fs::path(home) / "icons");
  }
  for (const gchar* const* dir = g_get_system_data_dirs(); dir != nullptr && *dir != nullptr; ++dir) {
    roots.emplace_back(fs::path(*dir) / "icons");
  }
  static constexpr std::array kSizes{"scalable", "96x96", "64x64", "128x128", "48x48", "256x256", "32x32"};
  for (const fs::path& root : roots) {
    for (const char* size : kSizes) {
      for (const char* ext : {".svg", ".png"}) {
        const fs::path file = root / "hicolor" / size / "apps" / (icon + ext);
        if (fs::exists(file, ec)) {
          return file.string();
        }
      }
    }
  }
  for (const char* ext : {".svg", ".png", ".xpm"}) {
    const fs::path file = fs::path("/usr/share/pixmaps") / (icon + ext);
    if (fs::exists(file, ec)) {
      return file.string();
    }
  }
  return {};
}

std::string appInfoIconFile(GAppInfo* info) {
  GIcon* icon = g_app_info_get_icon(info);
  if (icon == nullptr) {
    return {};
  }
  if (G_IS_FILE_ICON(icon)) {
    gchar* path = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon)));
    std::string out = path != nullptr ? path : "";
    g_free(path);
    return out;
  }
  if (G_IS_THEMED_ICON(icon)) {
    for (const gchar* const* name = g_themed_icon_get_names(G_THEMED_ICON(icon)); name != nullptr && *name != nullptr;
         ++name) {
      if (std::string file = iconFileFor(*name); !file.empty()) {
        return file;
      }
    }
  }
  return {};
}

std::string appIconFile(const std::string& appId) {
  if (appId.empty()) {
    return {};
  }
  GDesktopAppInfo* info = g_desktop_app_info_new((appId + ".desktop").c_str());
  if (info == nullptr) {
    return iconFileFor(appId);
  }
  std::string file = appInfoIconFile(G_APP_INFO(info));
  g_object_unref(info);
  return file;
}

AppIconNode::AppIconNode(std::string path, float size) : m_path(std::move(path)), m_size(size) {
  m_image = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_image->setSize(size, size);
  setSize(size, size);
}

void AppIconNode::doLayout(Renderer& renderer) {
  const int px = static_cast<int>(std::ceil(m_size * renderer.renderScale()));
  if (px != m_loadedPx && !m_path.empty()) {
    m_image->setSourceFile(renderer, m_path, px);
    m_loadedPx = px;
  }
  m_image->setSize(m_size, m_size);
  m_image->layout(renderer);
}
