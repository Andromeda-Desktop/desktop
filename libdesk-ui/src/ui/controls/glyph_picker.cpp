#include "ui/controls/glyph_picker.h"

#include "core/input/cursor_shape.h"
#include "i18n/i18n.h"
#include "render/text/glyph_registry.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <memory>
#include <set>
#include <unordered_set>

class GlyphGridAdapter : public VirtualGridAdapter {
public:
  struct Entry {
    std::string name;
    char32_t codepoint = 0;
    std::string category;
  };

  GlyphGridAdapter(float chromeScale) : m_chromeScale(chromeScale) {
    const std::size_t tablerCount = GlyphRegistry::tablerGlyphCount();
    const auto& aliases = GlyphRegistry::aliases();
    std::set<std::string> categories;

    std::unordered_set<std::string> seen;
    seen.reserve(tablerCount + aliases.size());
    m_master.reserve(tablerCount + aliases.size());

    for (const auto& [name, target] : aliases) {
      if (seen.insert(name).second) {
        if (const auto glyph = GlyphRegistry::findTablerGlyph(target)) {
          m_master.push_back({name, glyph->codepoint, std::string(glyph->category)});
          categories.emplace(glyph->category);
        }
      }
    }
    for (std::size_t i = 0; i < tablerCount; ++i) {
      const GlyphRegistry::TablerGlyph glyph = GlyphRegistry::tablerGlyphAt(i);
      if (seen.emplace(glyph.name).second) {
        m_master.push_back({std::string(glyph.name), glyph.codepoint, std::string(glyph.category)});
        categories.emplace(glyph.category);
      }
    }
    std::ranges::sort(m_master, {}, &Entry::name);
    m_categories.assign(categories.begin(), categories.end());

    m_visible.reserve(m_master.size());
    rebuildVisible({}, {});
  }

