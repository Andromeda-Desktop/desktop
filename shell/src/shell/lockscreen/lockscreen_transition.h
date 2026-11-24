#pragma once

#include "render/core/texture_handle.h"
#include "render/scene/node.h"

#include <cstdint>

class TextureShaderNode;

enum class LockscreenTransitionKind : std::uint8_t {
  Fade = 0,
  Wipe = 1,
  Disc = 2,
  Stripes = 3,
  Zoom = 4,
  Honeycomb = 5,
  Vortex = 6,
  Pixel = 7,
  Diamond = 8,
  Golden = 9,
  Melt = 10,
};

struct LockscreenTransitionParams {
  float direction = 0.0F;
  float centerX = 0.5F;
  float centerY = 0.5F;
  float stripeCount = 12.0F;
  float angle = 30.0F;
  float cellSize = 0.04F;
  float smoothness = 0.3F;
  float aspectRatio = 1.777F;
};

// The desktop capture over the lock screen, uncovered by one of the transition shaders as progress goes 0 -> 1.
class LockscreenTransitionCover final : public Node {
public:
  LockscreenTransitionCover();

  void setTexture(TextureId texture);
  void setTransition(LockscreenTransitionKind transition, float progress, const LockscreenTransitionParams& params);
  void setSize(float width, float height) override;
  void setFrameSize(float width, float height);

private:
  TextureShaderNode* m_shader = nullptr;
};
