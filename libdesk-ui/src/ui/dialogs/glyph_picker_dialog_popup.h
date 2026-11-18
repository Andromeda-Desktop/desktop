#pragma once

#include "ui/dialogs/dialog_popup_host.h"
#include "ui/dialogs/glyph_picker_dialog.h"

class GlyphPicker;
class SelectDropdownPopup;
struct KeyboardEvent;
struct PointerEvent;
class Surface;

class GlyphPickerDialogPopup final : public DialogPopupHost, public GlyphPickerDialogPresenter {
public:
  ~GlyphPickerDialogPopup() override;

  void initialize(
      Display& display, const UiSettings& settings, RenderContext& renderContext,
      LayerPopupHostRegistry& popupHosts
  );

  [[nodiscard]] bool openGlyphPicker() override;
  void closeGlyphPickerWithoutResult() override;
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] bool ownsSelectDropdownSurface(const Surface* surface) const noexcept;

protected:
  void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
  void layoutSheet(float contentWidth, float contentHeight) override;
  void cancelToFacade() override;
  [[nodiscard]] InputArea* initialFocusArea() override;
  void onSheetClose() override;

private:
  void accept(const GlyphPickerResult& result);

  GlyphPicker* m_sheet = nullptr;
  std::unique_ptr<SelectDropdownPopup> m_selectPopup;
};