  [[nodiscard]] std::size_t itemCount() const override { return m_visible.size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    Glyph* glyphRaw = nullptr;
    auto tile = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .padding = 0.0F,
        .configure = [this](Flex& flex) {
          flex.setRadius(Style::scaledRadiusMd(m_chromeScale));
          flex.clearBorder();
        },
    });
    tile->addChild(
        ui::glyph({
            .out = &glyphRaw,
            .glyphSize = 28.0F * m_chromeScale,
        })
    );
    tile->setUserData(glyphRaw);
    return tile;
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (index >= m_visible.size()) {
      tile.setVisible(false);
      return;
    }
    const Entry& entry = m_master[m_visible[index]];

    auto* flex = static_cast<Flex*>(&tile);
    auto* glyph = static_cast<Glyph*>(flex->userData());

    if (selected) {
      flex->setFill(colorSpecFromRole(ColorRole::Accent));
      if (glyph != nullptr) {
        glyph->setColor(colorSpecFromRole(ColorRole::OnAccent));
      }
    } else if (hovered) {
      flex->setFill(colorSpecFromRole(ColorRole::Fill3));
      if (glyph != nullptr) {
        glyph->setColor(colorSpecFromRole(ColorRole::Label1));
      }
    } else {
      flex->clearFill();
      if (glyph != nullptr) {
        glyph->setColor(colorSpecFromRole(ColorRole::Label1));
      }
    }
    if (glyph != nullptr) {
      glyph->setGlyph(entry.name);
    }
  }

  void rebuildVisible(std::string_view filter, std::string_view category) {
    m_visible.clear();
    const std::string needle = StringUtils::toLower(filter);
    const bool filterByName = !needle.empty();
    const bool filterByCategory = !category.empty();
    for (std::size_t i = 0; i < m_master.size(); ++i) {
      if (filterByCategory && m_master[i].category != category) {
        continue;
      }
      if (!filterByName || m_master[i].name.contains(needle)) {
        m_visible.push_back(i);
      }
    }
  }

  [[nodiscard]] const std::vector<std::string>& categories() const noexcept { return m_categories; }

  [[nodiscard]] std::optional<std::size_t> indexOfName(std::string_view name) const {
    for (std::size_t i = 0; i < m_visible.size(); ++i) {
      if (m_master[m_visible[i]].name == name) {
        return i;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] const Entry& entryAt(std::size_t visibleIndex) const { return m_master[m_visible[visibleIndex]]; }

private:
  float m_chromeScale = 1.0F;
  std::vector<Entry> m_master;
  std::vector<std::size_t> m_visible;
  std::vector<std::string> m_categories;
};

void GlyphPicker::apply() {
  if (!m_onApply) {
    return;
  }
  const auto result = currentResult();
  if (result.has_value()) {
    m_onApply(*result);
  }
}

GlyphPicker::GlyphPicker(float chromeScale) : m_chromeScale(std::max(0.1F, chromeScale)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceMd * m_chromeScale);

  // Rulebook 9.10: header h 40, title-3 at inset 12; the host pads the sheet 12.
  auto headerRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * m_chromeScale,
      .minHeight = Style::controlHeightLg * m_chromeScale,
  });
  headerRow->setPadding(0.0F, 0.0F, 0.0F, Style::spaceMd * m_chromeScale);
  headerRow->addChild(
      ui::label({
          .out = &m_title,
          .text = i18n::tr("ui.dialogs.glyph-picker.title"),
          .fontSize = Tokens::typeTitle3.size * m_chromeScale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::Label1),
      })
  );
  headerRow->addChild(ui::spacer());
  if (Style::closeButtonsEnabled()) {
    headerRow->addChild(
        ui::button({
            .glyph = "close",
            .glyphSize = Style::fontSizeBody * m_chromeScale,
            .variant = ButtonVariant::Default,
            .minWidth = Style::controlHeight * m_chromeScale,
            .minHeight = Style::controlHeight * m_chromeScale,
            .padding = Style::spaceXs * m_chromeScale,
            .radius = Style::scaledRadiusMd(m_chromeScale),
            .onClick = [this] {
              if (m_onCancel) {
                m_onCancel();
              }
            },
        })
    );
  }
  addChild(std::move(headerRow));

  m_adapter = std::make_unique<GlyphGridAdapter>(m_chromeScale);
  m_categoryOptions.reserve(m_adapter->categories().size() + 1);
  m_categoryOptions.push_back(i18n::tr("ui.dialogs.glyph-picker.all-categories"));
  for (const auto& category : m_adapter->categories()) {
    m_categoryOptions.push_back(category);
  }

  addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * m_chromeScale,
          },
          ui::searchCapsule({
              .out = &m_searchInput,
              .placeholder = i18n::tr("ui.dialogs.glyph-picker.search-placeholder"),
              .height = Tokens::sizeControlL,
              .scale = m_chromeScale,
              .flexGrow = 1.0F,
              .clearButton = true,
              .onChange = [this](const std::string& value) { applyFilter(value); },
          }),
          ui::select({
              .out = &m_categorySelect,
              .options = m_categoryOptions,
              .selectedIndex = 0,
              .fontSize = Style::fontSizeBody * m_chromeScale,
              .controlHeight = Tokens::sizeControlL * m_chromeScale,
              .horizontalPadding = Style::spaceMd * m_chromeScale,
              .glyphSize = Style::fontSizeBody * m_chromeScale,
              .width = 180.0F * m_chromeScale,
              .onSelectionChanged = [this](std::size_t, std::string_view) {
                const std::string filter = m_searchInput != nullptr ? m_searchInput->value() : std::string{};
                applyFilter(filter);
              },
          })
      )
  );

  addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .contentScale = m_chromeScale,
          .minCellWidth = 56.0F * m_chromeScale,
          .squareCells = true,
          .columnGap = Style::spaceXs * m_chromeScale,
          .rowGap = Style::spaceXs * m_chromeScale,
          .overscanRows = 2,
          .itemCursorShape = CursorShape::Pointer,
          .adapter = m_adapter.get(),
          .flexGrow = 1.0F,
          .onSelectionChanged = [this](std::optional<std::size_t>) { applySelectionToButton(); },
      })
  );

  addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .justify = FlexJustify::End,
              .gap = Style::spaceSm * m_chromeScale,
          },
          ui::button({
              .text = i18n::tr("common.actions.cancel"),
              .variant = ButtonVariant::Secondary,
              .minWidth = Tokens::componentDialogButtonMinWidth * m_chromeScale,
              .minHeight = Style::controlHeightLg * m_chromeScale,
              .paddingH = Tokens::spaceP20 * m_chromeScale,
              .radius = Style::controlHeightLg * m_chromeScale / 2.0F,
              .onClick =
                  [this] {
                    if (m_onCancel) {
                      m_onCancel();
                    }
                  },
          }),
          ui::button({
              .out = &m_applyButton,
              .text = i18n::tr("common.actions.apply"),
              .variant = ButtonVariant::Primary,
              .minWidth = Tokens::componentDialogButtonMinWidth * m_chromeScale,
              .minHeight = Style::controlHeightLg * m_chromeScale,
              .paddingH = Tokens::spaceP20 * m_chromeScale,
              .radius = Style::controlHeightLg * m_chromeScale / 2.0F,
              .onClick = [this] { apply(); },
          })
      )
  );

  applySelectionToButton();
}

