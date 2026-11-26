#include "shell/launcher/launcher_panel.h"

#include "shell/launcher/launcher_adapters.h"
#include "shell/launcher/launcher_panel_detail.h"
#include "shell/launcher/launcher_start.h"

#include "dbus/accounts/accounts_service.h"
#include "i18n/i18n.h"
#include "shell/dock/pinned_apps.h"
#include "system/default_apps.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/virtual_grid_view.h"

#include <pwd.h>
#include <unistd.h>

#include <cmath>

namespace {

  using StartView = ShellConfig::LauncherStartView;

  constexpr std::array<StartView, 3> kStartViews{StartView::Category, StartView::Grid, StartView::List};

  const EnumOption<StartView>& startViewOption(StartView view) {
    return *std::ranges::find(ShellConfig::kLauncherStartViews, view, &EnumOption<StartView>::value);
  }

  std::string startViewLabel(StartView view) { return i18n::tr(std::string(startViewOption(view).labelKey)); }

  // The user's full name, else the login name (AccountsService absent or no RealName).
  std::string startUserName(const AccountsService* accounts) {
    if (accounts != nullptr && !accounts->realName().empty()) {
      return accounts->realName();
    }
    const passwd* pw = getpwuid(getuid());
    return pw != nullptr && pw->pw_name != nullptr ? std::string(pw->pw_name) : std::string();
  }

  // "Asha Sitish" -> "AS"; one word -> its first letter.
  std::string initialsOf(std::string_view name) {
    std::string out;
    bool wordStart = true;
    for (const char c : name) {
      if (c == ' ') {
        wordStart = true;
        continue;
      }
      if (wordStart && out.size() < 2 && static_cast<unsigned char>(c) < 0x80) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
      }
      wordStart = false;
    }
    return out;
  }

  float startCardWidth(float scale) {
    const float columns = static_cast<float>(Tokens::componentLauncherStartCardColumns);
    const float inner = Tokens::componentLauncherStartWidth - 2.0F * Tokens::componentLauncherStartPaddingH;
    return (inner - (columns - 1.0F) * Tokens::spaceP12) / columns * scale;
  }

  std::size_t rowsFor(std::size_t items, std::size_t columns) { return (items + columns - 1) / columns; }

} // namespace

bool LauncherPanel::usesStartLayout() const {
  return m_scopedProviderId.empty() && m_config != nullptr
      && m_config->config().shell.launcher.layout == ShellConfig::LauncherLayout::Start;
}

std::string LauncherPanel::launcherPlaceholder() const {
  if (!m_scopedPlaceholder.empty()) {
    return m_scopedPlaceholder;
  }
  return i18n::tr(usesStartLayout() ? "launcher.start.search-placeholder" : "launcher.search-placeholder");
}

ShellConfig::LauncherStartView LauncherPanel::startView() const {
  return m_config != nullptr ? m_config->config().shell.launcher.startView : StartView::Category;
}

float LauncherPanel::startHomeHeight() const {
  const float scale = contentScale();
  const auto columns = static_cast<std::size_t>(Tokens::componentLauncherStartColumns);
  const float head = Tokens::componentLauncherStartSectionGap + Tokens::componentLauncherStartSectionHead + Tokens::spaceP8;
  float height = Tokens::componentLauncherStartPaddingTop + Tokens::componentLauncherStartField + head + Tokens::spaceP8
      + Tokens::componentLauncherStartFooter;
  const std::size_t pinRows = m_startShowAllPins
      ? rowsFor(m_startPinned.size(), columns)
      : std::min(rowsFor(m_startPinned.size(), columns), static_cast<std::size_t>(Tokens::componentLauncherStartRows));
  if (pinRows > 0) {
    height += head + static_cast<float>(pinRows) * (Tokens::componentLauncherStartTile + Tokens::spaceP4) - Tokens::spaceP4;
  }
  const float max = Tokens::componentLauncherStartMaxHeight * scale;
  if (startView() != StartView::Category || m_startCards.empty()) {
    return max;
  }
  const auto cardRows = static_cast<float>(
      rowsFor(m_startCards.size(), static_cast<std::size_t>(Tokens::componentLauncherStartCardColumns))
  );
  const float cards = cardRows * launcher_start::cardCellHeight(startCardWidth(scale), scale)
      + (cardRows - 1.0F) * Tokens::spaceP12 * scale;
  return std::min(max, height * scale + cards);
}

