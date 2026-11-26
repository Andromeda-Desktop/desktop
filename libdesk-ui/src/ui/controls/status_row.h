#pragma once

#include "ui/controls/flex.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Box;
class Button;
class Glyph;
class Input;
class InputArea;
class Label;
class Spinner;

// A two-line sidebar row (rulebook §9.7 session row): symbol 16, name over a status line, trailing time.
// It grows with the text and never clips; with no status line the name centres on the symbol.
class StatusRow final : public Flex {
public:
  struct Data {
    std::string symbol;
    bool symbolDanger = false; // palette red: the only colour in the row
    bool spinning = false;     // a spinner replaces the symbol: work runs
    std::string name;
    std::string status;        // empty: one line
    std::string time;          // tabular digits
    bool unseen = false;       // name and time 600, label_1
    bool selected = false;     // label_1 content; the tint behind it is the list's (SelectionList)
    std::optional<ColorSpec> chip; // a raised chip of this fill with a stroke (the focused row of a group), name 600
  };

  explicit StatusRow(float scale);

  void setData(const Data& data);
  void setTime(const std::string& time);
  void setOnClick(std::function<void()> callback);
  // A secondary click, at its scene position (the row's context menu).
  void setOnSecondaryClick(std::function<void(float sceneX, float sceneY)> callback);
  // A left-button drag past a few px: Move per motion, then Drop on release (no click). The row dims while it drags.
  enum class DragPhase : std::uint8_t { Move, Drop, Cancel };
  // Ends a drag in progress with Cancel (Escape); the release that follows neither drops nor clicks.
  void cancelDrag();
  void setOnDrag(std::function<void(DragPhase phase, float sceneX, float sceneY)> callback) { m_onDrag = std::move(callback); }
  // The name becomes a field holding `name`; done gets the new text on Return or focus loss, nullopt on Escape.
  // Focus renameArea() after this call.
  void beginRename(const std::string& name, std::function<void(std::optional<std::string>)> done);
  [[nodiscard]] InputArea* renameArea() const noexcept;
  // Trailing icon buttons in place of the time while the pointer is over the row; none in the rail.
  struct Action {
    std::string glyph;
    std::string tooltip;
    std::function<void()> onClick;
  };
  void setActions(std::vector<Action> actions);
  // An icon rail (narrow window): the symbol alone, centred; the name becomes its tooltip.
  void setRail(bool rail);
  // A row that never has a status line (Settings in a status list): compact one-line height 32, not 48.
  void setOneLine(bool oneLine);

protected:
  void doLayout(Renderer& renderer) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  void applyState();
  [[nodiscard]] bool hot() const;
  void placeArea();
  void endRename(std::optional<std::string> name);
  enum class Mode : std::uint8_t { Display, Renaming };

  float m_scale;
  Data m_data;
  Box* m_shadow = nullptr; // E1 under the chip
  Glyph* m_symbol = nullptr;
  Spinner* m_spinner = nullptr;
  Label* m_name = nullptr;
  Input* m_field = nullptr;
  Mode m_mode = Mode::Display;
  std::function<void(std::optional<std::string>)> m_renameDone;
  std::function<void(float, float)> m_onSecondaryClick;
  std::function<void(DragPhase, float, float)> m_onDrag;
  // Dropped: the release ended a drag; its click is not a click.
  enum class Drag : std::uint8_t { Idle, Pressed, Dragging, Cancelled, Dropped };
  Drag m_drag = Drag::Idle;
  float m_pressX = 0.0F; // scene point of the left press
  float m_pressY = 0.0F;
  Label* m_status = nullptr;
  Label* m_time = nullptr;
  Node* m_text = nullptr;
  bool m_rail = false;
  InputArea* m_area = nullptr;
  Flex* m_actions = nullptr;
  std::vector<Button*> m_actionButtons;
  std::function<void()> m_onClick;
};
