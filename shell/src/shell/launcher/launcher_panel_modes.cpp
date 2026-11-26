#include "shell/launcher/launcher_panel.h"

#include "shell/launcher/launcher_adapters.h"
#include "shell/launcher/launcher_panel_detail.h"
#include "shell/panel/glass_piece.h"


bool LauncherPanel::pasteModeActive() const noexcept {
  return std::ranges::find(kModeProviders, modeProviderId()) != kModeProviders.end();
}

void LauncherPanel::syncModeTabs() {
  if (m_ui.modeTabs == nullptr) {
    return;
  }
  const float scale = contentScale();
  if (m_ui.modeTabItems.empty()) {
    for (const std::string_view id : kModeProviders) {
      const auto provider = std::ranges::find(m_providers, id, [](const auto& p) { return p->id(); });
      if (provider == m_providers.end()) {
        continue;
      }
      auto tab = ui::row({
          .align = FlexAlign::Center,
          .paddingH = Tokens::spaceP12 * scale,
          .radius = Tokens::sizeControlS * 0.5F * scale,
          .minHeight = Tokens::sizeControlS * scale,
      });
      tab->addChild(ui::label({
          .text = (*provider)->displayName(),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
      }));
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setCursorShape(CursorShape::Pointer);
      area->setOnClick([this, modeId = std::string(id)](const InputArea::PointerData&) { setMode(modeId); });
      Flex* tabPtr = tab.get();
      tab->addChild(std::move(area));
      m_ui.modeTabs->addChild(std::move(tab));
      m_ui.modeTabItems.emplace_back(std::string(id), tabPtr);
    }
  }
  const bool show = m_browserMode && pasteModeActive();
  m_ui.modeTabs->setVisible(show);
  m_ui.modeTabs->setParticipatesInLayout(show);
  for (auto& [id, tab] : m_ui.modeTabItems) {
    const bool on = id == modeProviderId();
    tab->setFill(on ? colorSpecFromRole(ColorRole::Lens) : clearColorSpec());
    if (auto* label = dynamic_cast<Label*>(tab->children().front().get())) {
      label->setColor(colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2));
    }
    if (auto* area = dynamic_cast<InputArea*>(tab->children().back().get())) {
      area->setFrameSize(tab->width(), tab->height());
    }
  }
}

void LauncherPanel::syncPackStrip() {
  if (m_ui.packStrip == nullptr) {
    return;
  }
  const bool show = m_browserMode && modeProviderId() == "Stickers";
  m_ui.packStrip->setVisible(show);
  m_ui.packStrip->setParticipatesInLayout(show);
  if (show && m_ui.packStrip->children().empty()) {
    const float scale = contentScale();
    for (std::size_t i = 0; i < kStickerPacks.size(); ++i) {
      m_ui.packStrip->addChild(ui::button({
          .text = i18n::tr(std::format("launcher.stickers.packs.{}", kStickerPacks[i].second)),
          .controlHeight = Tokens::sizeControlS * scale,
          .variant = ButtonVariant::Ghost,
          .onClick = [this, i] { setStickerPack(i); },
      }));
    }
  }
  for (std::size_t i = 0; i < m_ui.packStrip->children().size(); ++i) {
    if (auto* chip = dynamic_cast<Button*>(m_ui.packStrip->children()[i].get())) {
      chip->setVariant(i == m_packIndex ? ButtonVariant::Tonal : ButtonVariant::Ghost);
    }
  }
}

