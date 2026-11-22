#pragma once

#include "terminal_view.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class AppLoop;
class AppWindow;
class InputArea;
class Node;
class Renderer;
struct KeyboardEvent;

// Dolphin-style terminal pane (F4, mockup files-v2-live.html §16, FILES-UI-SPEC §18): the user's shell under the view,
// drawn by the same TerminalView as Andromeda Terminal. The shell follows the folder (a " cd" while no job runs) and
// the folder follows the shell's cd (OSC 7 from the shell integration).
class TerminalPane {
public:
  struct Host {
    std::function<void(const std::filesystem::path&)> cwdChanged;
    std::function<void()> exited; // the shell ended (exit, Ctrl+D)
    std::function<void(InputArea*)> focus;
  };

  TerminalPane(AppLoop& loop, AppWindow& window, const std::filesystem::path& dir, Host host);
  ~TerminalPane();
  TerminalPane(const TerminalPane&) = delete;
  TerminalPane& operator=(const TerminalPane&) = delete;

  [[nodiscard]] bool running() const noexcept { return m_running; }
  [[nodiscard]] std::unique_ptr<Node> build(Renderer& renderer, float scale, float width);
  void focus();
  // The pane left the scene (F4 hid it): its nodes are gone.
  void detach();
  [[nodiscard]] bool focused() const;
  // A key while the pane has focus; false when the window should handle it.
  bool key(const KeyboardEvent& event);
  // The view moved to `dir`: cd there unless a job runs in the shell or it is there already.
  void followFolder(const std::filesystem::path& dir);
  void sendText(const std::string& text);
  [[nodiscard]] std::string screenText();

  static constexpr float kHeight = 216.0F;

private:
  void requestFrame();

  AppLoop& m_loop;
  AppWindow& m_window;
  Host m_host;
  std::unique_ptr<term::TerminalView> m_view;
  bool m_running = false;
  bool m_built = false;
  bool m_focusWanted = false;
  std::filesystem::path m_cwd;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