void LauncherPanel::createStartSheet(std::unique_ptr<Input> input, std::unique_ptr<Flex> body) {
  const float scale = contentScale();
  const float padH = Tokens::componentLauncherStartPaddingH * scale;
  const float field = Tokens::componentLauncherStartField * scale;

  auto container = ui::column({.out = &m_ui.container, .align = FlexAlign::Stretch, .fillWidth = true, .fillHeight = true});
  auto content = ui::column({.align = FlexAlign::Stretch, .fillWidth = true, .flexGrow = 1.0F});
  content->setPadding(Tokens::componentLauncherStartPaddingTop * scale, padH, Tokens::spaceP8 * scale, padH);

  input->setFlexGrow(1.0F);
  auto search = ui::row(
      {.align = FlexAlign::Center,
       .gap = Tokens::spaceP12 * scale,
       .paddingH = Tokens::spaceP16 * scale,
       .fill = colorSpecFromRole(ColorRole::GlassCard),
       .radius = field * 0.5F,
       .minHeight = field,
       .fillWidth = true},
      ui::glyph({.glyph = "search", .glyphSize = Tokens::sizeSymbolL * scale, .color = colorSpecFromRole(ColorRole::Label2)}),
      std::move(input)
  );
  search->setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 1.0F);
  content->addChild(std::move(search));

  // Section head: title-3 leading, a small fill_2 button trailing (24 high), 24 above and 8 below.
  const auto sectionHead = [scale](Flex** out, std::string title) {
    auto head = ui::row({
        .out = out,
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP8 * scale,
        .minHeight = Tokens::componentLauncherStartSectionHead * scale,
        .fillWidth = true,
    });
    head->setPadding(Tokens::componentLauncherStartSectionGap * scale, 0.0F, Tokens::spaceP8 * scale, 0.0F);
    head->addChild(ui::label({
        .text = std::move(title),
        .fontSize = Tokens::typeTitle3.size * scale,
        .fontWeight = FontWeight::SemiBold,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxLines = 1,
    }));
    return head;
  };
  const auto smallButton = [scale](Button** out, std::string text, std::string glyph, bool trailing, std::function<void()> onClick) {
    return ui::button({
        .out = out,
        .text = std::move(text),
        .glyph = std::move(glyph),
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Tokens::sizeSymbolInline * scale,
        .controlHeight = Tokens::componentLauncherStartSectionHead * scale,
        .variant = ButtonVariant::Tonal,
        .paddingH = Tokens::spaceP8 * scale,
        .gap = Tokens::spaceP4 * scale,
        .radius = Style::scaledRadius(Tokens::radiusR8, scale),
        .onClick = std::move(onClick),
        .configure = [trailing](Button& button) { button.setGlyphTrailing(trailing); },
    });
  };
  const auto spacer = [] { return ui::row({.flexGrow = 1.0F}); };

  auto pinnedHead = sectionHead(&m_ui.startPinnedHead, i18n::tr("launcher.start.pinned"));
  pinnedHead->addChild(spacer());
  pinnedHead->addChild(smallButton(&m_ui.startShowAll, i18n::tr("launcher.start.show-all"), "chevron-down", true, [this] {
    m_startShowAllPins = !m_startShowAllPins;
    syncStartSheet();
  }));
  content->addChild(std::move(pinnedHead));

  LauncherListStyle tileStyle = launcherListStyleFrom(false, true, m_config, scale, panelCardOpacity());
  m_startPinnedAdapter = std::make_unique<LauncherAppGridAdapter>(tileStyle);
  m_startPinnedAdapter->setResults(&m_startPinned);
  // Pinned tiles stand for rows of the home list: the home shows every app, so each pin is one of m_results.
  const auto resultIndex = [this](std::size_t pin) -> std::optional<std::size_t> {
    if (pin >= m_startPinned.size()) {
      return std::nullopt;
    }
    const auto it = std::ranges::find(m_results, m_startPinned[pin].id, &LauncherResult::id);
    return it == m_results.end() ? std::nullopt : std::optional(static_cast<std::size_t>(it - m_results.begin()));
  };
  m_startPinnedAdapter->setOnActivate([this, resultIndex](std::size_t pin) {
    if (const auto index = resultIndex(pin)) {
      activateAt(*index);
    }
  });
  m_startPinnedAdapter->setOnSecondaryActivate([this, resultIndex](std::size_t pin, float x, float y) {
    if (const auto index = resultIndex(pin)) {
      (void)openAppActionsMenu(*index, x, y);
    }
  });
  content->addChild(ui::virtualGridView({
      .out = &m_ui.startPinnedGrid,
      .contentScale = scale,
      .columns = static_cast<std::size_t>(Tokens::componentLauncherStartColumns),
      .cellHeight = Tokens::componentLauncherStartTile * scale,
      .squareCells = false,
      .columnGap = Tokens::spaceP4 * scale,
      .rowGap = Tokens::spaceP4 * scale,
      .itemCursorShape = CursorShape::Pointer,
      .scrollbarVisible = false,
      .adapter = m_startPinnedAdapter.get(),
      .configure = [](VirtualGridView& grid) { grid.setFillWidth(true); },
  }));

  auto allHead = sectionHead(&m_ui.startAllHead, i18n::tr("launcher.start.all"));
  allHead->addChild(smallButton(&m_ui.startBack, {}, "chevron-left", false, [this] {
    m_activeCategoryType = All;
    m_activeCategory.clear();
    applyActiveCategory();
  }));
  allHead->addChild(spacer());
  allHead->addChild(smallButton(&m_ui.startViewButton, {}, "chevron-down", true, [this] { openStartViewMenu(); }));
  content->addChild(std::move(allHead));

  m_startCardAdapter = std::make_unique<LauncherStartCardAdapter>(&m_startCards, scale, [this](std::size_t card) {
    if (card >= m_startCards.size()) {
      return;
    }
    m_activeCategoryType = Category;
    m_activeCategory = m_startCards[card].category;
    applyActiveCategory();
  });
  const float cardWidth = startCardWidth(scale);
  content->addChild(ui::virtualGridView({
      .out = &m_ui.startCardGrid,
      .contentScale = scale,
      .columns = static_cast<std::size_t>(Tokens::componentLauncherStartCardColumns),
      .cellHeight = launcher_start::cardCellHeight(cardWidth, scale),
      .squareCells = false,
      .columnGap = Tokens::spaceP12 * scale,
      .rowGap = Tokens::spaceP12 * scale,
      .itemCursorShape = CursorShape::Pointer,
      .adapter = m_startCardAdapter.get(),
      .flexGrow = 1.0F,
      .configure = [](VirtualGridView& grid) { grid.setFillWidth(true); },
  }));
  content->addChild(std::move(body));
  container->addChild(std::move(content));

  // Footer: hairline, fill_1, avatar 32 and the full name leading; Files, Settings and Power trailing.
  auto footer = ui::column({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Fill1), .fillWidth = true});
  footer->addChild(ui::separator({.color = colorSpecFromRole(ColorRole::Separator), .thickness = 1.0F, .spacing = 0.0F}));
  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Tokens::spaceP12 * scale,
      .paddingH = padH,
      .minHeight = (Tokens::componentLauncherStartFooter - 1.0F) * scale,
      .fillWidth = true,
  });
  const float avatar = Tokens::componentLauncherStartAvatar * scale;
  auto avatarBox = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .fill = colorSpecFromRole(ColorRole::Fill3),
      .radius = avatar * 0.5F,
      .minWidth = avatar,
      .minHeight = avatar,
  });
  avatarBox->setMaxWidth(avatar);
  const std::string name = startUserName(m_accounts);
  avatarBox->addChild(ui::label({
      .out = &m_ui.startInitials,
      .text = initialsOf(name),
      .fontSize = Style::fontSizeCaption * scale,
      .fontWeight = FontWeight::SemiBold,
      .color = colorSpecFromRole(ColorRole::Label1),
      .maxLines = 1,
  }));
  m_ui.launcherAvatarFallback = m_ui.startInitials;
  avatarBox->addChild(ui::image({
      .out = &m_ui.launcherAvatar,
      .radius = avatar * 0.5F,
      .width = avatar,
      .height = avatar,
      .visible = false,
      .participatesInLayout = false,
  }));
  row->addChild(std::move(avatarBox));
  row->addChild(ui::label({
      .out = &m_ui.startName,
      .text = name,
      .fontSize = Tokens::typeHeadline.size * scale,
      .fontWeight = FontWeight::SemiBold,
      .color = colorSpecFromRole(ColorRole::Label1),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
      .flexGrow = 1.0F,
  }));
  const auto iconButton = [scale](std::string glyph, std::string tooltip, std::function<void()> onClick) {
    return ui::button({
        .glyph = std::move(glyph),
        .glyphSize = Tokens::sizeSymbol * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = std::move(tooltip),
        .minWidth = Tokens::sizeControlM * scale,
        .minHeight = Tokens::sizeControlM * scale,
        .radius = Style::scaledRadius(Tokens::radiusR8, scale),
        .onClick = std::move(onClick),
    });
  };
  row->addChild(iconButton("folder", i18n::tr("launcher.start.files"), [this] {
    const std::string id = default_apps::effectiveDefault(default_apps::Category::FileManager);
    const auto it = std::ranges::find_if(m_allResults, [&id](const LauncherResult& r) {
      return !id.empty() && r.desktopActionId.empty() && r.desktopEntryPath.ends_with("/" + id);
    });
    if (it != m_allResults.end()) {
      const auto index = std::ranges::find(m_results, it->id, &LauncherResult::id);
      if (index != m_results.end()) {
        activateAt(static_cast<std::size_t>(index - m_results.begin()));
      }
    }
  }));
  row->addChild(iconButton("settings", i18n::tr("launcher.start.settings"), [] {
    PanelManager::instance().close();
    PanelManager::instance().openSettingsWindow();
  }));
  row->addChild(iconButton("power", i18n::tr("launcher.start.power"), [this] { showSessionActions(); }));
  footer->addChild(std::move(row));
  container->addChild(std::move(footer));
  setRoot(std::move(container));
}

