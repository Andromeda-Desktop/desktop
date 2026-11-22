#include "ui/dialogs/color_picker_dialog_popup.h"

#include "core/input/keybind_matcher.h"
#include "core/deferred_call.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/controls/color_picker.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "platform/input_events.h"

#include <algorithm>
#include <memory>

ColorPickerDialogPopup::~ColorPickerDialogPopup() { destroyPopup(); }

void ColorPickerDialogPopup::initialize(
    Display& display, const UiSettings& settings, RenderContext& renderContext, LayerPopupHostRegistry& popupHosts
) {
  initializeBase(display, settings, renderContext, popupHosts);
}

bool ColorPickerDialogPopup::openColorPicker() {
  const float scale = uiScale();
  const auto width = static_cast<std::uint32_t>(ColorPickerSheet::preferredDialogWidth(scale));
  const auto height =
      static_cast<std::uint32_t>(ColorPickerSheet::preferredDialogHeight(static_cast<float>(width), scale));
  return openPopup(width, height);
}

void ColorPickerDialogPopup::closeColorPickerWithoutResult() { destroyPopup(); }

bool ColorPickerDialogPopup::onPointerEvent(const PointerEvent& event) {
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    if (m_selectPopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      m_selectPopup->closeSelectDropdown();
      return true;
    }
  }
  return DialogPopupHost::onPointerEvent(event);
}

void ColorPickerDialogPopup::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->onKeyboardEvent(event);
    return;
  }
  DialogPopupHost::onKeyboardEvent(event);
}

void ColorPickerDialogPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
  auto sheet = std::make_unique<ColorPickerSheet>(uiScale());
  sheet->setTitle(ColorPickerDialog::currentOptions().title);
  if (const auto& initialColor = ColorPickerDialog::currentOptions().initialColor; initialColor.has_value()) {
    sheet->colorPicker()->setColor(*initialColor);
  } else {
    sheet->colorPicker()->setColor(colorForRole(ColorRole::Accent));
  }
  sheet->setOnCancel([this] { deferWhileAlive([this] { cancel(); }); });
  sheet->setOnApply([this](const Color& color) { deferWhileAlive([this, color] { accept(color); }); });
  m_sheet = sheet.get();
  contentParent->addChild(std::move(sheet));

  if (display() != nullptr && renderContext() != nullptr && surface() != nullptr) {
    if (m_selectPopup == nullptr) {
      m_selectPopup = std::make_unique<SelectDropdownPopup>(*display(), *renderContext());
    }
    m_selectPopup->setShadowConfig(shadowStyle());
    m_selectPopup->setParent(surface(), OutputId::None);
    contentParent->setPopupContext(m_selectPopup.get());
  }
}

void ColorPickerDialogPopup::layoutSheet(float contentWidth, float contentHeight) {
  if (m_sheet == nullptr) {
    return;
  }
  m_sheet->setPickerColumnWidth(std::max(160.0F, contentWidth));
  m_sheet->setSize(contentWidth, contentHeight);
  m_sheet->layout(renderer());
}

void ColorPickerDialogPopup::cancelToFacade() { ColorPickerDialog::cancelIfPending(); }

InputArea* ColorPickerDialogPopup::initialFocusArea() {
  return m_sheet != nullptr ? m_sheet->initialFocusArea() : nullptr;
}

void ColorPickerDialogPopup::onSheetClose() {
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->closeSelectDropdown();
  }
}

// Enter applies (rulebook 9.10: Enter = default); a focused hex or number field keeps Enter to commit its text.
bool ColorPickerDialogPopup::preDispatchKeyboard(const KeyboardEvent& event) {
  if (m_sheet == nullptr || !event.pressed || event.preedit
      || !KeybindMatcher::matches(KeybindAction::Validate, event.sym, event.modifiers)) {
    return false;
  }
  const InputArea* focused = inputDispatcher().focusedArea();
  if (focused != nullptr && focused->textInputClient() != nullptr) {
    return false;
  }
  m_sheet->apply();
  return true;
}

void ColorPickerDialogPopup::accept(const Color& result) {
  closeAfterAccept(ColorPickerDialog::settle(result));
}
