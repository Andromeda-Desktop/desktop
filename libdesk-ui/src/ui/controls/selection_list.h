#pragma once

#include "render/animation/animation_manager.h"
#include "ui/controls/flex.h"

#include <functional>

class Box;

// A vertical list with one accent tint behind the selected child (rulebook §9.7). On a change the tint travels: its
// leading edge on spring.snap, its trailing edge on spring.trail; `onCross` runs once at half the travel, where the
// label weight crosses to the new row (motion P1).
class SelectionList final : public Flex {
public:
  explicit SelectionList(float scale);

  // nullptr hides the tint. `animate` false jumps (first build, reduced motion is handled by the springs).
  void setSelected(Node* child, bool animate, std::function<void()> onCross = {});

protected:
  void doLayout(Renderer& renderer) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  void place();
  void apply();

  float m_scale;
  Box* m_tint = nullptr;
  Node* m_selected = nullptr;
  float m_top = 0.0F;
  float m_bottom = 0.0F;
  bool m_travelling = false;
  bool m_pendingTravel = false;
  std::function<void()> m_onCross;
  AnimationManager::Id m_topAnim = 0;
  AnimationManager::Id m_bottomAnim = 0;
};
