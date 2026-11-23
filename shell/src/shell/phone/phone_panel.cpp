#include "shell/phone/phone_panel.h"

#include "dbus/link/link_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "shell/phone/phone_state.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <memory>

namespace {

  constexpr float kActionHeight = 64.0F;
  // The actions' full names; their labels are short to fit four in a row.
  constexpr std::array kTooltips{
      "bar.widgets.phone.ring",  "bar.widgets.phone.send-clipboard", "bar.widgets.phone.dnd",
      "bar.widgets.phone.ringer", "bar.widgets.phone.wifi",          "bar.widgets.phone.bluetooth",
      "bar.widgets.phone.torch",
  };
  constexpr std::array<const char*, 3> kToggleCommands{"set-wifi", "set-bluetooth", "set-torch"};

} // namespace

float PhonePanel::measureContentHeight(Renderer& renderer, float contentWidth) {
  if (m_rootLayout == nullptr) {
    return 0.0F;
  }
  m_rootLayout->setSize(contentWidth, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(contentWidth);
  return m_rootLayout->measure(renderer, constraints).height;
}

void PhonePanel::create() {
  const float scale = contentScale();
  auto layout = ui::column({.out = &m_rootLayout, .align = FlexAlign::Stretch, .gap = Style::spaceMd * scale});
  layout->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
          ui::glyph({
              .out = &m_glyph,
              .glyph = "device-mobile",
              .glyphSize = Style::fontSizeTitle * 1.5F * scale,
              .color = colorSpecFromRole(ColorRole::AccentText),
          }),
          ui::column(
              {.align = FlexAlign::Stretch, .gap = Style::spaceXs * 0.5F * scale, .flexGrow = 1.0F},
              ui::label({
                  .out = &m_name,
                  .fontSize = Style::fontSizeTitle * scale,
                  .fontWeight = FontWeight::Bold,
                  .color = colorSpecFromRole(ColorRole::Label1),
                  .maxLines = 1,
              }),
              ui::label({
                  .out = &m_state,
                  .fontSize = Style::fontSizeBody * scale,
                  .color = colorSpecFromRole(ColorRole::Label2),
                  .maxLines = 1,
              })
          )
      )
  );
  auto actions = ui::row({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
  auto toggles = ui::row({.out = &m_toggles, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
  for (std::uint8_t i = 0; i < ActionCount; ++i) {
    (i < kFirstToggle ? actions : toggles)
        ->addChild(
            ui::button({
                .out = &m_actions[i],
                .text = std::string(),
                .glyph = "bell",
                .fontSize = Style::fontSizeCaption * scale,
                .glyphSize = Style::fontSizeTitle * scale,
                .contentAlign = ButtonContentAlign::Center,
                .surfaceOpacity = panelCardOpacity(),
                .tooltip = i18n::tr(kTooltips[i]),
                .minHeight = kActionHeight * scale,
                .gap = Style::spaceXs * scale,
                .radius = Style::scaledRadiusMd(scale),
                .flexGrow = 1.0F,
                .onClick = [this, i]() { act(static_cast<Action>(i)); },
                .configure =
                    [](Button& control) {
                      control.setDirection(FlexDirection::Vertical);
                      control.setAlign(FlexAlign::Center);
                      control.setJustify(FlexJustify::Center);
                    },
            })
        );
  }
  // An empty fourth cell keeps the toggles on the grid of the actions above.
  toggles->addChild(ui::spacer());
  layout->addChild(std::move(actions));
  layout->addChild(std::move(toggles));
  // Rulebook 9.13: an error shows where it happened, with the action that fixes it.
  layout->addChild(
      ui::row(
          {.out = &m_error, .align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::glyph({
              .glyph = "alert-circle",
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::DangerText),
          }),
          ui::label({
              .out = &m_errorText,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::Label1),
              .maxLines = 2,
              .flexGrow = 1.0F,
          }),
          ui::button({
              .out = &m_retry,
              .text = i18n::tr("bar.widgets.phone.try-again"),
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .onClick =
                  [this]() {
                    const LinkDevice* phone = m_link != nullptr ? phone::primary(*m_link) : nullptr;
                    const LinkCommandState* state = phone != nullptr ? m_link->commandState(phone->id) : nullptr;
                    if (state != nullptr && state->phase == LinkCommandState::Phase::Failed) {
                      m_link->sendSystemCommand(phone->id, state->command, state->value);
                    }
                  },
          })
      )
  );
  layout->addChild(
      ui::button({
          .out = &m_open,
          .text = i18n::tr("bar.widgets.phone.open-link"),
          .glyph = "external-link",
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .variant = ButtonVariant::Ghost,
          .onClick = []() {
            LinkService::openApp();
            PanelManager::instance().close();
          },
      })
  );
  setRoot(std::move(layout));
  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }
}

void PhonePanel::onClose() {
  // A failed command's reason belongs to the press that caused it, not to the next opening.
  if (const LinkDevice* phone = m_link != nullptr ? phone::primary(*m_link) : nullptr; phone != nullptr) {
    m_link->dismissCommandError(phone->id);
  }
  clearReleasedRoot();
  m_rootLayout = nullptr;
  m_glyph = nullptr;
  m_name = nullptr;
  m_state = nullptr;
  m_actions.fill(nullptr);
  m_toggles = nullptr;
  m_error = nullptr;
  m_errorText = nullptr;
  m_retry = nullptr;
  m_open = nullptr;
}

void PhonePanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  for (Button* button : m_actions) {
    button->updateInputArea();
  }
  m_retry->updateInputArea();
  m_open->updateInputArea();
}