void LauncherPanel::rebuildStartHome() {
  m_startPinned.clear();
  if (m_config != nullptr) {
    for (const DesktopEntry& entry : shell::dock::pinned_apps::resolveEntries(m_config->config().shell.launcher.pinned)) {
      const auto it = std::ranges::find_if(m_allResults, [&entry](const LauncherResult& r) {
        return r.providerId == kApplicationsProviderId && r.desktopActionId.empty() && r.desktopEntryPath == entry.path;
      });
      if (it != m_allResults.end()) {
        m_startPinned.push_back(*it);
      }
    }
  }
  m_startCards.clear();
  for (const LauncherCategory& category : m_currentCategories) {
    LauncherStartCard card{.category = category.label, .label = category.label, .icons = {}};
    std::size_t n = 0;
    bool any = false;
    for (const LauncherResult& r : m_allResults) {
      if (r.category != category.label) {
        continue;
      }
      any = true;
      if (!r.iconPath.empty() && n < card.icons.size()) {
        card.icons[n++] = r.iconPath;
      }
    }
    if (any) {
      m_startCards.push_back(std::move(card));
    }
  }
  if (m_ui.startPinnedGrid != nullptr) {
    m_ui.startPinnedGrid->notifyDataChanged();
  }
  if (m_ui.startCardGrid != nullptr) {
    m_ui.startCardGrid->notifyDataChanged();
  }
}

