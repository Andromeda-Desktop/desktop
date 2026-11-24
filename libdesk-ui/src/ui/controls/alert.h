#pragma once

#include "render/core/renderer.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ui {

  struct AlertButtonProps {
    std::string text;
    ButtonVariant variant = ButtonVariant::Default;
    std::function<void()> onClick;
  };

  struct AlertProps {
    std::string title;
    std::optional<TextEllipsize> titleEllipsize; // Middle keeps a file name's extension
    std::string body;
    std::string excerpt; // verbatim monospace text under the body, such as a pasted command
    std::vector<AlertButtonProps> buttons;
    std::size_t defaultIndex = 0;
    Button** defaultButton = nullptr; // receives the default button, for the host's initial focus
    float scale = 1.0F;
  };

  // Rulebook 9.10 alert content: title, body, and two buttons side by side or three and more stacked. A host that
  // draws its own card (settings modals) takes this; an app sheet takes alertCard.
  [[nodiscard]] std::unique_ptr<Flex> alertContent(const AlertProps& props);
  // The whole 9.10 alert card: alertContent on alertFrame.
  [[nodiscard]] std::unique_ptr<Flex> alertCard(const AlertProps& props);
  // The 9.10 alert card without content (solid popover, alert padding and radius, children g 8), for an alert with
  // more than title, body and buttons; it puts 24 above its buttons itself. width is unscaled.
  [[nodiscard]] std::unique_ptr<Flex> alertFrame(float scale, float width);

} // namespace ui
