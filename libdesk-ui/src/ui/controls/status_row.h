#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <string>

class Glyph;
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
    bool selected = false;     // accent tint, label_1
  };

  explicit StatusRow(float scale);

  void setData(const Data& data);
  void setTime(const std::string& time);
  void setOnClick(std::function<void()> callback);

protected:
  void doLayout(Renderer& renderer) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  void applyState();
  void placeArea();

  float m_scale;
  Data m_data;
  Glyph* m_symbol = nullptr;
  Label* m_name = nullptr;
  Label* m_status = nullptr;
  Label* m_time = nullptr;
  InputArea* m_area = nullptr;
  std::function<void()> m_onClick;
};