void LauncherPanel::syncStartSheet() {
  if (m_ui.startPinnedGrid == nullptr || m_ui.body == nullptr) {
    return;
  }
  const float scale = contentScale();
  const bool home = m_query.empty() && m_view.providerViewId().empty();
  const bool category = m_activeCategoryType == Category;
  const auto show = [](Node* node, bool visible) {
    node->setVisible(visible);
    node->setParticipatesInLayout(visible);
  };

  const auto columns = static_cast<std::size_t>(Tokens::componentLauncherStartColumns);
  const auto maxRows = static_cast<std::size_t>(Tokens::componentLauncherStartRows);
  const std::size_t allRows = rowsFor(m_startPinned.size(), columns);
  const std::size_t rows = m_startShowAllPins ? allRows : std::min(allRows, maxRows);
  const bool pinned = home && !category && rows > 0;
  show(m_ui.startPinnedHead, pinned);
  show(m_ui.startPinnedGrid, pinned);
  show(m_ui.startShowAll, allRows > maxRows);
  m_ui.startShowAll->setText(i18n::tr(m_startShowAllPins ? "launcher.start.show-less" : "launcher.start.show-all"));
  m_ui.startShowAll->setGlyph(m_startShowAllPins ? "chevron-up" : "chevron-down");
  const float tile = Tokens::componentLauncherStartTile * scale;
  const float gridHeight = static_cast<float>(rows) * tile + static_cast<float>(rows > 0 ? rows - 1 : 0) * Tokens::spaceP4 * scale;
  m_ui.startPinnedGrid->setMinHeight(gridHeight);
  m_ui.startPinnedGrid->setMaxHeight(gridHeight);

  show(m_ui.startAllHead, home);
  show(m_ui.startBack, home && category);
  if (category) {
    const auto card = std::ranges::find(m_startCards, m_activeCategory, &LauncherStartCard::category);
    m_ui.startBack->setText(card != m_startCards.end() ? card->label : m_activeCategory);
  }
  m_ui.startViewButton->setText(i18n::tr("launcher.start.view", "view", startViewLabel(startView())));
  const bool cards = home && !category && startView() == StartView::Category;
  show(m_ui.startCardGrid, cards);
  show(m_ui.body, !cards);
}

