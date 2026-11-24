#include "extract_sheet.h"

#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>

namespace fs = std::filesystem;

namespace {
  const float& kFieldLabel = Tokens::componentFilesFieldLabel;

  // "photos" for photos.tar.gz and photos.zip.001, as FileOps1 names the folder.
  std::string archiveStem(const fs::path& archive) {
    std::string name = archive.filename().string();
    if (name.size() > 4 && name.ends_with(".001")) {
      name.resize(name.size() - 4);
    }
    for (const char* ext : {".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2", ".tgz", ".txz", ".tbz2", ".zip", ".7z", ".tar"}) {
      const std::string_view e(ext);
      if (name.size() > e.size() && name.ends_with(e)) {
        return name.substr(0, name.size() - e.size());
      }
    }
    return name;
  }
} // namespace

ExtractSheet::ExtractSheet(fs::path archive, std::vector<std::string> topNames, Extract extract, ChooseFolder choose,
                           std::function<void()> close, std::function<void()> rebuild)
    : m_archive(std::move(archive)), m_topNames(std::move(topNames)), m_extract(std::move(extract)),
      m_choose(std::move(choose)), m_close(std::move(close)), m_rebuild(std::move(rebuild)),
      m_destination(m_archive.parent_path()) {}

std::string ExtractSheet::smartNote() const {
  if (m_topNames.size() == 1) {
    return i18n::tr("files.extract.note-one", "name", m_topNames.front());
  }
  if (m_topNames.empty()) {
    return i18n::tr("files.extract.note-empty", "folder", archiveStem(m_archive));
  }
  return i18n::tr("files.extract.note-many", "count", m_topNames.size(), "folder", archiveStem(m_archive));
}

std::unique_ptr<Node> ExtractSheet::build(float scale) {
  const float s = scale;
  const std::string name = m_archive.filename().string();
  auto sheet = ui::column({.align = FlexAlign::Stretch, .width = Tokens::componentDialogWidth * s});

  auto header = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::componentDialogPadding * s,
                         .height = Tokens::componentDialogHeader * s});
  header->addChild(ui::label({
      .text = m_state == State::Normal          ? i18n::tr("files.extract.title", "name", name)
              : m_state == State::WrongPassword ? i18n::tr("files.extract.wrong-password")
                                                : i18n::tr("files.extract.protected", "name", name),
      .fontSize = Tokens::typeTitle3.size * s,
      .fontWeight = FontWeight::SemiBold,
      .color = colorSpecFromRole(ColorRole::Label1),
      .maxLines = 1,
      .ellipsize = TextEllipsize::Middle,
  }));
  sheet->addChild(std::move(header));

  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                          .paddingH = Tokens::componentDialogPadding * s});
  if (m_state != State::Normal) {
    body->addChild(ui::label({.text = m_state == State::WrongPassword
                                          ? i18n::tr("files.extract.password-wrong", "name", name)
                                          : i18n::tr("files.extract.password-prompt"),
                              .fontSize = Style::fontSizeBody * s,
                              .color = colorSpecFromRole(m_state == State::WrongPassword ? ColorRole::DangerText
                                                                                         : ColorRole::Label2)}));
    auto field = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    field->addChild(ui::input({.value = m_request.password, .placeholder = i18n::tr("files.extract.password"),
                               .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                               .passwordMode = true, .invalid = m_state == State::WrongPassword, .flexGrow = 1.0F,
                               .onChange = [this](const std::string& v) {
                                 m_request.password = v;
                                 if (m_ok != nullptr) {
                                   m_ok->setEnabled(!v.empty());
                                 }
                               },
                               .onSubmit = [this](const std::string&) { submit(); }}));
    body->addChild(std::move(field));
  }

  auto to = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
  to->addChild(ui::label({.text = i18n::tr("files.extract.to"), .fontSize = Style::fontSizeBody * s,
                          .color = colorSpecFromRole(ColorRole::Label2), .width = kFieldLabel * s}));
  to->addChild(ui::label({.text = m_destination.filename().empty() ? m_destination.string() : m_destination.filename().string(),
                          .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                          .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
  if (m_choose) {
    to->addChild(ui::button({.text = i18n::tr("files.extract.change"), .variant = ButtonVariant::Default, .onClick = [this] {
                               m_choose([this](const fs::path& folder) {
                                 m_destination = folder;
                                 DeferredCall::callLater([rebuild = m_rebuild] { rebuild(); });
                               });
                             }}));
  }
  body->addChild(std::move(to));
  body->addChild(ui::label({.text = smartNote(), .fontSize = Style::fontSizeCaption * s,
                            .color = colorSpecFromRole(ColorRole::Label2)}));

  const auto check = [&](const std::string& label, bool& value) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    row->addChild(ui::checkbox({.checked = value, .scale = s, .onChange = [&value](bool on) { value = on; }}));
    row->addChild(ui::label({.text = std::string(label), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label1)}));
    body->addChild(std::move(row));
  };
  check(i18n::tr("files.extract.show-folder"), m_request.showWhenDone);
  check(i18n::tr("files.extract.trash-archive"), m_request.trashArchive);
  sheet->addChild(std::move(body));

  auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s,
                         .padding = Tokens::componentDialogPadding * s});
  footer->addChild(ui::button({.text = std::string(i18n::tr("common.actions.cancel")), .variant = ButtonVariant::Default, .onClick = [this] {
                                 DeferredCall::callLater([close = m_close] { close(); });
                               }}));
  footer->addChild(ui::button({.out = &m_ok, .text = i18n::tr("files.extract.extract"),
                               .enabled = m_state == State::Normal || !m_request.password.empty(),
                               .variant = ButtonVariant::Primary,
                               .onClick = [this] { DeferredCall::callLater([this] { submit(); }); }}));
  sheet->addChild(std::move(footer));
  return sheet;
}

void ExtractSheet::submit() {
  if (m_state != State::Normal && m_request.password.empty()) {
    return;
  }
  m_request.destination = m_destination;
  m_extract(m_request);
  m_close();
}
