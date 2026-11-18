#pragma once

#include "ui/shadow_style.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/select_popup_context.h"

#include <cstdint>

class RenderContext;
class Display;
struct KeyboardEvent;
struct PointerEvent;

// A Select's dropdown is a context menu: this adapter maps DropdownRequest onto the shared
// ContextMenuPopup / ContextMenuControl stack so rows, scrolling, and keyboard navigation have a
// single implementation. Owners hold one instance per surface and route events into it.
class SelectDropdownPopup : public SelectPopupContext {
public:
  SelectDropdownPopup(Display& display, RenderContext& renderContext);
  ~SelectDropdownPopup() override;

  void setParent(Surface* parent, OutputId output);
  void setShadowConfig(const ShadowStyle& shadow);

  void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override;
  void closeSelectDropdown() override;
  [[nodiscard]] bool isSelectDropdownOpen() const override;

  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

  [[nodiscard]] Surface* surface() const noexcept;
  [[nodiscard]] std::uint32_t popupWidth() const noexcept;
  [[nodiscard]] std::uint32_t popupHeight() const noexcept;

private:
  ContextMenuPopup m_popup;
  Surface* m_parent = nullptr;
  OutputId m_parentOutput = OutputId::None;
};
