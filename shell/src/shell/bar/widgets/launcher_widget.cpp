#include "shell/bar/widgets/launcher_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "ui/controls/box.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <cmath>
#include <memory>

LauncherWidget::LauncherWidget(wl_output* /*output*/, Options options)
    : m_barGlyphId(std::move(options.glyph)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)),
      m_iconSize(options.iconSize), m_cellSize(options.cellSize), m_pillWidth(options.pillWidth) {}

void LauncherWidget::create() {
  auto area = ui::inputArea({});
  area->addChild(ui::box({.out = &m_lens, .fill = colorSpecFromRole(ColorRole::Fill3), .visible = false}));

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_barGlyphId,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)),
        })
    );
  }
  if (m_pillWidth > 0) {
    // A field on the bar glass (concept-wm.html .pill): the glass card fill and edge, so it stays light in dark mode.
    m_lens->setFill(colorSpecFromRole(ColorRole::GlassCard));
    m_lens->setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 1.0F);
    m_lens->setVisible(true);
    area->addChild(ui::label({
        .out = &m_label,
        .text = i18n::tr("launcher.search-placeholder"),
        .fontSize = Style::fontSizeBody * m_contentScale,
        .color = colorSpecFromRole(ColorRole::Label2),
        .maxLines = 1,
    }));
  }

  setRoot(std::move(area));
}

void LauncherWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr) {
    return;
  }

  if (m_image != nullptr) {
    // The image draws at baseGlyphSize x scale: icon_size rescales it.
    const float scale = m_iconSize > 0 ? m_contentScale * static_cast<float>(m_iconSize) / Style::baseGlyphSize
                                       : m_contentScale;
    widget_custom_image::sync(*m_image, renderer, m_customImage, scale, widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)));
    fitCell(*node, *m_image);
  } else if (m_glyph != nullptr) {
    m_glyph->setGlyphSize((m_iconSize > 0 ? static_cast<float>(m_iconSize) : Style::baseGlyphSize) * m_contentScale);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)));
    m_glyph->measure(renderer);
    if (m_label != nullptr) {
      m_label->setFontSize(Style::fontSizeBody * m_contentScale);
      m_label->measure(renderer);
    }
    fitCell(*node, *m_glyph);
  }
}

void LauncherWidget::fitCell(Node& node, Node& icon) const {
  if (m_label != nullptr) {
    // Search pill: control height, glyph inset 12, text 8 after it, cut at the pill end.
    const float h = Style::controlHeight * m_contentScale;
    const float w = static_cast<float>(m_pillWidth) * m_contentScale;
    const float inset = Tokens::spaceP12 * m_contentScale;
    const float gap = Style::spaceSm * m_contentScale;
    icon.setPosition(inset, std::round((h - icon.height()) * 0.5F));
    const float textX = inset + icon.width() + gap;
    m_label->setMaxWidth(std::max(0.0F, w - textX - inset));
    m_label->setPosition(textX, std::round((h - m_label->height()) * 0.5F));
    node.setSize(w, h);
    m_lens->setSize(w, h);
    m_lens->setRadius(h * 0.5F);
    return;
  }
  const float cell = static_cast<float>(m_cellSize) * m_contentScale;
  const float w = std::max(cell, icon.width());
  const float h = std::max(cell, icon.height());
  icon.setPosition(std::round((w - icon.width()) * 0.5F), std::round((h - icon.height()) * 0.5F));
  node.setSize(w, h);
  m_lens->setSize(w, h);
  m_lens->setRadius(std::min(w, h) * 0.5F);
}

void LauncherWidget::doUpdate(Renderer& /*renderer*/) {
  const bool open = PanelManager::instance().isOpenPanel("launcher");
  if (m_label != nullptr) {
    // The pill keeps its capsule; an open launcher deepens it.
    if (open != m_pillOpen) {
      m_pillOpen = open;
      m_lens->setFill(colorSpecFromRole(open ? ColorRole::Fill2 : ColorRole::GlassCard));
      requestRedraw();
    }
    return;
  }
  if (m_lens->visible() != open) {
    m_lens->setVisible(open);
    requestRedraw();
  }
}
