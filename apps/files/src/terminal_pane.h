#pragma once

#include "terminal_view.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AppLoop;
class AppWindow;
class Flex;
class InputArea;
class Node;
class Renderer;
struct KeyboardEvent;

// Dolphin-style terminal pane (F4, mockup files-v2-live.html §16, FILES-UI-SPEC §18): the user's shell under the view,
// drawn by the same TerminalView as Andromeda Terminal. The shell follows the folder (a " cd" while no job runs) and
// the folder follows the shell's cd (OSC 7 from the shell integration). A header strip above the shells resizes the pane
// by drag and holds Split and Close; splits sit side by side, and the active one (last clicked) follows the folder.
class TerminalPane {
public:
  struct Host {
    std::function<void(const std::filesystem::path&)> cwdChanged;
    std::function<void()> exited; // the shell ended (exit, Ctrl+D)
    std::function<void(InputArea*)> focus;
    std::function<void(float)> resizing; // logical px, while the header is dragged
    std::function<void(float)> resized;  // logical px, when the drag ends
  };

  // `height` in logical px; build() clamps it to the room it is given.
  TerminalPane(AppLoop& loop, AppWindow& window, const std::filesystem::path& dir, float height, Host host);
  ~TerminalPane();
  TerminalPane(const TerminalPane&) = delete;
  TerminalPane& operator=(const TerminalPane&) = delete;

  [[nodiscard]] bool running() const noexcept { return !m_splits.empty(); }
  [[nodiscard]] float height() const noexcept { return m_height; }
  // maxHeight: logical px the window can give the pane.
  [[nodiscard]] std::unique_ptr<Node> build(Renderer& renderer, float scale, float width, float maxHeight);
  // A new shell beside the active one, in its folder.
  void split();
  // Ends the active shell; ending the last one ends the pane (Host::exited).
  void closeActive();
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

  static constexpr float kDefaultHeight = 248.0F;
  static constexpr float kMinHeight = 120.0F;
  static constexpr std::size_t kMaxSplits = 4;

private:
  struct Split {
    std::uint64_t id;
    std::unique_ptr<term::TerminalView> view;
    std::filesystem::path cwd;
    Flex* card = nullptr; // set by build(), null while the pane is out of the scene
  };
  struct Drag {
    float startSceneY;
    float startHeight;
  };

  bool addSplit(const std::filesystem::path& dir, std::size_t at);
  void removeSplit(std::uint64_t id);
  [[nodiscard]] Split* splitOf(std::uint64_t id);
  [[nodiscard]] Split& active() { return m_splits[m_active]; }
  void resizeTo(float height);
  void requestFrame();

  AppLoop& m_loop;
  AppWindow& m_window;
  Host m_host;
  std::vector<Split> m_splits;
  // Ended splits whose nodes are still in the scene, with their retire number: a build frees only the ones retired
  // before it; their input callbacks reach the views until then.
  std::vector<std::pair<std::uint64_t, std::unique_ptr<term::TerminalView>>> m_retired;
  std::uint64_t m_retireSeq = 0;
  std::size_t m_active = 0;
  std::uint64_t m_lastId = 0;
  float m_height;
  float m_maxHeight;
  float m_scale = 1.0F;
  float m_cardWidth = 0.0F;
  Flex* m_outer = nullptr;
  Flex* m_cardsRow = nullptr;
  std::optional<Drag> m_drag;
  bool m_built = false;
  bool m_focusWanted = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
