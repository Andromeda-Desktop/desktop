#pragma once

#include "core/timer_manager.h"
#include "platform/output_id.h"
#include "render/animation/animation_manager.h"
#include "ui/tooltip_content.h"
#include "ui/signal.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

class InputArea;
class Node;
class Display;
class Popup;
class RenderContext;
class Renderer;
class Surface;

class TooltipManager {
public:
  static TooltipManager& instance();

  // `uiScale` gives the current ui scale (accessibility); null = 1.
  void initialize(Display& display, std::function<float()> uiScale, RenderContext* renderContext);
  void shutdown();
  // Destroy any live tooltip immediately. Required before destroying a parent
  // layer surface — xdg_popup must die before its parent (Jay enforces this).
  void forceDestroy();

  // origin: the window geometry origin in scene coordinates (a shadow margin), subtracted from anchors.
  void onHoverChange(InputArea* area, Surface* parent, OutputId output, float originX = 0.0F, float originY = 0.0F);
  void onBarHoverChange(InputArea* area, Surface* parent, OutputId output);
  void syncAnchor(InputArea* area);

  void suppressBarTooltipsForPanel(std::string_view panelId);
  void restoreBarTooltipsForPanel(std::string_view panelId);

private:
  TooltipManager() = default;

  enum class State { Idle, Pending, Showing, FadingOut };

  struct Size {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
  };

  void showPopup();
  void handleHoverChange(InputArea* area);
  void scheduleRetargetPopup();
  void scheduleReshow();
  void dismissPopup();
  void scheduleDestroyPopup();
  void destroyPopup();
  [[nodiscard]] bool canRetargetPopup() const;
  void refreshFromArea(InputArea* area);
  void refreshPopupContent();
  void scheduleProviderRefresh();
  Size measureContent(Renderer& renderer, const TooltipContent& content);
  // Configured render scale of m_pendingOutput, for pre-surface measurement.
  [[nodiscard]] float pendingOutputScale() const;
  void buildScene(const TooltipContent& content, float w, float h, float opacity = 0.0F);
  void prepareFrame(bool needsUpdate, bool needsLayout);

  Display* m_display = nullptr;
  std::function<float()> m_uiScale;
  RenderContext* m_renderContext = nullptr;

  State m_state = State::Idle;
  bool m_reshowQueued = false;
  bool m_retargetQueued = false;
  bool m_destroyScheduled = false;
  bool m_showAfterDestroy = false;
  Timer m_showTimer;
  Timer m_refreshTimer;

  std::unordered_set<std::string> m_suppressedBarTooltipPanels;

  TooltipContent m_pendingContent;
  // What the current scene shows: a provider refresh with the same content skips the rebuild and redraw.
  TooltipContent m_builtContent;
  InputArea* m_builtArea = nullptr;
  Surface* m_pendingParent = nullptr;
  float m_originX = 0.0F;
  float m_originY = 0.0F;
  OutputId m_pendingOutput = OutputId::None;
  InputArea* m_pendingArea = nullptr;

  Surface* m_activeParent = nullptr;
  OutputId m_activeOutput = OutputId::None;

  std::unique_ptr<Popup> m_surface;
  AnimationManager m_animations;
  std::unique_ptr<Node> m_sceneRoot;
  AnimationManager::Id m_fadeAnimId = 0;

  Signal<>::ScopedConnection m_paletteConn;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