void LauncherPanel::syncEmojiStrip() {
  if (m_ui.emojiStrip == nullptr) {
    return;
  }
  const bool show = m_browserMode && modeProviderId() == "Emoji" && m_query.empty();
  m_ui.emojiStrip->setVisible(show);
  m_ui.emojiStrip->setParticipatesInLayout(show);
  if (!show || !m_ui.emojiStrip->children().empty()) {
    return;
  }
  const auto emoji = std::ranges::find(m_providers, "Emoji", [](const auto& p) { return p->id(); });
  if (emoji == m_providers.end()) {
    return;
  }
  const float scale = contentScale();
  std::vector<LauncherCategory> groups{{i18n::tr("launcher.pill.recent"), "clock"}};
  std::ranges::copy((*emoji)->categories(), std::back_inserter(groups));
  for (const LauncherCategory& group : groups) {
    m_ui.emojiStrip->addChild(ui::button({
        .glyph = group.glyphName,
        .glyphSize = Tokens::sizeSymbol * scale,
        .controlHeight = Tokens::sizeControlS * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = group.label,
        .minWidth = Tokens::sizeControlS * scale,
        .onClick =
            [this, label = group.label] {
              if (m_launcherSectionsAdapter == nullptr || m_ui.launcherSections == nullptr) {
                return;
              }
              if (const auto row = m_launcherSectionsAdapter->rowForHeader(label)) {
                m_ui.launcherSections->scrollToIndex(*row);
              }
            },
    }));
  }
}

namespace {
  // Mode drop order: Apps, Files, Clipboard, Emoji.
  constexpr std::array<std::string_view, 2> kDropModes{"Clipboard", "Emoji"};
} // namespace

bool LauncherPanel::modeDropOn(std::size_t index) const {
  switch (index) {
  case 0:
    return appsView();
  case 1:
    return filesScope();
  default:
    return modeProviderId() == kDropModes[index - 2];
  }
}

void LauncherPanel::syncModeDrops() {
  const float scale = contentScale();
  for (std::size_t i = 0; i < m_ui.modeDrops.size(); ++i) {
    auto& drop = m_ui.modeDrops[i];
    if (drop.node == nullptr) {
      continue;
    }
    const bool available = i < 2 || std::ranges::any_of(m_providers, [&](const auto& p) { return p->id() == kDropModes[i - 2]; });
    drop.node->setVisible(available);
    drop.node->setParticipatesInLayout(available);
    const bool on = modeDropOn(i);
    if (on) {
      drop.node->setFill(colorSpecFromRole(ColorRole::Accent));
    } else {
      styleGlassPiece(*drop.node, Tokens::componentLauncherModeDrop * 0.5F * scale, scale, panelCardOpacity());
    }
    drop.glyph->setColor(colorSpecFromRole(on ? ColorRole::OnAccent : ColorRole::Label1));
  }
  if (m_ui.input != nullptr && m_scopedPlaceholder.empty()) {
    constexpr std::array<std::string_view, 4> kModePlaceholders{
        "launcher.mode-placeholder.apps", "launcher.mode-placeholder.files", "launcher.mode-placeholder.clipboard",
        "launcher.mode-placeholder.emoji"
    };
    std::string_view key = "launcher.search-placeholder";
    for (std::size_t i = 0; i < m_ui.modeDrops.size() && i < kModePlaceholders.size(); ++i) {
      if (modeDropOn(i)) {
        key = kModePlaceholders[i];
      }
    }
    m_ui.input->setPlaceholder(i18n::tr(key));
  }
}

void LauncherPanel::toggleModeDrop(std::size_t index) {
  const bool on = modeDropOn(index);
  if (filesScope()) {
    setFilesScope(false);
  }
  if (index == 1 && !on) {
    setFilesScope(true);
    return;
  }
  if (on) {
    m_view.set(launcher_view::Default{});
  } else if (index == 0) {
    m_view.set(launcher_view::Apps{});
  } else if (index > 1) {
    m_view.set(launcher_view::Media{.providerId = std::string(kDropModes[index - 2])});
  }
  m_selectedIndex = 0;
  syncModeTabs();
  if (m_ui.input != nullptr && !m_ui.input->value().empty()) {
    m_ui.input->setValue("");
  }
  onInputChanged("");
}

void LauncherPanel::setMode(std::string_view providerId) {
  m_view.set(launcher_view::Media{.providerId = std::string(providerId)});
  m_selectedIndex = 0;
  syncModeTabs();
  if (m_ui.input != nullptr && !m_ui.input->value().empty()) {
    m_ui.input->setValue("");
  }
  onInputChanged("");
}

