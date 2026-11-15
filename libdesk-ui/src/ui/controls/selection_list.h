#pragma once

#include "render/animation/animation_manager.h"
#include "ui/controls/flex.h"

#include <cstddef>
#include <functional>
#include <memory>

class Box;

// A vertical list with one accent tint behind the selected child (rulebook §9.7). On a change the tint travels: its
// leading edge on spring.snap, its trailing edge on spring.trail; `onCross` runs once at half the travel, where the
// label weight crosses to the new row (motion P1).
class SelectionList final : public Flex {
public:
  explicit SelectionList(float scale);

  // nullptr hides the tint. `animate` false jumps (first build, reduced motion is handled by the springs).
  void setSelected(Node* child, bool animate, std::function<void()> onCross = {});

  // Motion P13: the row opens from height 0 to `height` on spring.move, its content fades in from content_gate of the
  // travel; at rest it takes its own height again (XL text grows it). `index` counts rows, not the tint.
  void insertAnimated(std::unique_ptr<Flex> row, std::size_t index, float height);
  // The row fades out on spring.fade_out, then its height closes on spring.move; `beforeFree` runs after it left the
  // scene and before it is freed (drop input focus and hover held by its areas).
  void removeAnimated(Flex* row, std::function<void()> beforeFree);

protected:
  void doLayout(Renderer& renderer) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  void place();
  void apply();

  float m_scale;
  Box* m_tint = nullptr;
  Node* m_selected = nullptr;
  Node* m_inserting = nullptr; // the row that is opening, and its rest height
  float m_insertHeight = 0.0F;
  float m_top = 0.0F;
  float m_bottom = 0.0F;
  bool m_travelling = false;
  bool m_pendingTravel = false;
  std::function<void()> m_onCross;
  AnimationManager::Id m_topAnim = 0;
  AnimationManager::Id m_bottomAnim = 0;
};
