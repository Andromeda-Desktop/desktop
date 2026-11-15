#pragma once

#include "render/scene/node.h"

#include <string>

class Image;
using GAppInfo = struct _GAppInfo;

// A GIO icon name or path as a file under the icon theme dirs (hicolor, pixmaps), or "".
[[nodiscard]] std::string iconFileFor(const std::string& icon);
// The icon file of an installed app, or "".
[[nodiscard]] std::string appInfoIconFile(GAppInfo* info);
// The icon file of the app with this desktop id (no ".desktop" suffix), or "".
[[nodiscard]] std::string appIconFile(const std::string& appId);

// An app icon from a file path: it loads at the first layout, when a renderer is there, and reloads on a scale change.
class AppIconNode final : public Node {
public:
  AppIconNode(std::string path, float size);

protected:
  void doLayout(Renderer& renderer) override;

private:
  std::string m_path;
  float m_size;
  Image* m_image = nullptr;
  int m_loadedPx = 0;
};