void LauncherPanel::leaveProviderView() {
  if (m_view.is<launcher_view::Browser<BrowserViewState>>()) {
    m_view.set(launcher_view::Default{});
  }
}

void LauncherPanel::setFilesScope(bool on) {
  if (on) {
    m_view.set(launcher_view::Files{});
  } else if (filesScope()) {
    m_view.set(launcher_view::Default{});
  }
  m_selectedIndex = 0;
  if (!on) {
    m_kindIndex = 0;
    if (auto* files = filesProvider()) {
      files->setScopeKind({});
    }
    if (m_ui.filesTokenLabel != nullptr) {
      m_ui.filesTokenLabel->setText(i18n::tr("launcher.files.title"));
    }
  }
  syncKindTabs();
  if (m_ui.filesToken != nullptr) {
    m_ui.filesToken->setVisible(on);
    m_ui.filesToken->setParticipatesInLayout(on);
  }
  if (m_ui.launcherSections != nullptr) {
    m_ui.launcherSections->scrollView().setScrollOffset(0.0F);
  }
  queueResultsTransition(on ? ResultsTransition::NavigationForward : ResultsTransition::NavigationBackward);
  onInputChanged(m_query);
}

void LauncherPanel::setFilesKind(std::size_t index) {
  m_kindIndex = index % kFileKinds.size();
  const auto& [word, key] = kFileKinds[m_kindIndex];
  if (auto* files = filesProvider()) {
    files->setScopeKind(std::string(word));
  }
  if (m_ui.filesTokenLabel != nullptr) {
    m_ui.filesTokenLabel->setText(
        word.empty() ? i18n::tr("launcher.files.title")
                     : i18n::tr("launcher.files.title") + " · " + i18n::tr(std::format("launcher.files.kinds.{}", key))
    );
  }
  m_selectedIndex = 0;
  syncKindTabs();
  onInputChanged(m_query);
}

void LauncherPanel::setStickerPack(std::size_t index) {
  m_packIndex = index % kStickerPacks.size();
  if (auto* stickers = stickersProvider()) {
    stickers->setPack(std::string(kStickerPacks[m_packIndex].first));
  }
  m_selectedIndex = 0;
  syncPackStrip();
  onInputChanged(m_query);
}

KlipyProvider* LauncherPanel::stickersProvider() const {
  const auto provider = std::ranges::find(m_providers, "Stickers", [](const auto& p) { return p->id(); });
  return provider == m_providers.end() ? nullptr : dynamic_cast<KlipyProvider*>(provider->get());
}

void LauncherPanel::syncKindTabs() {
  if (m_ui.kindTabs == nullptr) {
    return;
  }
  const float scale = contentScale();
  if (m_ui.kindTabItems.empty()) {
    for (std::size_t i = 0; i < kFileKinds.size(); ++i) {
      auto tab = ui::row({
          .align = FlexAlign::Center,
          .paddingH = Tokens::spaceP12 * scale,
          .radius = Tokens::sizeControlS * 0.5F * scale,
          .minHeight = Tokens::sizeControlS * scale,
      });
      tab->addChild(ui::label({
          .text = i18n::tr(std::format("launcher.files.kinds.{}", kFileKinds[i].second)),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
      }));
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setCursorShape(CursorShape::Pointer);
      area->setOnClick([this, i](const InputArea::PointerData&) { setFilesKind(i); });
      Flex* tabPtr = tab.get();
      tab->addChild(std::move(area));
      m_ui.kindTabs->addChild(std::move(tab));
      m_ui.kindTabItems.push_back(tabPtr);
    }
  }
  const bool show = m_browserMode && filesScope();
  m_ui.kindTabs->setVisible(show);
  m_ui.kindTabs->setParticipatesInLayout(show);
  for (std::size_t i = 0; i < m_ui.kindTabItems.size(); ++i) {
    Flex* tab = m_ui.kindTabItems[i];
    const bool on = i == m_kindIndex;
    tab->setFill(on ? colorSpecFromRole(ColorRole::Lens) : clearColorSpec());
    if (auto* label = dynamic_cast<Label*>(tab->children().front().get())) {
      label->setColor(colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2));
    }
    if (auto* area = dynamic_cast<InputArea*>(tab->children().back().get())) {
      area->setFrameSize(tab->width(), tab->height());
    }
  }
}

