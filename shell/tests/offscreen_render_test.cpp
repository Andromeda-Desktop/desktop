#include "render/core/color.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/image_node.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "tests/test_check.h"

#include <cstdint>
#include <memory>
#include <vector>

int main() {
  constexpr std::uint32_t kSize = 16;
  GlSharedContext shared;
  shared.initializeSurfaceless();
  RenderContext context;
  context.initialize(shared);

  RenderTarget target;
  target.createOffscreen(context);
  target.setContentScale(1.0F);
  target.setLogicalSize(kSize, kSize);
  target.resize(kSize, kSize);

  Node root;
  root.setSize(kSize, kSize);
  auto rect = std::make_unique<RectNode>();
  rect->setSize(kSize, kSize / 2);
  rect->setStyle({.fill = rgba(1.0F, 0.0F, 0.0F), .noAa = true});
  root.addChild(std::move(rect));

  std::vector<std::uint8_t> pixels(kSize * kSize * 4);
  TEST_CHECK(!context.readPixels(target, pixels));
  TEST_CHECK(context.renderScene(target, &root));
  TEST_CHECK(context.readPixels(target, pixels));

  const auto at = [&pixels](std::uint32_t x, std::uint32_t y) { return &pixels[(y * kSize + x) * 4]; };
  // The rect fills the top half: rows come top-down.
  TEST_CHECK(at(4, 2)[0] == 255 && at(4, 2)[1] == 0 && at(4, 2)[3] == 255);
  TEST_CHECK(at(4, 12)[3] == 0);
  target.destroy();

  // An image at a fractional origin draws on whole buffer pixels: no edge pixel is partly covered.
  constexpr std::uint32_t kScaledSize = 20;
  constexpr int kImageSide = 10;
  RenderTarget scaled;
  scaled.createOffscreen(context);
  scaled.setContentScale(1.25F);
  scaled.setLogicalSize(16, 16);
  scaled.resize(kScaledSize, kScaledSize);
  const std::vector<std::uint8_t> white(static_cast<std::size_t>(kImageSide * kImageSide * 4), 255);
  const TextureHandle texture = context.textureManager().loadFromRgba(white.data(), kImageSide, kImageSide);
  TEST_CHECK(texture.id != 0);
  Node scaledRoot;
  scaledRoot.setSize(16, 16);
  auto image = std::make_unique<ImageNode>();
  image->setPosition(3.3F, 3.3F);
  image->setFrameSize(8.0F, 8.0F);
  image->setTextureSize(kImageSide, kImageSide);
  image->setTextureId(texture.id);
  scaledRoot.addChild(std::move(image));
  std::vector<std::uint8_t> scaledPixels(kScaledSize * kScaledSize * 4);
  TEST_CHECK(context.renderScene(scaled, &scaledRoot));
  TEST_CHECK(context.readPixels(scaled, scaledPixels));
  int partial = 0;
  for (std::size_t i = 3; i < scaledPixels.size(); i += 4) {
    partial += scaledPixels[i] != 0 && scaledPixels[i] != 255 ? 1 : 0;
  }
  TEST_CHECK(partial == 0);
  scaled.destroy();
  context.cleanup();
  shared.cleanup();
  return 0;
}
