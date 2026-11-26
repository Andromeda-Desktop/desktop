#pragma once

#include "shell/launcher/launcher_panel_detail.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

// One card of the Start sheet's All by category: the category and up to four of its app icons.
struct LauncherStartCard {
  std::string category;
  std::string label;
  std::array<std::string, 4> icons;
};

namespace launcher_start {
  // Card: square, r 12, view fill with a hairline; icons 36 in a 2 x 2 grid at p 16, g 12; name under it at g 8.
  [[nodiscard]] inline float cardInset(float scale) { return Tokens::spaceP16 * scale; }
  [[nodiscard]] inline float cardGap(float scale) { return Tokens::spaceP12 * scale; }
  [[nodiscard]] inline float cardLabelGap(float scale) { return Tokens::spaceP8 * scale; }
  [[nodiscard]] inline float cardLabelHeight(float scale) { return Tokens::typeFootnote.line * scale; }
  [[nodiscard]] inline float cardCellHeight(float cardWidth, float scale) {
    return cardWidth + cardLabelGap(scale) + cardLabelHeight(scale);
  }
} // namespace launcher_start

class LauncherStartCardTile final : public Node {
public:
  explicit LauncherStartCardTile(float scale) : m_scale(scale) {
    addChild(ui::box({
        .out = &m_card,
        .fill = colorSpecFromRole(ColorRole::View),
        .border = colorSpecFromRole(ColorRole::Stroke),
        .borderWidth = 1.0F,
        .radius = Style::scaledRadius(Tokens::radiusR12, scale),
    }));
    for (auto& icon : m_icons) {
      addChild(ui::image({.out = &icon, .visible = false}));
    }
    addChild(ui::label({
        .out = &m_label,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
        .configure = [](Label& label) { label.setTextAlign(TextAlign::Center); },
    }));
  }

  void bind(Renderer& renderer, const LauncherStartCard& card, float width, bool hovered) {
    auto& accessible = accessibleState();
    accessible.role = AccessibleRole::ListItem;
    accessible.name = card.label;
    accessible.actionName = "activate";
    m_paths = card.icons;
    m_card->setFill(colorSpecFromRole(hovered ? ColorRole::Fill1 : ColorRole::View));
    m_label->setText(card.label);
    m_label->setMaxWidth(width);
    place(width);
    for (std::size_t i = 0; i < m_icons.size(); ++i) {
      load(renderer, i);
    }
  }

protected:
  void doLayout(Renderer& renderer) override {
    place(width());
    for (std::size_t i = 0; i < m_icons.size(); ++i) {
      load(renderer, i);
    }
    Node::doLayout(renderer);
    m_label->measure(renderer);
    m_label->setPosition((width() - m_label->width()) * 0.5F, width() + launcher_start::cardLabelGap(m_scale));
  }

private:
  void place(float width) {
    m_card->setPosition(0.0F, 0.0F);
    m_card->setSize(width, width);
    const float inset = launcher_start::cardInset(m_scale);
    const float cell = std::max(0.0F, (width - 2.0F * inset - launcher_start::cardGap(m_scale)) * 0.5F);
    const float icon = Tokens::componentLauncherStartCardIcon * m_scale;
    for (std::size_t i = 0; i < m_icons.size(); ++i) {
      const float cx = inset + (i % 2 == 0 ? 0.0F : cell + launcher_start::cardGap(m_scale)) + cell * 0.5F;
      const float cy = inset + (i < 2 ? 0.0F : cell + launcher_start::cardGap(m_scale)) + cell * 0.5F;
      m_icons[i]->setSize(icon, icon);
      m_icons[i]->setPosition(cx - icon * 0.5F, cy - icon * 0.5F);
    }
  }

  void load(Renderer& renderer, std::size_t i) {
    if (m_paths[i].empty()) {
      m_icons[i]->clear(renderer);
      m_icons[i]->setVisible(false);
      return;
    }
    const int target = static_cast<int>(std::round(Tokens::componentLauncherStartCardIcon * m_scale));
    m_icons[i]->setVisible(m_icons[i]->load(renderer, m_paths[i], {.targetSize = target, .mipmap = true}) == ImageLoad::Ready);
  }

  float m_scale;
  Box* m_card = nullptr;
  std::array<Image*, 4> m_icons{};
  std::array<std::string, 4> m_paths;
  Label* m_label = nullptr;
};

class LauncherStartCardAdapter final : public VirtualGridAdapter {
public:
  LauncherStartCardAdapter(const std::vector<LauncherStartCard>* cards, float scale, std::function<void(std::size_t)> onActivate)
      : m_cards(cards), m_scale(scale), m_onActivate(std::move(onActivate)) {}

  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setScale(float scale) { m_scale = scale; }

  [[nodiscard]] std::size_t itemCount() const override { return m_cards->size(); }
  [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<LauncherStartCardTile>(m_scale); }
  void bindTile(Node& tile, std::size_t index, bool /*selected*/, bool hovered) override {
    if (m_renderer == nullptr || index >= m_cards->size()) {
      return;
    }
    static_cast<LauncherStartCardTile&>(tile).bind(*m_renderer, (*m_cards)[index], tile.width(), hovered);
  }
  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

private:
  const std::vector<LauncherStartCard>* m_cards;
  Renderer* m_renderer = nullptr;
  float m_scale;
  std::function<void(std::size_t)> m_onActivate;
};