void LauncherPanel::cycleMode(int direction) {
  if (m_ui.modeTabItems.empty() || modeProviderId().empty()) {
    return;
  }
  const auto it = std::ranges::find(m_ui.modeTabItems, modeProviderId(), &std::pair<std::string, Flex*>::first);
  const auto count = static_cast<int>(m_ui.modeTabItems.size());
  const int index = it == m_ui.modeTabItems.end() ? 0 : static_cast<int>(it - m_ui.modeTabItems.begin());
  setMode(m_ui.modeTabItems[static_cast<std::size_t>((index + direction + count) % count)].first);
}

void LauncherPanel::bindPreview(Renderer& renderer) {
  if (m_ui.previewPane == nullptr || !m_ui.previewPane->visible()) {
    return;
  }
  const LauncherResult& result = m_results[m_selectedIndex];
  const float scale = contentScale();
  const int iconSize = static_cast<int>(std::round(Tokens::componentLauncherIcon * scale));
  const bool emoji = !result.badge.empty();
  m_ui.previewEmoji->setVisible(emoji);
  m_ui.previewEmoji->setParticipatesInLayout(emoji);
  m_ui.previewEmoji->setText(result.badge);
  const bool tones = emoji && result.variants.size() == m_ui.previewToneLabels.size();
  m_ui.previewTones->setVisible(tones);
  m_ui.previewTones->setParticipatesInLayout(tones);
  for (std::size_t i = 0; tones && i < m_ui.previewToneLabels.size(); ++i) {
    m_ui.previewToneLabels[i]->setText(result.variants[i]);
  }
  bool imageReady = false;
  const bool media = isMediaPresentation(result);
  // A clipboard image row keeps its thumbnail in iconPath; the preview shows the full picture instead.
  if (!emoji && !media && !result.iconPath.empty() && result.presentation != "clipboard-image") {
    if (result.iconPath != m_previewIconPath) {
      m_ui.previewImage->clear(renderer);
      m_previewIconPath = result.iconPath;
    }
    imageReady =
        m_ui.previewImage->load(renderer, result.iconPath, {.targetSize = iconSize, .mipmap = true}) == ImageLoad::Ready;
  }
  m_ui.previewImage->setVisible(imageReady);
  m_ui.previewImage->setParticipatesInLayout(imageReady);
  m_ui.previewGlyph->setGlyph(result.glyphName.empty() ? "app-window" : result.glyphName);
  m_ui.previewGlyph->setVisible(!imageReady && !emoji);
  m_ui.previewGlyph->setParticipatesInLayout(!imageReady && !emoji);
  m_ui.previewOpen->setText(
      i18n::tr(
          emoji || (pasteModeActive() && result.presentation != "action") ? "launcher.pill.paste-hint"
                                                                                  : "launcher.pill.open-hint"
      )
  );

  std::string title = singleLinePreview(result.title);
  if (emoji && !title.empty()) {
    // CLDR emoji names are lower case; the preview shows them in sentence case.
    title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
  }
  m_ui.previewTitle->setText(title);
  m_ui.previewTitle->setMaxWidth(m_ui.previewPane->width() - 2.0F * Tokens::spaceP16 * scale);
  const std::string kind = result.origin.empty() ? result.subtitle : result.subtitle + " · " + result.origin;
  m_ui.previewKind->setText(singleLinePreview(kind));
  // A clipboard entry shows its own meta line (bindPreview details).
  m_ui.previewKind->setVisible(!kind.empty() && !result.presentation.starts_with("clipboard"));

  const DesktopEntry* entry = nullptr;
  if (!result.desktopEntryPath.empty()) {
    const auto& entries = desktopEntries();
    const auto it = std::ranges::find(entries, result.desktopEntryPath, &DesktopEntry::path);
    entry = it == entries.end() ? nullptr : &*it;
  }
  const bool clip = result.presentation.starts_with("clipboard");
  const bool file = result.providerId == "Files" && result.id != FilesProvider::kShowAllId;
  const bool hasAction = clip || file || (entry != nullptr && !entry->actions.empty());
  const bool clipImage = result.presentation == "clipboard-image";
  const bool clipFiles = result.presentation == "clipboard-files";
  m_ui.previewOpenImage->setText(i18n::tr(clipFiles ? "launcher.files.reveal" : "launcher.clipboard.open"));
  m_ui.previewOpenImage->setVisible(clipImage || clipFiles);
  m_ui.previewOpenImage->setParticipatesInLayout(clipImage || clipFiles);
  m_ui.previewCopyPath->setVisible(clipFiles);
  m_ui.previewCopyPath->setParticipatesInLayout(clipFiles);
  m_ui.previewAction->setVisible(hasAction);
  m_ui.previewAction->setParticipatesInLayout(hasAction);
  if (clip) {
    m_ui.previewAction->setText(i18n::tr(result.pinned ? "launcher.clipboard.unpin" : "launcher.clipboard.pin"));
  } else if (file) {
    m_ui.previewAction->setText(i18n::tr("launcher.files.reveal"));
  } else if (hasAction) {
    m_ui.previewAction->setText(entry->actions.front().name);
  }
  m_ui.previewDelete->setText(i18n::tr(file ? "launcher.files.copy-path" : "launcher.clipboard.delete"));
  m_ui.previewDelete->setVisible(clip || file);
  m_ui.previewDelete->setParticipatesInLayout(clip || file);
  const bool body = result.presentation == "clipboard-text" || clipFiles;
  m_ui.previewBodyBox->setVisible(body);
  m_ui.previewBodyBox->setParticipatesInLayout(body);
  m_ui.previewTitle->setVisible(!body);
  m_ui.previewTitle->setParticipatesInLayout(!body);
  if (body) {
    const auto provider = std::ranges::find(m_providers, "Clipboard", [](const auto& p) { return p->id(); });
    const auto* clipboard = provider == m_providers.end() ? nullptr : dynamic_cast<const ClipboardProvider*>(provider->get());
    m_ui.previewBody->setText(clipboard == nullptr ? result.title : clipboard->text(result));
    m_ui.previewBody->setMaxWidth(m_ui.previewPane->width() - 2.0F * (Tokens::spaceP16 + Tokens::spaceP12) * scale);
  }
  bool shot = result.presentation == "clipboard-image";
  std::string gif;
  if (result.presentation == "gif") {
    const auto provider = std::ranges::find(m_providers, "Gifs", [](const auto& p) { return p->id(); });
    if (auto* klipy = provider == m_providers.end() ? nullptr : dynamic_cast<KlipyProvider*>(provider->get())) {
      gif = klipy->previewGif(result);
    }
  }
  if (!gif.empty() && playPreviewGif(renderer, gif)) {
    m_previewShotId.clear();
    shot = true;
  } else if (media) {
    stopPreviewGif();
    m_previewShotId.clear();
    shot = !result.iconPath.empty()
        && m_ui.previewShot->load(
               renderer, result.iconPath,
               {.targetSize = static_cast<int>(std::round(Tokens::componentLauncherPreviewMedia * 1.75F * scale)),
                .mipmap = true}
           ) == ImageLoad::Ready;
  }
  m_ui.previewShot->setVisible(shot);
  m_ui.previewShot->setParticipatesInLayout(shot);
  ClipboardProvider* clipboard = nullptr;
  if (clip) {
    m_ui.previewOpen->setText(i18n::tr("launcher.pill.paste-hint"));
    const auto provider = std::ranges::find(m_providers, "Clipboard", [](const auto& p) { return p->id(); });
    clipboard = provider == m_providers.end() ? nullptr : dynamic_cast<ClipboardProvider*>(provider->get());
  }
  if (!media) {
    stopPreviewGif();
  }
  // The image comes from the row's data URI (decrypted and encoded on the SharedWorker) and decodes in the async
  // texture cache; an image without one yet asks for it and shows when the results refresh.
  if (shot && clipboard != nullptr && m_previewShotId != result.id + result.iconPath) {
    m_previewShotId = result.id + result.iconPath;
    if (result.iconPath.empty()) {
      m_ui.previewShot->clear(renderer);
      clipboard->requestThumbnail(result);
    } else {
      m_ui.previewShot->load(
          renderer, result.iconPath,
          {.targetSize = static_cast<int>(std::round(Tokens::componentLauncherPreviewMedia * 1.75F * scale))}
      );
    }
  }
  if (shot || media || body) {
    m_ui.previewGlyph->setVisible(false);
    m_ui.previewGlyph->setParticipatesInLayout(false);
  }

  std::array<std::pair<std::string, std::string>, 3> meta{};
  std::size_t count = 0;
  if (entry != nullptr) {
    const int uses = m_usageTracker.getCount(result.providerId, result.id);
    if (const auto last = m_usageTracker.getLastUsed(result.providerId, result.id)) {
      meta[count++] = {i18n::tr("launcher.pill.last-opened"),
                       formatTimeAgo(std::chrono::system_clock::time_point(std::chrono::seconds(*last)))};
    }
    if (uses > 0) {
      meta[count++] = {i18n::tr("launcher.pill.opened"), i18n::trp("launcher.pill.times", uses)};
    }
    if (!result.category.empty()) {
      meta[count++] = {i18n::tr("launcher.pill.category"), result.category};
    }
  } else if (clip) {
    // One meta line over the content (proto surfaces.js): "Text, 21 characters, Terminal, 20:03".
    std::string line;
    if (result.presentation == "clipboard-files") {
      line = i18n::tr("clipboard.preview.files-title");
    } else if (result.presentation == "clipboard-text") {
      const std::string text = clipboard == nullptr ? result.title : clipboard->text(result);
      line = i18n::trp("launcher.clipboard.meta-text", std::ranges::count_if(text, [](char c) { return (c & 0xC0) != 0x80; }));
    } else {
      line = i18n::tr("launcher.clipboard.image");
      if (const std::size_t bytes = clipboard == nullptr ? 0 : clipboard->byteSize(result); bytes > 0) {
        line += ", " + formatByteSize(bytes);
      }
    }
    if (!result.subtitle.empty()) {
      line += ", " + result.subtitle;
    }
    m_ui.previewKind->setText(line);
    m_ui.previewKind->setVisible(true);
  } else if (file) {
    // One stat on selection: the index answer carries no sizes, and the file may have changed since.
    std::error_code ec;
    const auto status = std::filesystem::status(result.id, ec);
    if (!ec && std::filesystem::is_regular_file(status)) {
      meta[count++] = {i18n::tr("launcher.files.size"), formatByteSize(std::filesystem::file_size(result.id, ec))};
    }
    if (const auto written = std::filesystem::last_write_time(result.id, ec); !ec) {
      meta[count++] = {i18n::tr("launcher.files.modified"),
                       formatTimeAgo(std::chrono::clock_cast<std::chrono::system_clock>(written))};
    }
  } else if (emoji) {
    meta[count++] = {i18n::tr("launcher.pill.category"), result.category};
    if (const int uses = m_usageTracker.getCount(result.providerId, result.id); uses > 0) {
      meta[count++] = {i18n::tr("launcher.pill.used"), i18n::trp("launcher.pill.times", uses)};
    }
    std::string codepoints;
    for (std::size_t i = 0; i < result.badge.size();) {
      char32_t cp = 0;
      const auto c = static_cast<unsigned char>(result.badge[i]);
      const std::size_t len = c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
      cp = len == 1 ? c : (c & (0xFF >> (len + 1)));
      for (std::size_t k = 1; k < len && i + k < result.badge.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(result.badge[i + k]) & 0x3F);
      }
      i += len;
      if (cp != 0xFE0F && cp != 0x200D) {
        codepoints += std::format("{}U+{:04X}", codepoints.empty() ? "" : " ", static_cast<std::uint32_t>(cp));
      }
    }
    meta[count++] = {i18n::tr("launcher.pill.unicode"), codepoints};
  }
  for (std::size_t i = 0; i < m_ui.previewMetaRows.size(); ++i) {
    const bool show = i < count;
    m_ui.previewMetaRows[i]->setVisible(show);
    m_ui.previewMetaRows[i]->setParticipatesInLayout(show);
    if (show) {
      m_ui.previewMetaKeys[i]->setText(meta[i].first);
      m_ui.previewMetaValues[i]->setText(meta[i].second);
    }
  }
}

