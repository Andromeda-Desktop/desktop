#pragma once

#include "render/core/texture_handle.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

class TextureManager;

struct LoadedImageFile {
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;
  // Decoded dimensions before any crop/resize (SVG: rasterized size).
  int sourceWidth = 0;
  int sourceHeight = 0;
};

[[nodiscard]] std::expected<LoadedImageFile, std::string>
loadImageFile(const std::string& path, int targetSize = 0, bool centerSquareCrop = false);

// Decodes and uploads an image file; an invalid handle when it does not load. A free function, not a TextureManager
// virtual, so binaries that never load images link no decoders.
[[nodiscard]] TextureHandle
loadTextureFile(TextureManager& textures, const std::string& path, int targetSize = 0, bool mipmap = false);

// Approximate Gaussian blur (three box passes) of a tightly packed RGBA buffer, in place.
void blurRgba(std::vector<std::uint8_t>& rgba, int width, int height, int radius);

// 90th-percentile relative luminance (WCAG) of an RGBA buffer, sampled over about 64k pixels; -1 when empty.
[[nodiscard]] float rgbaBrightLuminance(const std::vector<std::uint8_t>& rgba);
