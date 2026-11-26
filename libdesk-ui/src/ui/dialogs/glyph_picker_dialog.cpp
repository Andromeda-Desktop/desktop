#include "ui/dialogs/glyph_picker_dialog.h"

#include "i18n/i18n.h"
#include "ui/dialogs/dialog_request.h"

#include <utility>

namespace {

  DialogRequest<GlyphPickerDialogOptions, GlyphPickerResult> s_request;
  GlyphPickerDialogPresenter* s_presenter = nullptr;
  std::optional<GlyphPickerResult> s_lastResult;

} // namespace

void GlyphPickerDialog::setPresenter(GlyphPickerDialogPresenter* presenter) noexcept { s_presenter = presenter; }

bool GlyphPickerDialog::open(GlyphPickerDialogOptions options, CompletionCallback callback) {
  if (options.title.empty()) {
    options.title = i18n::tr("ui.dialogs.glyph-picker.title");
  }
  return s_request.open(std::move(options), std::move(callback), [] {
    return s_presenter != nullptr && s_presenter->openGlyphPicker();
  });
}

void GlyphPickerDialog::complete(const GlyphPickerResult& result) { settle(result)(); }

std::function<void()> GlyphPickerDialog::settle(const GlyphPickerResult& result) {
  s_lastResult = result;
  return s_request.settle(result);
}

void GlyphPickerDialog::cancelIfPending() { s_request.cancel(); }

const GlyphPickerDialogOptions& GlyphPickerDialog::currentOptions() { return s_request.options(); }

std::optional<GlyphPickerResult> GlyphPickerDialog::lastResult() noexcept { return s_lastResult; }
