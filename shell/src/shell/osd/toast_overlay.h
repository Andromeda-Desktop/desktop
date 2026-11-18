#pragma once

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "ui/signal.h"
#include "wayland/glass_surface.h"
#include "wayland/layer_surface.h"

#include <cstdint>
#include <memory>
#include <string>

class CompositorPlatform;
class ConfigService;
class Glyph;
class IpcService;
class Label;
class Node;
class RenderContext;
class WaylandConnection;

// A short notice in a glass capsule centred above the output bottom (concept C s-toast): it opens with scale .92 -> 1
// and a fade, stays component.toast.timeout_ms, then fades out. A new toast while one shows replaces its text and
// restarts the timeout; a toast during the fade-out fades back in.
class ToastOverlay {
public:
  enum class Phase : std::uint8_t { Hidden, Shown, Hiding };

  void initialize(WaylandConnection& wayland, CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext);
  void registerIpc(IpcService& ipc);
  void show(std::string text);
  [[nodiscard]] Phase phase() const noexcept { return m_phase; }

private:
  void createSurface();
  void destroySurface();
  void prepareFrame();
  void startHide();
  [[nodiscard]] float uiScale() const;

  WaylandConnection* m_wayland = nullptr;
  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  Phase m_phase = Phase::Hidden;
  std::string m_text;
  bool m_textChanged = false;
  Timer m_timeout;
  // Destroyed in reverse order: the scene before the animations (~Node cancels its animations), the surface last.
  std::unique_ptr<LayerSurface> m_surface;
  GlassSurface m_glass;
  AnimationManager m_animations;
  std::unique_ptr<Node> m_root;
  Signal<>::ScopedConnection m_glassConn;
  Node* m_capsule = nullptr;
  Label* m_label = nullptr;
  Glyph* m_glyph = nullptr;
};
