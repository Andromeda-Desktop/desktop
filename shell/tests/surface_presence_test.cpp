// Model test for the frame host on presence: a fake platform surface with a looping animation (a spinner) gets
// random presence changes, frame callbacks (the compositor still sends them while hidden), tick requests and drains.
// Invariants: no animation step runs while the presence is not Shown; a surface that becomes Shown steps its
// spinner on the next frame. The fake has no GPU, so its configure redraw stays requested and every frame renders;
// "no callback requests while hidden and unchanged" needs a real present and is not checked here.

#include "platform/surface.h"
#include "render/animation/animation_manager.h"
#include "render/backend/native_egl_window.h"

#include <array>
#include <cstdint>
#include <memory>
#include <print>
#include <random>

namespace {
  int gFailures = 0;

  void expect(bool condition, const char* message, unsigned seed, int step) {
    if (!condition) {
      std::println(stderr, "surface_presence_test: seed {} step {}: {}", seed, step, message);
      ++gFailures;
    }
  }

  class FakeSurface final : public Surface {
  public:
    bool initialize() override {
      setRunning(true);
      onConfigure(64, 64);
      return true;
    }
    void setInputRegion(const std::vector<InputRect>& /*rects*/) override {}
    void setOpaqueRegion(const std::vector<InputRect>& /*rects*/) override {}
    void setBlurRegion(const std::vector<InputRect>& /*rects*/) override {}
    void setOpaque(bool /*opaque*/) override {}
    void clearBlurRegion() override {}

    void present(SurfacePresence presence) { setPresence(presence); }
    // The compositor sends the done event for a requested callback, shown or not.
    void deliverFrame() {
      if (!m_pending) {
        return;
      }
      m_pending = false;
      m_clockMs += 16;
      frameDone(m_clockMs);
    }
    [[nodiscard]] bool pending() const noexcept { return m_pending; }

  protected:
    // A present: the commit carries the next frame callback.
    void render() override { requestFrame(); }
    [[nodiscard]] bool hasNativeSurface() const noexcept override { return true; }
    [[nodiscard]] std::unique_ptr<NativeEglWindow> createNativeWindow() override { return nullptr; }
    [[nodiscard]] bool frameCallbackPending() const noexcept override { return m_pending; }
    void requestFrameCallback() override {
      m_pending = true;
    }
    void cancelFrameCallback() override { m_pending = false; }
    void commitNative() override {}
    [[nodiscard]] bool scalesByViewport() const noexcept override { return false; }
    void applyNativeScale() override {}

  private:
    bool m_pending = false;
    std::uint32_t m_clockMs = 0;
  };

  void drain() {
    for (int i = 0; i < 4 && (Surface::hasPendingFrameWork() || Surface::hasPendingRenders()); ++i) {
      Surface::drainPendingFrameWork();
      Surface::drainPendingRenders();
    }
  }

  enum class Input : std::uint8_t { Present, Frame, TickRequest, Drain };
} // namespace

int main() {
  constexpr std::array kPresences{
      SurfacePresence::Shown, SurfacePresence::Occluded, SurfacePresence::Hidden, SurfacePresence::Off
  };
  constexpr std::array kInputs{Input::Present, Input::Frame, Input::Frame, Input::TickRequest, Input::Drain};
  for (unsigned seed = 1; seed <= 32; ++seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> pickInput(0, kInputs.size() - 1);
    std::uniform_int_distribution<std::size_t> pickPresence(0, kPresences.size() - 1);

    AnimationManager animations;
    FakeSurface surface;
    surface.setAnimationManager(&animations);
    int step = 0;
    int steps = 0;
    int stepsWhileNotShown = 0;
    std::function<void()> spin = [&] {
      animations.animateTimer(0.0F, 1.0F, 100.0F, Easing::Linear, [&](float /*value*/) {
        ++steps;
        if (surface.presence() != SurfacePresence::Shown) {
          ++stepsWhileNotShown;
        }
      }, [&] { spin(); });
    };
    surface.initialize();
    spin();
    surface.requestFrameTick();
    drain();

    for (; step < 300; ++step) {
      const bool wasShown = surface.presence() == SurfacePresence::Shown;
      switch (kInputs[pickInput(rng)]) {
      case Input::Present:
        surface.present(kPresences[pickPresence(rng)]);
        break;
      case Input::Frame:
        surface.deliverFrame();
        break;
      case Input::TickRequest:
        surface.requestFrameTick();
        break;
      case Input::Drain:
        drain();
        break;
      }
      drain();
      expect(stepsWhileNotShown == 0, "an animation stepped while not shown", seed, step);
      if (!wasShown && surface.presence() == SurfacePresence::Shown) {
        const int before = steps;
        surface.deliverFrame();
        drain();
        expect(steps > before, "a spinner did not resume when shown", seed, step);
      }
    }
  }
  return gFailures == 0 ? 0 : 1;
}