void LauncherPanel::setStartView(ShellConfig::LauncherStartView view) {
  if (m_config == nullptr || view == startView()) {
    return;
  }
  (void)m_config->setOverride(
      {"shell", "launcher", "start_view"}, std::string(startViewOption(view).key)
  );
  m_activeCategoryType = All;
  m_activeCategory.clear();
  applyActiveCategory();
}

void LauncherPanel::openStartViewMenu() {
  if (m_ui.startViewButton == nullptr) {
    return;
  }
  std::vector<ContextMenuControlEntry> entries;
  for (std::size_t i = 0; i < kStartViews.size(); ++i) {
    entries.push_back(ContextMenuControlEntry{
        .id = static_cast<std::int32_t>(i),
        .label = startViewLabel(kStartViews[i]),
        .checkmark = kStartViews[i] == startView(),
    });
  }
  float x = 0.0F;
  float y = 0.0F;
  Node::absolutePosition(m_ui.startViewButton, x, y);
  (void)openMenu(std::move(entries), x, y + m_ui.startViewButton->height(), [this](const ContextMenuControlEntry& entry) {
    if (entry.id >= 0 && static_cast<std::size_t>(entry.id) < kStartViews.size()) {
      setStartView(kStartViews[static_cast<std::size_t>(entry.id)]);
    }
  });
}
