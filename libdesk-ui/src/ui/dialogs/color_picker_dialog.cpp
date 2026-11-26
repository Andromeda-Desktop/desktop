#include "ui/dialogs/color_picker_dialog.h"

#include "i18n/i18n.h"
#include "ui/dialogs/dialog_request.h"

#include <utility>

namespace {

  DialogRequest<ColorPickerDialogOptions, Color> s_request{DialogRequest<ColorPickerDialogOptions, Color>::Delivery::Deferred};
  ColorPickerDialogPresenter* s_presenter = nullptr;
  std::optional<Color> s_lastResult;

} // namespace

void ColorPickerDialog::setPresenter(ColorPickerDialogPresenter* presenter) noexcept { s_presenter = presenter; }

bool ColorPickerDialog::open(ColorPickerDialogOptions options, CompletionCallback callback) {
  if (options.title.empty()) {
    options.title = i18n::tr("ui.dialogs.color-picker.title");
  }
  return s_request.open(std::move(options), std::move(callback), [] {
    return s_presenter != nullptr && s_presenter->openColorPicker();
  });
}

void ColorPickerDialog::complete(const Color& result) { settle(result)(); }

std::function<void()> ColorPickerDialog::settle(const Color& result) {
  s_lastResult = result;
  return s_request.settle(result);
}

void ColorPickerDialog::cancelIfPending() { s_request.cancel(); }

const ColorPickerDialogOptions& ColorPickerDialog::currentOptions() { return s_request.options(); }

std::optional<Color> ColorPickerDialog::lastResult() noexcept { return s_lastResult; }
