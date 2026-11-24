#include "ui/controls/alert.h"

#include "design_tokens.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

namespace ui {

  std::unique_ptr<Flex> alertContent(const AlertProps& props) {
    const float s = props.scale;
    auto text = ui::column(
        {.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s},
        ui::label({
            .text = props.title,
            .fontSize = Tokens::typeTitle3.size * s,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::Label1),
            .maxLines = 3,
            .ellipsize = props.titleEllipsize,
        })
    );
    if (!props.body.empty()) {
      text->addChild(ui::label({
          .text = props.body,
          .fontSize = Tokens::typeParagraph.size * s,
          .color = colorSpecFromRole(ColorRole::Label2),
          .maxLines = 8,
      }));
    }

    const bool sideBySide = props.buttons.size() <= 2;
    auto buttons = sideBySide ? ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .fillWidth = true})
                              : ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
    const float height = Tokens::sizeControlM * s;
    for (std::size_t i = 0; i < props.buttons.size(); ++i) {
      const AlertButtonProps& spec = props.buttons[i];
      const bool capsule = spec.variant == ButtonVariant::Primary || spec.variant == ButtonVariant::Destructive;
      buttons->addChild(ui::button({
          .out = i == props.defaultIndex ? props.defaultButton : nullptr,
          .text = spec.text,
          .fontSize = Style::fontSizeBody * s,
          .variant = spec.variant,
          .minHeight = height,
          .paddingV = 0.0F,
          .paddingH = Style::spaceMd * s,
          .radius = capsule ? height * 0.5F : Style::scaledRadiusMd(s),
          .flexGrow = sideBySide ? 1.0F : 0.0F,
          .onClick = spec.onClick,
      }));
    }
    return ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP24 * s}, std::move(text), std::move(buttons));
  }

  std::unique_ptr<Flex> alertFrame(float scale, float width) {
    return ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * scale,
                       .padding = Tokens::componentDialogAlertPadding * scale, .fill = colorSpecFromRole(ColorRole::Popover),
                       .radius = Style::scaledRadius(Tokens::componentDialogAlertRadius, scale), .width = width * scale});
  }

  std::unique_ptr<Flex> alertCard(const AlertProps& props) {
    auto card = alertFrame(props.scale, Tokens::componentDialogAlertWidth);
    card->addChild(alertContent(props));
    return card;
  }

} // namespace ui
