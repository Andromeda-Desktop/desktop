#pragma once

#include <memory>

class InputArea;
class Node;
class Renderer;
struct KeyboardEvent;

namespace term {

  class AgentView;
  class TerminalView;

  // The content plane of one session in the terminal window: a terminal grid or an agent session log.
  // All methods run on the UI thread.
  class ContentView {
  public:
    virtual ~ContentView() = default;
    // uiScale: logical px per design px; deviceScale: buffer px per logical px. The GL context is current.
    virtual std::unique_ptr<Node> build(Renderer& renderer, float uiScale, float deviceScale, float width,
                                        float height) = 0;
    virtual void setSize(float width, float height) = 0;
    // Once per frame tick, with the GL context current.
    virtual void frame() = 0;
    // Its nodes were freed with the window's scene.
    virtual void sceneDestroyed() = 0;
    virtual void setDeviceScale(float deviceScale) = 0;
    // false when the window should handle the key.
    virtual bool key(const KeyboardEvent& event) = 0;
    virtual void setWindowActive(bool active) = 0;
    [[nodiscard]] virtual InputArea* focusArea() const noexcept = 0;
    // Non-null for a terminal session: find and reattach apply only to it.
    [[nodiscard]] virtual TerminalView* terminal() noexcept { return nullptr; }
    [[nodiscard]] virtual AgentView* agent() noexcept { return nullptr; }
  };

} // namespace term
