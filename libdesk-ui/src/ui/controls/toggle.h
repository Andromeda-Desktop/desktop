#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <functional>
#include <optional>
#include <string>

enum class ToggleSize : std::uint8_t {
  Small,
  Medium,
  Large,
};

class InputArea;

class Toggle : public Flex {
public:
  struct Colors {
    ColorRole trackOff;
    ColorRole trackOn;
    ColorRole thumbOff;
    ColorRole thumbOn;
  };

  Toggle();

  void setChecked(bool checked);
  void setCheckedImmediate(bool checked);
  void setEnabled(bool enabled);
  void setToggleSize(ToggleSize size);
  void setScale(float scale);
  // Replaces the fill2/accent track and on_accent knob; the checked border takes trackOn.
  void setColors(const Colors& colors);
  void setOnChange(std::function<void(bool)> callback);
  void setTabFocusKey(std::string key);
  [[nodiscard]] bool hovered() const noexcept;
  [[nodiscard]] bool pressed() const noexcept;
  [[nodiscard]] bool checked() const noexcept { return m_checked; }
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  // Flips the switch as a click would; does nothing while disabled.
  void activate();
  [[nodiscard]] ToggleSize toggleSize() const noexcept { return m_size; }

private:
  void activateFromInput();
  void applySize();
  void applyState();
  void applyAnimatedState(float t);
  void onPaletteToneChanged() override { applyAnimatedState(m_animationProgress); }
  void updatePressWiden();
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  class RectNode* m_thumbShadow = nullptr; // E1 under the knob
  class RectNode* m_thumb = nullptr;
  InputArea* m_inputArea = nullptr;
  std::uint32_t m_animId = 0;
  std::uint32_t m_colorAnimId = 0;
  std::uint32_t m_widenAnimId = 0;
  float m_colorProgress = 0.0F;
  float m_widen = 0.0F;
  std::function<void(bool)> m_onChange;
  ToggleSize m_size = ToggleSize::Medium;
  bool m_checked = false;
  bool m_enabled = true;
  float m_inset = 0.0F;
  float m_travel = 0.0F;
  float m_thumbSize = 0.0F;
  float m_scale = 1.0F;
  float m_animationProgress = 0.0F;
  std::optional<Colors> m_colors;
  Signal<>::ScopedConnection m_paletteConn;
};
