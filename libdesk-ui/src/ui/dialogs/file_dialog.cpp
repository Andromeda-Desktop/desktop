#include "ui/dialogs/file_dialog.h"

#include "i18n/i18n.h"
#include "ui/dialogs/dialog_request.h"

#include <utility>

namespace {

  DialogRequest<FileDialogOptions, std::filesystem::path> s_request;
  FileDialogPresenter* s_presenter = nullptr;
  std::filesystem::path s_lastBrowsedDirectory;

  std::string defaultTitle(FileDialogMode mode) {
    switch (mode) {
    case FileDialogMode::Open:
      return i18n::tr("ui.dialogs.file.title.open");
    case FileDialogMode::Save:
      return i18n::tr("ui.dialogs.file.title.save");
    case FileDialogMode::SelectFolder:
      return i18n::tr("ui.dialogs.file.title.select-folder");
    }
    return i18n::tr("ui.dialogs.file.title.default");
  }

} // namespace

void FileDialog::setPresenter(FileDialogPresenter* presenter) noexcept { s_presenter = presenter; }

bool FileDialog::open(FileDialogOptions options, CompletionCallback callback) {
  if (options.title.empty()) {
    options.title = defaultTitle(options.mode);
  }
  if (options.startDirectory.empty()) {
    options.startDirectory = s_lastBrowsedDirectory;
  }
  return s_request.open(std::move(options), std::move(callback), [] {
    return s_presenter != nullptr && s_presenter->openFileDialog();
  });
}

void FileDialog::complete(std::optional<std::filesystem::path> result) { settle(std::move(result))(); }

std::function<void()> FileDialog::settle(std::optional<std::filesystem::path> result) {
  return s_request.settle(std::move(result));
}

void FileDialog::cancelIfPending() { s_request.cancel(); }

void FileDialog::setLastBrowsedDirectory(std::filesystem::path directory) noexcept {
  s_lastBrowsedDirectory = std::move(directory);
}
const FileDialogOptions& FileDialog::currentOptions() { return s_request.options(); }