void PhonePanel::doUpdate(Renderer& /*renderer*/) {
  if (m_rootLayout == nullptr || m_link == nullptr) {
    return;
  }
  const LinkDevice* phone = phone::primary(*m_link);
  const LinkStatus* status = phone != nullptr ? m_link->status(phone->id) : nullptr;
  const LinkControls* controls = phone != nullptr && phone->connected ? m_link->controls(phone->id) : nullptr;
  const bool connected = phone != nullptr && phone->connected;
  const bool ringing = connected && m_link->phoneRinging(phone->id);

  if (phone == nullptr) {
    m_glyph->setGlyph("device-mobile-off");
    (void)m_name->setText(i18n::tr(m_link->available() ? "bar.widgets.phone.none" : "bar.widgets.phone.unavailable"));
    (void)m_state->setText({});
  } else {
    m_glyph->setGlyph(phone::glyph(*phone, status));
    (void)m_name->setText(phone->name);
    std::string state = phone::connectionText(*m_link, *phone);
    if (status != nullptr) {
      state = phone::batteryText(*status) + " · " + state;
    }
    (void)m_state->setText(state);
  }

  const std::string ringer = controls != nullptr ? controls->ringer : std::string();
  const auto set = [&](Action action, std::string_view glyph, const std::string& text, bool enabled, bool selected) {
    m_actions[action]->setGlyph(glyph);
    m_actions[action]->setText(text);
    m_actions[action]->setEnabled(enabled);
    m_actions[action]->setSelected(selected);
  };
  set(Ring, "bell-ringing", i18n::tr(ringing ? "bar.widgets.phone.stop-ring" : "bar.widgets.phone.ring"), connected,
      ringing);
  set(Clipboard, "clipboard-copy", i18n::tr("bar.widgets.phone.clipboard"), connected, false);
  set(Dnd, "moon", i18n::tr("bar.widgets.phone.dnd-short"), controls != nullptr && controls->dnd >= 0,
      controls != nullptr && controls->dnd == 1);
  set(Ringer, phone::ringerGlyph(ringer), i18n::tr("bar.widgets.phone.ringer-" + (ringer.empty() ? "normal" : ringer)),
      !ringer.empty(), false);

  const LinkToggles* toggles = connected ? m_link->toggles(phone->id) : nullptr;
  const LinkCommandState* command = connected ? m_link->commandState(phone->id) : nullptr;
  const LinkDeviceInfo* info = connected ? m_link->info(phone->id) : nullptr;
  const std::string transport = info != nullptr ? info->transport : std::string();
  m_toggles->setVisible(toggles != nullptr);
  if (toggles != nullptr) {
    const std::array states{toggles->wifi, toggles->bluetooth, toggles->torch};
    // Turning off the radio Link runs on would end the connection: the key stays on and says why, and linkd's
    // refusal shows inline when pressed.
    const std::array<bool, 3> carries{transport == "wifi" || transport == "nearby", transport == "bluetooth", false};
    const std::array<std::pair<const char*, const char*>, 3> glyphs{
        std::pair{"wifi", "wifi-off"}, std::pair{"bluetooth", "bluetooth-off"}, std::pair{"bulb", "bulb-off"}
    };
    for (std::uint8_t i = 0; i < 3; ++i) {
      const auto action = static_cast<Action>(kFirstToggle + i);
      const bool pending = command != nullptr && command->phase == LinkCommandState::Phase::Pending &&
                           command->command == kToggleCommands[i];
      // A pending command shows the state it asks for, and takes no second press.
      const bool on = pending ? command->value == 1 : states[i] == 1;
      const bool locked = on && carries[i];
      set(action, on ? glyphs[i].first : glyphs[i].second, i18n::tr(kTooltips[action]), states[i] >= 0 && !pending,
          on);
      m_actions[action]->setTooltip(
          i18n::tr(locked ? (i == 0 ? "bar.widgets.phone.wifi-carries-link" : "bar.widgets.phone.bluetooth-carries-link")
                          : kTooltips[action])
      );
    }
  }
  const bool failed = command != nullptr && command->phase == LinkCommandState::Phase::Failed;
  m_error->setVisible(failed);
  (void)m_errorText->setText(failed ? command->error : std::string());
  m_rootLayout->markPaintDirty();
}

void PhonePanel::act(Action action) {
  const LinkDevice* phone = m_link != nullptr ? phone::primary(*m_link) : nullptr;
  if (phone == nullptr || !phone->connected) {
    return;
  }
  const LinkControls* controls = m_link->controls(phone->id);
  switch (action) {
  case Ring:
    m_link->ring(phone->id, !m_link->phoneRinging(phone->id));
    break;
  case Clipboard:
    m_link->shareClipboard(phone->id);
    break;
  case Dnd:
    if (controls != nullptr) {
      m_link->sendSystemCommand(phone->id, "set-dnd", controls->dnd == 1 ? 0 : 1);
    }
    break;
  case Ringer:
    if (controls != nullptr) {
      m_link->sendSystemCommand(phone->id, "set-ringer", phone::nextRinger(controls->ringer));
    }
    break;
  case Wifi:
  case Bluetooth:
  case Torch:
    if (const LinkToggles* toggles = m_link->toggles(phone->id); toggles != nullptr) {
      const std::array states{toggles->wifi, toggles->bluetooth, toggles->torch};
      const std::uint8_t i = action - kFirstToggle;
      m_link->sendSystemCommand(phone->id, kToggleCommands[i], states[i] == 1 ? 0 : 1);
    }
    break;
  case ActionCount:
    break;
  }
}
