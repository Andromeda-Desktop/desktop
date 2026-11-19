#pragma once

#include "core/animation.h"

#include <string>

extern "C" {
#include <wlr/util/box.h>
}

struct wlr_scene_buffer;
struct wlr_scene_tree;

namespace andromeda::ov {
  class AppIcons;
}

namespace andromeda {

  class Output;
  class Server;

  // Launch feedback (rulebook P15): the app's icon lifts off the dock or launcher, moves toward the middle of the
  // output and breathes there until the app's first window maps; then it grows and fades into it.
  class LaunchFeedback : public Animatable {
  public:
    // `icons` outlives the feedback. `from` and `area` are layout boxes: the launching icon and the output's usable
    // area. The icon shows once `icons` has drawn it at the largest size it reaches on this output.
    LaunchFeedback(
        Server& server, Output* output, wlr_scene_tree* parent, ov::AppIcons& icons, std::string appId,
        const wlr_box& from, const wlr_box& area
    );
    ~LaunchFeedback() override;
    LaunchFeedback(const LaunchFeedback&) = delete;
    LaunchFeedback& operator=(const LaunchFeedback&) = delete;

    // The window mapped (or the launch expired): grow and fade out.
    void finish();
    [[nodiscard]] bool finished() const;
    // The icon's box now, in layout coordinates: the window that replaces it grows from there.
    [[nodiscard]] wlr_box box() const;

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override { return !finished(); }
    [[nodiscard]] bool animatesOn(const Output* output) const override { return output == m_output; }

  private:
    void apply(double nowMsec);
    [[nodiscard]] wlr_box boxAt(double scale) const;

    Server* m_server = nullptr;
    Output* m_output = nullptr;
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_buffer* m_node = nullptr;
    ov::AppIcons* m_icons = nullptr;
    std::string m_appId;
    int m_iconPx = 0; // device pixels
    wlr_box m_from{};
    double m_targetX = 0.0; // centre the icon travels to, layout coordinates
    double m_targetY = 0.0;
    AnimatedValue m_travel{0.0}; // 0 at the icon, 1 at the target
    AnimatedValue m_alpha{0.0};
    AnimatedValue m_exit{0.0}; // 0 while waiting, 1 when grown into the window
    double m_breathStartMsec = -1.0;
    bool m_finishing = false;
    double m_scale = 1.0; // the icon's scale at the last apply
  };

} // namespace andromeda
