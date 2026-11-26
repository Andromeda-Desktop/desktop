#include "smart_folder_sheet.h"

#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <format>

namespace fs = std::filesystem;

namespace {
  const float& kWidth = Tokens::componentDialogWidth; // five kind segments need the full dialog width
  const float& kFieldLabel = Tokens::componentFilesFieldLabel;
  constexpr std::array<const char*, 5> kKinds{"", "pdf", "image", "doc", "sheet"};
  constexpr std::array<const char*, 5> kKindLabels{"files.smart.any", "files.smart.pdfs", "files.smart.images", "files.smart.docs", "files.smart.sheets"};
  constexpr std::array<int, 4> kDays{0, 7, 30, 365};
  constexpr std::array<const char*, 4> kDayLabels{"files.smart.any", "files.smart.7d", "files.smart.30d", "files.smart.year"};
} // namespace

SmartFolderSheet::SmartFolderSheet(SmartFolder folder, fs::path here, std::string original,
                                   std::function<void(const SmartFolder&, const std::string&)> save,
                                   std::function<void()> close, std::function<void()> rebuild)
    : m_folder(std::move(folder)), m_here(std::move(here)), m_original(std::move(original)), m_save(std::move(save)),
      m_close(std::move(close)), m_rebuild(std::move(rebuild)) {}

std::string SmartFolderSheet::error() const {
  if (m_folder.name.find_first_not_of(" \t") == std::string::npos || m_folder.name.find('/') != std::string::npos) {
    return i18n::tr("files.smart.name-invalid");
  }
  if (m_folder.name != m_original) {
    const auto all = smart_folders::load();
    if (std::ranges::any_of(all, [&](const SmartFolder& f) { return f.name == m_folder.name; })) {
      return i18n::tr("files.smart.name-exists", "name", m_folder.name);
    }
  }
  if (m_folder.text.empty() && m_folder.kind.empty() && m_folder.days == 0) {
    return i18n::tr("files.smart.no-condition");
  }
  return {};
}

void SmartFolderSheet::update() {
  const std::string bad = error();
  if (m_sentence != nullptr) {
    m_sentence->setText(bad.empty() ? m_folder.sentence() : bad);
    m_sentence->setColor(colorSpecFromRole(bad.empty() ? ColorRole::Label2 : ColorRole::DangerText));
  }
  if (m_ok != nullptr) {
    m_ok->setEnabled(bad.empty());
  }
}

void SmartFolderSheet::set(const std::string& field, const std::string& value) {
  if (field == "name") {
    m_folder.name = value;
  } else if (field == "text") {
    m_folder.text = value;
  } else if (field == "kind") {
    m_folder.kind = value;
  } else if (field == "days") {
    m_folder.days = std::atoi(value.c_str());
  } else if (field == "where") {
    m_folder.folder = value == "pc" ? fs::path() : m_here;
  }
  DeferredCall::callLater([rebuild = m_rebuild] { rebuild(); });
}

void SmartFolderSheet::submit() {
  if (!error().empty()) {
    update();
    return;
  }
  m_save(m_folder, m_original);
  m_close();
}

std::unique_ptr<Node> SmartFolderSheet::build(float scale) {
  const float s = scale;
  auto sheet = ui::column({.align = FlexAlign::Stretch, .width = kWidth * s});
  auto header = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::componentDialogPadding * s,
                         .height = Tokens::componentDialogHeader * s});
  header->addChild(ui::label({.text = i18n::tr("files.smart.title"), .fontSize = Tokens::typeTitle3.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
  sheet->addChild(std::move(header));

  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                          .paddingH = Tokens::componentDialogPadding * s});
  const auto fieldRow = [&](const std::string& label) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    auto cell = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .width = kFieldLabel * s});
    cell->addChild(ui::label({.text = std::string(label), .fontSize = Style::fontSizeCaption * s,
                              .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2)}));
    row->addChild(std::move(cell));
    return row;
  };
  const auto text = [&](const std::string& label, std::string& value, const std::string& placeholder) {
    auto row = fieldRow(label);
    row->addChild(ui::input({.value = value, .placeholder = std::string(placeholder), .fontSize = Style::fontSizeBody * s,
                             .controlHeight = Style::controlHeight * s, .flexGrow = 1.0F,
                             .onChange = [this, &value](const std::string& v) {
                               value = v;
                               update();
                             },
                             .onSubmit = [this](const std::string&) { submit(); }}));
    body->addChild(std::move(row));
  };
  const auto choice = [&](const std::string& label, std::vector<ui::SegmentedOption> options, std::size_t selected,
                          std::function<void(std::size_t)> set) {
    auto row = fieldRow(label);
    row->addChild(ui::segmented({.options = std::move(options), .selectedIndex = selected, .scale = s,
                                 .equalSegmentWidths = true, .flexGrow = 1.0F,
                                 .onChange = [this, set = std::move(set)](std::size_t i) {
                                   set(i);
                                   update();
                                 }}));
    body->addChild(std::move(row));
  };

  text(i18n::tr("files.smart.field-name"), m_folder.name, i18n::tr("files.smart.hint-name"));
  text(i18n::tr("files.smart.field-contains"), m_folder.text, i18n::tr("files.smart.hint-contains"));
  std::vector<ui::SegmentedOption> kinds;
  for (const char* k : kKindLabels) {
    kinds.push_back({.label = i18n::tr(k)});
  }
  const auto kindAt = static_cast<std::size_t>(std::ranges::find(kKinds, m_folder.kind) - kKinds.begin());
  choice(i18n::tr("files.smart.field-kind"), std::move(kinds), std::min(kindAt, kKinds.size() - 1),
         [this](std::size_t i) { m_folder.kind = kKinds[i]; });
  std::vector<ui::SegmentedOption> days;
  for (const char* d : kDayLabels) {
    days.push_back({.label = i18n::tr(d)});
  }
  const auto dayAt = static_cast<std::size_t>(std::ranges::find(kDays, m_folder.days) - kDays.begin());
  choice(i18n::tr("files.smart.field-modified"), std::move(days), std::min(dayAt, kDays.size() - 1), [this](std::size_t i) { m_folder.days = kDays[i]; });
  if (!m_here.empty()) {
    choice(i18n::tr("files.smart.field-where"), {{.label = i18n::tr("files.smart.this-pc")}, {.label = m_here.filename().string()}}, m_folder.folder.empty() ? 0 : 1,
           [this](std::size_t i) { m_folder.folder = i == 0 ? fs::path() : m_here; });
  }
  body->addChild(ui::label({.out = &m_sentence, .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                            .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 2}));
  sheet->addChild(std::move(body));

  auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s,
                         .padding = Tokens::componentDialogPadding * s});
  footer->addChild(ui::button({.text = std::string(i18n::tr("common.actions.cancel")), .variant = ButtonVariant::Default,
                               .onClick = [this] { DeferredCall::callLater([close = m_close] { close(); }); }}));
  footer->addChild(ui::button({.out = &m_ok, .text = i18n::tr(m_original.empty() ? "files.smart.save-sidebar" : "files.smart.save"),
                               .variant = ButtonVariant::Primary,
                               .onClick = [this] { DeferredCall::callLaterIfAlive(m_alive, [this] { submit(); }); }}));
  sheet->addChild(std::move(footer));
  update();
  return sheet;
}
