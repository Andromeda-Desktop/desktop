// Model test: a damage repaint (repaint passes, occlusion cull, rounded-clip stencil skip) must give the same pixels
// as a full repaint of the same scene, for random scenes and random edits.
#include "render/core/color.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <print>
#include <random>
#include <vector>

namespace {

  constexpr std::uint32_t kSize = 384;
  constexpr float kS = 4.0F; // scene units to pixels: edits stay small against the target
  constexpr int kSeeds = 800;
  constexpr int kEdits = 6;

  struct Scene {
    Node root;
    std::vector<RectNode*> rects;
    std::vector<Node*> groups;
    // A small rect beside each card, in the card's parent: edits move it onto the card's corners and edges.
    std::vector<std::pair<RectNode*, const RectNode*>> edgeRects;
  };

  float pick(std::mt19937& rng, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
  bool chance(std::mt19937& rng, float p) { return pick(rng, 0.0F, 1.0F) < p; }

  RoundedRectStyle randomStyle(std::mt19937& rng) {
    const bool opaque = chance(rng, 0.6F);
    RoundedRectStyle style{
        .fill = rgba(pick(rng, 0, 1), pick(rng, 0, 1), pick(rng, 0, 1), opaque ? 1.0F : pick(rng, 0.2F, 0.9F)),
        .border = rgba(pick(rng, 0, 1), pick(rng, 0, 1), pick(rng, 0, 1), pick(rng, 0.0F, 1.0F)),
    };
    const float r = chance(rng, 0.5F) ? pick(rng, 0.0F, 24.0F) : 0.0F;
    style.radius = Radii(r, r, r, r);
    style.softness = chance(rng, 0.5F) ? 1.0F : 0.0F;
    style.borderWidth = chance(rng, 0.3F) ? pick(rng, 0.5F, 3.0F) : 0.0F;
    return style;
  }

  void placeRandomly(std::mt19937& rng, Node& node) {
    node.setPosition(kS * pick(rng, -16.0F, 80.0F), kS * pick(rng, -16.0F, 80.0F));
    node.setSize(kS * pick(rng, 8.0F, 96.0F), kS * pick(rng, 8.0F, 96.0F));
  }

  void addRect(std::mt19937& rng, Scene& scene, Node& parent, bool small) {
    auto rect = std::make_unique<RectNode>();
    placeRandomly(rng, *rect);
    if (small) {
      rect->setSize(pick(rng, 3.0F, 32.0F), pick(rng, 3.0F, 32.0F));
    }
    rect->setStyle(randomStyle(rng));
    if (chance(rng, 0.15F)) {
      rect->setOpacity(pick(rng, 0.3F, 1.0F));
    }
    if (small) {
      scene.rects.push_back(rect.get());
    }
    parent.addChild(std::move(rect));
  }

  // An opaque card that fills its parent, as an app pane or a terminal card: the usual occluder.
  void addCard(std::mt19937& rng, Scene& scene, Node& parent) {
    auto card = std::make_unique<RectNode>();
    const float inset = chance(rng, 0.5F) ? 0.0F : pick(rng, 0.0F, std::min(parent.width(), parent.height()) / 4.0F);
    card->setPosition(inset, inset);
    card->setSize(parent.width() - 2.0F * inset, parent.height() - 2.0F * inset);
    RoundedRectStyle style = randomStyle(rng);
    style.fill.a = 1.0F;
    if (chance(rng, 0.4F)) {
      style.radius = Radii(0.0F); // a square card under a rounded group clip needs the clip's stencil
    } else if (chance(rng, 0.5F)) {
      // Whole-node damage carries 16 px of slack: only a large corner leaves a gap a repaint inside the card reaches.
      style.radius = Radii(std::min(pick(rng, 40.0F, 120.0F), std::min(card->width(), card->height()) / 2.0F));
    }
    card->setStyle(style);
    const RectNode* cardNode = card.get();
    parent.addChild(std::move(card));
    auto edge = std::make_unique<RectNode>();
    edge->setSize(pick(rng, 2.0F, 12.0F), pick(rng, 2.0F, 12.0F));
    edge->setStyle(randomStyle(rng));
    scene.edgeRects.emplace_back(edge.get(), cardNode);
    parent.addChild(std::move(edge));
  }

  void build(std::mt19937& rng, Scene& scene) {
    scene.root.setSize(kSize, kSize);
    scene.groups.push_back(&scene.root);
    if (chance(rng, 0.7F)) {
      addCard(rng, scene, scene.root);
    }
    const int groups = std::uniform_int_distribution(0, 3)(rng);
    for (int g = 0; g < groups; ++g) {
      auto group = std::make_unique<Node>();
      placeRandomly(rng, *group);
      if (chance(rng, 0.6F)) {
        group->setClipChildren(true);
      }
      if (chance(rng, 0.5F)) {
        const float r = pick(rng, 2.0F, 20.0F);
        group->setClipRadii(Radii(r, r, r, r));
      }
      if (chance(rng, 0.2F)) {
        group->setOpacity(pick(rng, 0.4F, 1.0F));
      }
      if (chance(rng, 0.7F)) {
        addCard(rng, scene, *group);
      }
      Node* parent = scene.groups[std::uniform_int_distribution<std::size_t>(0, scene.groups.size() - 1)(rng)];
      scene.groups.push_back(group.get());
      parent->addChild(std::move(group));
    }
    const int rects = std::uniform_int_distribution(2, 10)(rng);
    for (int i = 0; i < rects; ++i) {
      Node& parent = *scene.groups[std::uniform_int_distribution<std::size_t>(0, scene.groups.size() - 1)(rng)];
      addRect(rng, scene, parent, chance(rng, 0.7F));
    }
    if (scene.rects.empty()) {
      addRect(rng, scene, scene.root, true);
    }
  }

  // One small edit, so the next frame repaints only part of the target.
  void edit(std::mt19937& rng, Scene& scene) {
    if (!scene.edgeRects.empty() && chance(rng, 0.5F)) {
      const auto [rect, card] =
          scene.edgeRects[std::uniform_int_distribution<std::size_t>(0, scene.edgeRects.size() - 1)(rng)];
      if (chance(rng, 0.5F)) {
        RoundedRectStyle style = randomStyle(rng);
        style.fill.a = pick(rng, 0.2F, 0.9F); // what the card hides must show through
        rect->setStyle(style); // in place: the repaint stays inside the corner
        return;
      }
      const float x = chance(rng, 0.5F) ? card->x() : card->x() + card->width();
      const float y = chance(rng, 0.5F) ? card->y() : card->y() + card->height();
      const bool alongX = chance(rng, 0.3F);
      const bool alongY = !alongX && chance(rng, 0.4F);
      // Mostly inside the corner square, where the arc, the border and the edge antialiasing are.
      const float inX = x == card->x() ? 1.0F : -1.0F;
      const float inY = y == card->y() ? 1.0F : -1.0F;
      const float w = rect->width();
      const float h = rect->height();
      rect->setPosition(alongX ? card->x() + pick(rng, 0.0F, card->width())
                               : x + (inX > 0 ? pick(rng, -4.0F, 40.0F) : -w - pick(rng, -4.0F, 40.0F)),
                        alongY ? card->y() + pick(rng, 0.0F, card->height())
                               : y + (inY > 0 ? pick(rng, -4.0F, 40.0F) : -h - pick(rng, -4.0F, 40.0F)));
      return;
    }
    if (scene.groups.size() > 1 && chance(rng, 0.15F)) {
      // A group moves or resizes: what its clip showed before must repaint too.
      placeRandomly(rng, *scene.groups[std::uniform_int_distribution<std::size_t>(1, scene.groups.size() - 1)(rng)]);
      return;
    }
    RectNode* rect = scene.rects[std::uniform_int_distribution<std::size_t>(0, scene.rects.size() - 1)(rng)];
    switch (std::uniform_int_distribution(0, 4)(rng)) {
    case 0: rect->setPosition(kS * pick(rng, -8.0F, 96.0F), kS * pick(rng, -8.0F, 96.0F)); break;
    case 1: rect->setStyle(randomStyle(rng)); break;
    case 2: rect->setSize(pick(rng, 2.0F, 32.0F), pick(rng, 2.0F, 32.0F)); break;
    case 3: rect->setOpacity(chance(rng, 0.5F) ? 1.0F : pick(rng, 0.2F, 1.0F)); break;
    default: rect->setVisible(!rect->visible()); break;
    }
  }

  std::vector<std::uint8_t> render(RenderContext& context, RenderTarget& target, Node& root) {
    std::vector<std::uint8_t> pixels(kSize * kSize * 4);
    context.renderScene(target, &root);
    root.clearDirty(); // as Surface does after a frame
    TEST_CHECK(context.readPixels(target, pixels));
    return pixels;
  }

  void prepare(RenderContext& context, RenderTarget& target) {
    target.createOffscreen(context);
    target.setContentScale(1.0F);
    target.setLogicalSize(kSize, kSize);
    target.resize(kSize, kSize);
  }

} // namespace

// The same seed and edit count always give the same scene.
std::unique_ptr<Scene> makeScene(int seed, int edits) {
  std::mt19937 rng(static_cast<std::uint32_t>(seed));
  auto scene = std::make_unique<Scene>();
  build(rng, *scene);
  for (int i = 0; i < edits; ++i) {
    edit(rng, *scene);
  }
  return scene;
}

int main() {
  GlSharedContext shared;
  shared.initializeSurfaceless();
  RenderContext context;
  context.initialize(shared);
  // A new target resets GPU resources and damages every node: both targets live for the whole run.
  RenderTarget partial;
  prepare(context, partial);
  RenderTarget full;
  prepare(context, full);

  std::unique_ptr<Scene> reference;
  std::unique_ptr<Scene> current;
  for (int seed = 0; seed < kSeeds; ++seed) {
    std::mt19937 rng(static_cast<std::uint32_t>(seed));
    // The last seed's scene stays alive so this root gets a new address: the first frame draws in full.
    auto fresh = std::make_unique<Scene>();
    build(rng, *fresh);
    current = std::move(fresh);
    Scene& scene = *current;
    render(context, partial, scene.root);
    for (int step = 0; step < kEdits; ++step) {
      edit(rng, scene);
      const std::vector<std::uint8_t> got = render(context, partial, scene.root);
      // A new root draws in full; the old one stays alive so the new one cannot reuse its address.
      std::unique_ptr<Scene> next = makeScene(seed, step + 1);
      const std::vector<std::uint8_t> want = render(context, full, next->root);
      reference = std::move(next);
      for (std::size_t i = 0; i < got.size(); ++i) {
        if (std::abs(int{got[i]} - int{want[i]}) > 1) {
          const std::size_t px = i / 4;
          std::println(stderr, "seed {} edit {}: pixel {},{} channel {}: damage repaint {} vs full {}", seed, step,
                       px % kSize, px / kSize, i % 4, got[i], want[i]);
          TEST_CHECK(false);
        }
      }
    }
  }

  partial.destroy();
  full.destroy();
  context.cleanup();
  shared.cleanup();
}