void LauncherPanel::activateEmojiVariant(std::size_t tone) {
  if (m_selectedIndex >= m_results.size() || tone >= m_results[m_selectedIndex].variants.size()) {
    return;
  }
  LauncherResult result = m_results[m_selectedIndex];
  result.id = "emoji-" + result.variants[tone];
  for (auto& provider : m_providers) {
    if (provider->id() == std::string_view(result.providerId) && provider->activate(result)) {
      finishActivation(*provider, result.id, provider->supportsAutoPaste());
      return;
    }
  }
}

void LauncherPanel::deleteSelectedClipboardEntry() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }
  if (auto* files = filesProvider(); files != nullptr && m_results[m_selectedIndex].providerId == "Files") {
    files->copyPath(m_results[m_selectedIndex]);
    finishActivation(*files, m_results[m_selectedIndex].id, false);
    return;
  }
  const auto provider = std::ranges::find(m_providers, "Clipboard", [](const auto& p) { return p->id(); });
  if (provider != m_providers.end()) {
    if (auto* clipboard = dynamic_cast<ClipboardProvider*>(provider->get()); clipboard != nullptr
        && clipboard->remove(m_results[m_selectedIndex])) {
      reapplyCurrentQuery();
    }
  }
}

void LauncherPanel::openSelectedClipboardImage() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }
  const auto provider = std::ranges::find(m_providers, "Clipboard", [](const auto& p) { return p->id(); });
  if (provider == m_providers.end()) {
    return;
  }
  auto* clipboard = dynamic_cast<ClipboardProvider*>(provider->get());
  const LauncherResult& result = m_results[m_selectedIndex];
  if (clipboard == nullptr) {
    return;
  }
  if (result.presentation == "clipboard-files") {
    clipboard->reveal(result);
  } else {
    clipboard->openImage(result);
  }
}

