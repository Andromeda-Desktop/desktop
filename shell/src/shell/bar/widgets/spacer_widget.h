#pragma once

#include "shell/bar/widget.h"

class Box;

class SpacerWidget : public Widget {
public:
  struct Options {
    int length = 20;
    // Draws the island divider (a 1 px label_3 line) instead of empty space; the bar keeps its gaps around it.
    bool line = false;
  };

  SpacerWidget(bool verticalBar, Options options);

  void create() override;

  bool noGapAroundMe() const noexcept override { return m_line == nullptr; }
  bool isBarClickThrough() const noexcept override { return true; }

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  float m_fixedLength;
  bool m_drawLine = false;
  Box* m_line = nullptr;
  bool m_verticalBar = false;
};
