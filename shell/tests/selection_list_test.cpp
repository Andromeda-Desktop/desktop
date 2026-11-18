// A removed row closes its gap from the first frame, so the rows below never wait for its fade.
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "tests/test_check.h"
#include "ui/controls/selection_list.h"

#include <chrono>
#include <cstdlib>
#include <memory>

std::unique_ptr<TextureManager> createDefaultTextureManager() { return nullptr; }

namespace {

  class StubRenderer : public Renderer {
  public:
    TextMetrics measureText(std::string_view, float fontSize, const TextFont&, float, int, TextAlign, TextEllipsize, bool)
        override {
      return TextMetrics{.bottom = fontSize};
    }
    TextMetrics measureFont(float fontSize, const TextFont&) override { return TextMetrics{.bottom = fontSize}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextMetrics measureGlyph(char32_t, float fontSize) override { return TextMetrics{.bottom = fontSize}; }
    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  Flex* addRow(SelectionList& list) {
    auto row = std::make_unique<Flex>();
    row->setMinHeight(48.0F);
    auto* raw = row.get();
    list.addChild(std::move(row));
    return raw;
  }

  void rowsBelowMoveOnTheFirstFrames(StubRenderer& renderer) {
    MotionService::instance().setEnabled(true);
    MotionService::instance().setSpeed(1.0F);
    AnimationManager animations;
    SelectionList list(1.0F);
    list.setAnimationManager(&animations);
    auto* first = addRow(list);
    addRow(list);
    auto* third = addRow(list);
    list.setSize(240.0F, 400.0F);
    list.layout(renderer);
    const float start = third->y();

    bool freed = false;
    list.removeAnimated(first, [&freed] { freed = true; });
    const auto t0 = std::chrono::steady_clock::now();
    for (int frame = 1; frame <= 3; ++frame) {
      animations.tickAt(t0 + std::chrono::milliseconds(16 * frame));
      list.layout(renderer);
    }
    TEST_CHECK(third->y() < start - 8.0F);

    for (int frame = 4; frame <= 120 && !freed; ++frame) {
      animations.tickAt(t0 + std::chrono::milliseconds(16 * frame));
      list.layout(renderer);
    }
    TEST_CHECK(freed);
    TEST_CHECK(third->y() == start - 48.0F);
  }

} // namespace

int main() {
  StubRenderer renderer;
  rowsBelowMoveOnTheFirstFrames(renderer);
  return EXIT_SUCCESS;
}