void LauncherPanel::copySelectedClipboardPaths() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }
  const auto provider = std::ranges::find(m_providers, "Clipboard", [](const auto& p) { return p->id(); });
  if (provider == m_providers.end()) {
    return;
  }
  if (auto* clipboard = dynamic_cast<ClipboardProvider*>(provider->get()); clipboard != nullptr) {
    clipboard->copyPaths(m_results[m_selectedIndex]);
  }
}

FilesProvider* LauncherPanel::filesProvider() const {
  const auto provider = std::ranges::find(m_providers, "Files", [](const auto& p) { return p->id(); });
  return provider == m_providers.end() ? nullptr : dynamic_cast<FilesProvider*>(provider->get());
}

void LauncherPanel::activatePreviewAction() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }
  if (auto* files = filesProvider(); files != nullptr && m_results[m_selectedIndex].providerId == "Files") {
    files->reveal(m_results[m_selectedIndex]);
    finishActivation(*files, m_results[m_selectedIndex].id, false);
    return;
  }
  if (m_results[m_selectedIndex].presentation.starts_with("clipboard")) {
    const auto provider = std::ranges::find(m_providers, "Clipboard", [](const auto& p) { return p->id(); });
    if (provider != m_providers.end()) {
      if (auto* clipboard = dynamic_cast<ClipboardProvider*>(provider->get()); clipboard != nullptr
          && clipboard->togglePinned(m_results[m_selectedIndex])) {
        reapplyCurrentQuery();
      }
    }
    return;
  }
  LauncherResult result = m_results[m_selectedIndex];
  const auto& entries = desktopEntries();
  const auto entry = std::ranges::find(entries, result.desktopEntryPath, &DesktopEntry::path);
  if (entry == entries.end() || entry->actions.empty()) {
    return;
  }
  const DesktopAction& action = entry->actions.front();
  result.id = AppProvider::actionResultId(result.desktopEntryPath, action.id);
  result.desktopActionId = action.id;
  for (auto& provider : m_providers) {
    if (provider->id() == std::string_view(result.providerId) && provider->activate(result)) {
      finishActivation(*provider, result.id, provider->supportsAutoPaste(), true);
      return;
    }
  }
}

