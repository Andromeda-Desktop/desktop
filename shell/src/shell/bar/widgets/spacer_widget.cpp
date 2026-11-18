#include "shell/bar/widgets/spacer_widget.h"

#include "ui/controls/box.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

SpacerWidget::SpacerWidget(bool verticalBar, Options options)
    : m_fixedLength(static_cast<float>(options.length)), m_drawLine(options.line), m_verticalBar(verticalBar) {}

void SpacerWidget::create() {
  auto spacer = ui::node({});
  spacer->setSize(0.0F, 0.0F);
  spacer->setHitTestVisible(false);
  if (m_drawLine) {
    spacer->addChild(ui::box({.out = &m_line, .fill = colorSpecFromRole(ColorRole::Label3)}));
  }
  setRoot(std::move(spacer));
}

void SpacerWidget::doLayout(Renderer& /*renderer*/, float /*containerWidth*/, float /*containerHeight*/) {
  if (root() != nullptr && m_line != nullptr) {
    const float along = Tokens::componentIslandDividerHeight * m_contentScale;
    m_line->setSize(m_verticalBar ? along : 1.0F, m_verticalBar ? 1.0F : along);
    root()->setSize(m_line->width(), m_line->height());
  } else if (root() != nullptr) {
    const float length = m_fixedLength * m_contentScale;
    root()->setSize(m_verticalBar ? 0.0F : length, m_verticalBar ? length : 0.0F);
  }
}