GlyphPicker::~GlyphPicker() {
  // Detach the adapter before m_grid (a child) gets destroyed by ~Node, since
  // VirtualGridView's pool tiles were minted by m_adapter and reference it.
  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
}

void GlyphPicker::setTitle(std::string_view title) {
  if (m_title != nullptr) {
    m_title->setText(title.empty() ? i18n::tr("ui.dialogs.glyph-picker.title") : std::string(title));
  }
}

void GlyphPicker::setInitialGlyph(std::optional<std::string> name) {
  m_pendingInitialGlyph = std::move(name);
  m_pendingInitialApplied = false;
  markLayoutDirty();
}

InputArea* GlyphPicker::initialFocusArea() const noexcept {
  return m_searchInput != nullptr ? m_searchInput->inputArea() : nullptr;
}

void GlyphPicker::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_searchInput != nullptr) {
    m_searchInput->setEnabled(enabled);
  }
  if (m_categorySelect != nullptr) {
    m_categorySelect->setEnabled(enabled);
  }
  if (m_applyButton != nullptr) {
    m_applyButton->setEnabled(enabled);
  }
  setOpacity(enabled ? 1.0F : 0.55F);
}

std::optional<GlyphPickerResult> GlyphPicker::currentResult() const {
  if (m_grid == nullptr || m_adapter == nullptr) {
    return std::nullopt;
  }
  const auto idx = m_grid->selectedIndex();
  if (!idx.has_value() || *idx >= m_adapter->itemCount()) {
    return std::nullopt;
  }
  const auto& entry = m_adapter->entryAt(*idx);
  return GlyphPickerResult{.name = entry.name, .codepoint = entry.codepoint};
}

void GlyphPicker::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);

  if (!m_pendingInitialApplied && m_pendingInitialGlyph.has_value() && m_grid != nullptr && m_adapter != nullptr) {
    if (const auto idx = m_adapter->indexOfName(*m_pendingInitialGlyph); idx.has_value()) {
      m_grid->setSelectedIndex(idx);
      m_grid->scrollToIndex(*idx);
    }
    m_pendingInitialApplied = true;
  }
}

LayoutSize GlyphPicker::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void GlyphPicker::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void GlyphPicker::applyFilter(const std::string& filter) {
  if (m_adapter == nullptr || m_grid == nullptr) {
    return;
  }
  const std::string category = m_categorySelect != nullptr && m_categorySelect->selectedIndex() > 0
      ? m_categoryOptions[m_categorySelect->selectedIndex()]
      : std::string{};
  const auto previousResult = currentResult();
  m_adapter->rebuildVisible(filter, category);
  // Drop selection if the previously selected name is no longer visible.
  if (previousResult.has_value()) {
    if (const auto idx = m_adapter->indexOfName(previousResult->name); idx.has_value()) {
      m_grid->setSelectedIndex(idx);
    } else {
      m_grid->setSelectedIndex(std::nullopt);
    }
  }
  m_grid->notifyDataChanged();
  m_grid->scrollView().setScrollOffset(0.0F);
}

void GlyphPicker::applySelectionToButton() {
  if (m_applyButton == nullptr) {
    return;
  }
  const bool hasSelection = m_grid != nullptr && m_grid->selectedIndex().has_value();
  m_applyButton->setEnabled(hasSelection);
}

float GlyphPicker::preferredDialogWidth(float scale) { return 540.0F * std::max(0.1F, scale); }

float GlyphPicker::preferredDialogHeight(float scale) { return 540.0F * std::max(0.1F, scale); }