bool LauncherPanel::playPreviewGif(Renderer& renderer, const std::string& path) {
  if (path == m_previewGifPath && !m_previewFrames.empty()) {
    return true;
  }
  stopPreviewGif();
  const std::vector<std::uint8_t> bytes = FileUtils::readBinaryFile(path);
  auto decoded = decodeAnimatedGif(bytes.data(), bytes.size(), kMaxPreviewGifFrames, kMaxPreviewGifBytes);
  if (!decoded || decoded->frames.empty()) {
    return false;
  }
  for (const auto& frame : decoded->frames) {
    const TextureHandle handle = renderer.textureManager().loadFromRgba(frame.rgba.data(), decoded->width, decoded->height, false);
    if (handle.id == 0) {
      break;
    }
    m_previewFrames.push_back({handle, std::chrono::milliseconds(std::max<std::uint32_t>(frame.durationMs, 20))});
  }
  if (m_previewFrames.empty()) {
    return false;
  }
  m_previewRenderer = &renderer;
  m_previewGifPath = path;
  m_previewFrame = 0;
  m_ui.previewShot->setExternalTexture(renderer, m_previewFrames.front().handle);
  if (m_previewFrames.size() > 1) {
    m_previewFrameTimer.start(m_previewFrames.front().duration, [this] { showNextPreviewFrame(); });
  }
  return true;
}

