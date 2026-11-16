#pragma once

#include "ui/controls/flex.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class Glyph;
class Input;
class InputArea;
class Label;

// A two-line sidebar row (rulebook §9.7 session row): symbol 16, name over a status line, trailing time.
// It grows with the text and never clips; with no status line the name centres on the symbol.
class StatusRow final : public Flex {
public:
  struct Data {
    std::string symbol;
    bool symbolDanger = false; // palette red: the only colour in the row
    std::string name;
    std::string status;        // empty: one line
    std::string time;          // tabular digits
    bool unseen = false;       // name and time 600, label_1
    bool selected = false;     // label_1 content; the tint behind it is the list's (SelectionList)
  };

  explicit StatusRow(float scale);

  void setData(const Data& data);
  void setTime(const std::string& time);
  void setOnClick(std::function<void()> callback);
  // A secondary click, at its scene position (the row's context menu).
  void setOnSecondaryClick(std::function<void(float sceneX, float sceneY)> callback);
  // The name becomes a field holding `name`; done gets the new text on Return or focus loss, nullopt on Escape.
  // Focus renameArea() after this call.
  void beginRename(const std::string& name, std::function<void(std::optional<std::string>)> done);
  [[nodiscard]] InputArea* renameArea() const noexcept;
  // An icon rail (narrow window): the symbol alone, centred; the name becomes its tooltip.
  void setRail(bool rail);

protected:
  void doLayout(Renderer& renderer) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  void applyState();
  void placeArea();
  void endRename(std::optional<std::string> name);
  enum class Mode : std::uint8_t { Display, Renaming };

  float m_scale;
  Data m_data;
  Glyph* m_symbol = nullptr;
  Label* m_name = nullptr;
  Input* m_field = nullptr;
  Mode m_mode = Mode::Display;
  std::function<void(std::optional<std::string>)> m_renameDone;
  std::function<void(float, float)> m_onSecondaryClick;
  Label* m_status = nullptr;
  Label* m_time = nullptr;
  Node* m_text = nullptr;
  bool m_rail = false;
  InputArea* m_area = nullptr;
  std::function<void()> m_onClick;
};
