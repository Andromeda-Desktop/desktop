#include "i18n/i18n.h"
#include "shell/settings/printers_model.h"
#include "shell/settings/settings_content_printers.h"
#include "shell/settings/settings_page_rows.h"
#include "shell/settings/settings_sheet_modal.h"
#include "shell/settings/settings_window.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <memory>
#include <string>
#include <utility>
#include <variant>

void SettingsWindow::addPrintersContent(float scale) {
  if (m_selectedSection != "printers") {
    return;
  }
  if (m_printers == nullptr) {
    m_printers = std::make_unique<PrintersModel>(
        [this] {
          if (isOpen() && m_selectedSection == "printers") {
            requestContentRebuild();
          }
        },
        [this] { return isOpen() && m_selectedSection == "printers"; }
    );
  }
  settings::addSettingsPrinters(
      *m_contentContainer,
      settings::SettingsPrintersContext{
          .scale = scale,
          .model = m_printers.get(),
          .detail = m_detail,
          .openDetail = [this](std::string detail) { openDetail(std::move(detail)); },
          .back = [this] { leaveDetail(); },
          .addByAddress = [this] { openPrinterAddressSheet(); },
      }
  );
}

void SettingsWindow::openPrinterAddressSheet() {
  if (m_editorSheetModal == nullptr) {
    m_editorSheetModal = std::make_unique<settings::SettingsSheetModal>();
    m_editorSheetModal->initialize(m_modalHost, [this] { dismissOpenSelectDropdown(); });
  } else if (m_editorSheetModal->isOpen()) {
    m_editorSheetModal->close();
  }
  const float sheetScale = uiScale();
  auto draft = std::make_shared<std::string>();
  // The probe may answer after this sheet closed; only onClosed owns the token, so it expires then.
  auto token = std::make_shared<char>();
  const auto submit = [this, draft, weak = std::weak_ptr<char>(token)] {
    if (draft->empty() || m_printers == nullptr) {
      return;
    }
    m_editorSheetModal->setStatusMessage(i18n::tr("settings.printers.checking"), false);
    m_printers->probe(*draft, [this, weak](std::variant<FoundPrinter, JobError> result) {
      if (weak.expired() || !m_editorSheetModal->isOpen()) {
        return;
      }
      if (const auto* error = std::get_if<JobError>(&result)) {
        m_editorSheetModal->setStatusMessage(error->message, true);
        return;
      }
      m_printers->add(std::get<FoundPrinter>(std::move(result)));
      m_editorSheetModal->close();
    });
  };
  m_editorSheetModal->open(settings::SettingsSheetRequest{
      .sheetTitle = i18n::tr("settings.printers.address-title"),
      .populateSheetBody =
          [this, draft, submit, sheetScale](Flex& body) {
            body.addChild(ui::label({
                .text = i18n::tr("settings.printers.address-hint"),
                .fontSize = Tokens::typeFootnote.size * sheetScale,
                .color = colorSpecFromRole(ColorRole::Label2),
                .maxLines = 2,
            }));
            body.addChild(ui::input({
                .value = *draft,
                .placeholder = "192.168.1.20",
                .fontSize = Style::fontSizeBody * sheetScale,
                .controlHeight = Style::controlHeight * sheetScale,
                .horizontalPadding = Style::spaceSm * sheetScale,
                .onChange = [draft](const std::string& text) { *draft = text; },
                .onSubmit = [submit](const std::string&) { submit(); },
            }));
            auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End,
                                    .gap = Style::spaceSm * sheetScale, .fillWidth = true});
            actions->addChild(settings::page_rows::plainButton(
                i18n::tr("common.actions.cancel"), ButtonVariant::Default, sheetScale,
                [this] { m_editorSheetModal->close(); }
            ));
            actions->addChild(settings::page_rows::plainButton(
                i18n::tr("settings.printers.add"), ButtonVariant::Primary, sheetScale, submit
            ));
            body.addChild(std::move(actions));
          },
      .scale = sheetScale,
      .minWidth = 360.0F * sheetScale,
      .maxWidth = 420.0F * sheetScale,
      .onClosed = [token]() mutable { token.reset(); },
  });
}