void LauncherPanel::showNextPreviewFrame() {
  if (m_previewFrames.size() <= 1 || m_previewRenderer == nullptr || m_ui.previewShot == nullptr) {
    return;
  }
  m_previewFrame = (m_previewFrame + 1) % m_previewFrames.size();
  m_ui.previewShot->setExternalTexture(*m_previewRenderer, m_previewFrames[m_previewFrame].handle);
  PanelManager::instance().requestRedraw();
  m_previewFrameTimer.start(m_previewFrames[m_previewFrame].duration, [this] { showNextPreviewFrame(); });
}

void LauncherPanel::stopPreviewGif() {
  m_previewFrameTimer.stop();
  if (m_previewFrames.empty()) {
    return;
  }
  if (m_ui.previewShot != nullptr && m_previewRenderer != nullptr) {
    m_ui.previewShot->clear(*m_previewRenderer);
  }
  for (PreviewFrame& frame : m_previewFrames) {
    m_previewRenderer->textureManager().unload(frame.handle);
  }
  m_previewFrames.clear();
  m_previewGifPath.clear();
}

bool LauncherPanel::shouldUseDetailPresentation() const {
  return m_results.size() == 1 && isDetailPresentation(m_results.front());
}

void LauncherPanel::bindDetailResult() {
  if (!shouldUseDetailPresentation()
      || m_ui.detailScroll == nullptr
      || m_ui.detailSubtitle == nullptr
      || m_ui.detailBody == nullptr) {
    return;
  }

  const LauncherResult& result = m_results.front();
  const bool hasSubtitle = !result.subtitle.empty();
  m_ui.detailSubtitle->setVisible(hasSubtitle);
  m_ui.detailSubtitle->setParticipatesInLayout(hasSubtitle);
  m_ui.detailSubtitle->setText(singleLinePreview(result.subtitle));
  m_ui.detailBody->setText(result.title.empty() ? result.id : result.title);
  m_ui.detailScroll->setScrollOffset(0.0F);
}
