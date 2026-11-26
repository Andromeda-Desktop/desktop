#include "shell/bar/widgets/battery_widget.h"

#include "dbus/power/power_profiles_service.h"

#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace {

  // An empty node that runs `onChange` when its effective palette tone changes.
  class PaletteToneWatch final : public Node {
  public:
    explicit PaletteToneWatch(std::function<void()> onChange) : m_onChange(std::move(onChange)) {
      setParticipatesInLayout(false);
      setHitTestVisible(false);
    }

  protected:
    void onPaletteToneChanged() override { m_onChange(); }

  private:
    std::function<void()> m_onChange;
  };

  // The bar updates once a minute unless a widget renders seconds, so a shorter
  // interval would never be honoured. The second of slack keeps an update that
  // lands just short of a full minute from pushing the refresh out to two.
  constexpr auto kTooltipForceRefreshInterval = std::chrono::seconds(59);


  std::string formatCompactDuration(std::int64_t seconds) {
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    std::string hourText = i18n::tr("time.units.hour-compact", "count", hours);
    std::string minuteText = i18n::tr("time.units.minute-compact", "count", minutes);
    if (hours > 0 && minutes > 0) {
      return i18n::tr("time.duration.two-parts", "first", hourText, "second", minuteText);
    }
    if (hours > 0) {
      return hourText;
    }
    if (minutes > 0) {
      return minuteText;
    }
    return i18n::tr("time.duration.less-than-minute");
  }

  // The graphic body has room for about four narrow characters, so durations of an hour or more use a clock-style
  // "2:54" instead of "2h 54m"; shorter ones keep the compact "53m" form.
  std::string formatGraphicDuration(std::int64_t seconds) {
    if (seconds < 3600) {
      return formatCompactDuration(seconds);
    }
    return std::format("{}:{:02}", seconds / 3600, (seconds % 3600) / 60);
  }

} // namespace

BatteryWidget::BatteryWidget(UPowerService* upower, PowerProfilesService* powerProfiles, Options options)
    : m_upower(upower), m_powerProfiles(powerProfiles), m_deviceSelector(std::move(options.deviceSelector)),
      m_warningThreshold(options.warningThreshold), m_warningColor(options.warningColor),
      m_displayMode(options.displayMode), m_labelContent(options.labelContent), m_showLabel(options.showLabel),
      m_hideWhenPlugged(options.hideWhenPlugged), m_hideWhenFull(options.hideWhenFull) {}

// Vertical bars are too narrow for time or rate text, so they always show the bare percentage; the
// tooltip carries the full detail. Time and rate are only known while the battery is actively charging
// or discharging, and the percentage stands in whenever the selected content has no value to show.
ColorSpec BatteryWidget::toneColor(const ColorSpec& normal) const {
  switch (m_lastTone) {
  case BatteryTone::Charging:
    return colorSpecFromRole(ColorRole::Success);
  case BatteryTone::Low:
  case BatteryTone::Critical:
    return m_warningColor;
  case BatteryTone::Saver:
    return colorSpecFromRole(ColorRole::Warning);
  default:
    return normal;
  }
}

std::string BatteryWidget::buildLabelText(int pct, const UPowerState& state) const {
  if (state.state == BatteryState::Unknown) {
    return "\u2014";
  }
  if (m_isVertical) {
    return std::to_string(pct);
  }

  switch (m_labelContent) {
  case BatteryLabelContent::Time: {
    const bool graphic = m_displayMode == BatteryDisplayMode::Graphic;
    if (state.state == BatteryState::Discharging && state.timeToEmpty > 0) {
      return graphic ? formatGraphicDuration(state.timeToEmpty) : formatCompactDuration(state.timeToEmpty);
    }
    if (state.state == BatteryState::Charging && state.timeToFull > 0) {
      return graphic ? formatGraphicDuration(state.timeToFull) : formatCompactDuration(state.timeToFull);
    }
    break;
  }
  case BatteryLabelContent::Rate:
    if (state.energyRate > 0.0) {
      if (m_displayMode == BatteryDisplayMode::Graphic) {
        return state.energyRate < 1.0 ? "<1W" : std::format("{:.0F}W", state.energyRate);
      }
      return std::format("{:.1F} W", state.energyRate);
    }
    break;
  case BatteryLabelContent::Percent:
    break;
  }
  return std::format("{}%", pct);
}

void BatteryWidget::create() {
  auto container = ui::inputArea({});
  setRoot(std::move(container));

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    createGraphicMode();
  } else if (m_displayMode == BatteryDisplayMode::Glyph) {
    createGlyphMode();
  } else {
    createLabelOnlyMode();
  }
}

void BatteryWidget::createGraphicMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::box({
          .out = &m_bodyBg,
      })
  );

  container->addChild(
      ui::box({
          .out = &m_fillRect,
      })
  );

  container->addChild(
      ui::box({
          .out = &m_terminalNub,
      })
  );

  if (m_showLabel) {
    container->addChild(
        ui::label({
            .out = &m_overlayLabel,
            .fontWeight = labelFontWeight(),
            .fontFamily = labelFontFamily(),
            .color = widgetForegroundOr(colorSpecFromRole(ColorRole::Label1)),
            .textAlign = TextAlign::Center,
        })
    );
    m_overlayLabel->setFontFeatures("tnum");
  }

  container->addChild(
      ui::glyph({
          .out = &m_overlayGlyph,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::Label1)),
          .visible = false,
      })
  );

  // syncPalette() bakes resolved colours, so a tone change (attach under the bar, glass slider) re-runs it.
  auto toneWatch = std::make_unique<PaletteToneWatch>([this] {
    syncPalette();
    requestRedraw();
  });
  root()->addChild(std::move(toneWatch));
  syncPalette();
  m_paletteConn = paletteChanged().connect([this] {
    syncPalette();
    requestRedraw();
  });
}

void BatteryWidget::createGlyphMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "battery-4",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)),
      })
  );

  container->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .visible = m_showLabel,
      })
  );
}

void BatteryWidget::createLabelOnlyMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .visible = m_showLabel,
      })
  );
}

void BatteryWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (rootNode == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  syncState(renderer);

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    layoutGraphicMode(renderer);
  } else if (m_displayMode == BatteryDisplayMode::Glyph) {
    layoutGlyphMode(renderer, containerWidth, containerHeight);
  } else {
    layoutLabelOnlyMode(renderer, containerWidth, containerHeight);
  }
}

void BatteryWidget::layoutGraphicMode(Renderer& renderer) {
  auto* rootNode = root();
  if (m_bodyBg == nullptr || m_fillRect == nullptr || m_terminalNub == nullptr || rootNode == nullptr) {
    return;
  }

  // The reference's 26 x 24 battery symbol drawn at its 16 px icon height; units are symbol units times s. Vertical
  // bars centre its body in a 22 px box (R9) and put the percent under it. Horizontal bars put the percent after it
  // and hug it with the 2-unit side margin of the 24-grid icons, so the row gaps stay equal.
  const float s = Tokens::componentIslandIcon / 24.0F * m_contentScale;
  const float boxW = m_isVertical ? 22.0F * m_contentScale : 28.3F * s;
  const float boxH = 24.0F * s;
  float labelW = 0.0F;
  float labelH = 0.0F;
  if (m_overlayLabel != nullptr && m_overlayLabel->visible()) {
    m_overlayLabel->setFontSize(Style::fontSizeCaption * fontScale());
    m_overlayLabel->measure(renderer);
    labelW = m_overlayLabel->width();
    labelH = m_overlayLabel->height();
  }
  const float gap = labelW > 0.0F ? Tokens::componentIslandGap * m_contentScale : 0.0F;
  const float rootW = m_isVertical ? std::max(boxW, labelW) : boxW + gap + labelW;
  const float boxX = m_isVertical ? std::round((rootW - boxW) * 0.5F) : 0.0F;
  if (labelW > 0.0F) {
    m_overlayLabel->setPosition(
        m_isVertical ? std::round((rootW - labelW) * 0.5F) : boxW + gap,
        m_isVertical ? boxH : std::round((boxH - labelH) * 0.5F)
    );
  }
  const float offX = m_isVertical ? boxX + boxW * 0.5F - 11.0F * s : boxX + 1.9F * s;
  if (m_overlayGlyph != nullptr && m_overlayGlyph->visible()) {
    m_overlayGlyph->setGlyphSize(13.8F * s);
    m_overlayGlyph->measure(renderer);
    m_overlayGlyph->setPosition(
        offX + 0.1F * s + (21.8F * s - m_overlayGlyph->width()) * 0.5F, (boxH - m_overlayGlyph->height()) * 0.5F
    );
  }
  m_bodyBg->setRadius(4.4F * s);
  m_bodyBg->setPosition(offX + 0.1F * s, 5.1F * s);
  m_bodyBg->setSize(21.8F * s, 13.8F * s);
  m_terminalNub->setRadius(0.9F * s);
  m_terminalNub->setPosition(offX + 22.6F * s, 9.1F * s);
  m_terminalNub->setSize(1.8F * s, 5.8F * s);
  m_fillRect->setRadius(1.5F * s);
  updateFillGeometry();
  rootNode->setSize(rootW, m_isVertical && labelW > 0.0F ? boxH + labelH : boxH);
}

void BatteryWidget::layoutGlyphMode(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }

  m_glyph->measure(renderer);

  if (m_label != nullptr && m_showLabel) {
    m_label->measure(renderer);

    if (m_isVertical) {
      const float w = std::max(m_glyph->width(), m_label->width());
      m_glyph->setPosition(std::round((w - m_glyph->width()) * 0.5F), 0.0F);
      m_label->setPosition(std::round((w - m_label->width()) * 0.5F), m_glyph->height());
      rootNode->setSize(w, m_glyph->height() + m_label->height());
    } else {
      const float h = std::max(m_glyph->height(), m_label->height());
      m_glyph->setPosition(0.0F, std::round((h - m_glyph->height()) * 0.5F));
      m_label->setPosition(m_glyph->width() + Style::spaceXs, std::round((h - m_label->height()) * 0.5F));
      rootNode->setSize(m_label->x() + m_label->width(), h);
    }
  } else {
    rootNode->setSize(m_glyph->width(), m_glyph->height());
  }
}

void BatteryWidget::layoutLabelOnlyMode(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_label == nullptr || rootNode == nullptr) {
    return;
  }

  m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * fontScale());
  m_label->measure(renderer);
  rootNode->setSize(m_label->width(), m_label->height());
}

void BatteryWidget::updateFillGeometry() {
  if (m_fillRect == nullptr || m_bodyBg == nullptr) {
    return;
  }

  const float fraction = std::clamp(m_animatedPct / 100.0F, 0.0F, 1.0F);

  const float s = m_bodyBg->height() / 13.8F;
  m_fillRect->setPosition(m_bodyBg->x() + 3.4F * s, m_bodyBg->y() + 3.4F * s);
  m_fillRect->setSize(15.0F * s * fraction, 7.0F * s);
}

void BatteryWidget::syncPalette() {
  const ColorSpec normalFg = widgetForegroundOr(colorSpecFromRole(ColorRole::Label1));
  const ColorSpec foreground = toneColor(normalFg);

  m_bodyBg->setFill(Color{0.0F, 0.0F, 0.0F, 0.0F});
  const float stroke = 1.8F * Tokens::componentIslandIcon / 24.0F * m_contentScale;
  m_bodyBg->setBorder(foreground, stroke);
  // An unknown state draws the body dashed.
  auto style = m_bodyBg->style();
  style.borderDash = m_lastTone == BatteryTone::Unknown ? stroke : 0.0F;
  m_bodyBg->setStyle(style);
  m_fillRect->setFill(foreground);
  m_terminalNub->setFill(foreground);
  if (m_overlayLabel != nullptr) {
    m_overlayLabel->setColor(foreground);
  }
  m_overlayGlyph->setColor(foreground);
}

void BatteryWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void BatteryWidget::onFrameTick(float /*deltaMs*/) { requestRedraw(); }

bool BatteryWidget::needsFrameTick() const { return m_displayMode == BatteryDisplayMode::Graphic && m_fillAnim != 0; }

void BatteryWidget::syncState(Renderer& renderer) {
  if (m_upower == nullptr) {
    return;
  }

  const auto s = m_upower->stateForDevice(m_deviceSelector);
  const bool saver = m_powerProfiles != nullptr && m_powerProfiles->powerSaverActive();
  const BatteryTone tone = batteryTone(
      s.state, static_cast<int>(std::round(s.percentage)), s.pluggedIn(), m_warningThreshold, saver
  );

  const auto now = std::chrono::steady_clock::now();
  const bool forceTimeRefresh = (m_lastTooltipRefreshTime == std::chrono::steady_clock::time_point{})
      || (now - m_lastTooltipRefreshTime >= kTooltipForceRefreshInterval);

  if (s.percentage == m_lastPct
      && s.state == m_lastState
      && s.pluggedIn() == m_lastPluggedIn
      && s.isPresent == m_lastPresent
      && s.energyRate == m_lastEnergyRate
      && s.timeToEmpty == m_lastTimeToEmpty
      && tone == m_lastTone
      && m_isVertical == m_lastVertical
      && !forceTimeRefresh) {
    return;
  }

  m_lastPct = s.percentage;
  m_lastState = s.state;
  m_lastPluggedIn = s.pluggedIn();
  m_lastPresent = s.isPresent;
  m_lastTone = tone;
  m_lastEnergyRate = s.energyRate;
  m_lastTimeToEmpty = s.timeToEmpty;
  m_lastVertical = m_isVertical;
  m_lastTooltipRefreshTime = now;

  const bool isPluggedIn = s.pluggedIn();

  const bool hasVisibleContent = m_displayMode != BatteryDisplayMode::None || m_showLabel;

  const bool showWidget = s.isPresent
      && hasVisibleContent
      && !(m_hideWhenPlugged && isPluggedIn)
      && (!m_hideWhenFull || (s.state != BatteryState::FullyCharged && s.state != BatteryState::PendingCharge));

  auto* rootNode = root();
  if (rootNode != nullptr) {
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
  }

  if (!showWidget) {
    return;
  }

  const int pct = static_cast<int>(std::round(s.percentage));

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    syncPalette();

    // Whole percent: UPower's fractional percentage changes on every poll and would redraw a sub-pixel change.
    const auto newPct = static_cast<float>(pct);
    // A 1 % step moves the fill by a fraction of a pixel: snap it instead of animating several frames.
    if (m_animations != nullptr && std::abs(m_animatedPct - newPct) > 1.5F) {
      m_animations->cancel(m_fillAnim);
      m_fillAnim = m_animations->animateSpring(
          m_animatedPct, newPct, Style::spring(Tokens::springMove),
          [this](float v) {
            m_animatedPct = v;
            updateFillGeometry();
            requestRedraw();
          },
          [this] { m_fillAnim = 0; }, this
      , 0.0F, Unit::Px);
      requestFrameTick();
    } else {
      m_animatedPct = newPct;
      updateFillGeometry();
    }

    // Graphic mode label
    if (m_overlayLabel != nullptr && m_showLabel) {
      m_overlayLabel->setText(buildLabelText(pct, s));
      m_overlayLabel->measure(renderer);
    }

    // State icon
    const char* stateGlyph = batteryToneGlyph(tone, s.state);
    if (m_overlayGlyph != nullptr && stateGlyph != nullptr) {
      m_overlayGlyph->setGlyph(stateGlyph);
      m_overlayGlyph->measure(renderer);
    }

    if (m_overlayLabel != nullptr) {
      m_overlayLabel->setVisible(m_showLabel);
    }
    if (m_overlayGlyph != nullptr) {
      m_overlayGlyph->setVisible(stateGlyph != nullptr);
    }
    // The outlined symbol draws the state glyph inside the body, where the level fill would hide it.
    m_fillRect->setVisible(stateGlyph == nullptr);
  } else if (m_displayMode == BatteryDisplayMode::Glyph) {
    const ColorSpec iconColor = toneColor(widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)));
    const ColorSpec labelColor = toneColor(widgetForegroundOr(colorSpecFromRole(ColorRole::Label1)));

    if (m_glyph != nullptr) {
      m_glyph->setGlyph(batteryGlyphName(s.percentage, s.state));
      m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
      m_glyph->setColor(iconColor);
      m_glyph->measure(renderer);
    }

    if (m_label != nullptr && m_showLabel) {
      m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * fontScale());
      m_label->setText(buildLabelText(pct, s));
      m_label->setColor(labelColor);
      m_label->measure(renderer);
    }
  } else if (m_displayMode == BatteryDisplayMode::None) {
    const ColorSpec fgColor = toneColor(widgetForegroundOr(colorSpecFromRole(ColorRole::Label1)));

    if (m_label != nullptr && m_showLabel) {
      m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * fontScale());
      m_label->setText(buildLabelText(pct, s));
      m_label->setColor(fgColor);
      m_label->measure(renderer);
    }
  }

  // Tooltip (both modes)
  if (rootNode != nullptr) {
    auto devices = m_upower->batteryDevices();
    auto laptopEnd =
        std::ranges::stable_partition(devices, [](const UPowerDeviceInfo& d) { return d.isLaptopBattery(); }).begin();
    int laptopBatteryCount = static_cast<int>(laptopEnd - devices.begin());

    std::vector<TooltipRow> rows;
    int laptopBatteryIndex = 0;
    for (const auto& dev : devices) {
      std::string name;
      if (dev.isLaptopBattery()) {
        name = (laptopBatteryCount > 1)
            ? i18n::tr("power.battery.tooltip.device-numbered", "index", ++laptopBatteryIndex)
            : i18n::tr("power.battery.tooltip.device");
      } else {
        name = !dev.model.empty()
            ? dev.model
            : (!dev.nativePath.empty() ? dev.nativePath : i18n::tr("power.battery.tooltip.unknown-device"));
      }
      int dp = static_cast<int>(std::round(dev.state.percentage));
      rows.push_back({std::move(name), std::to_string(dp) + "%"});

      if (dev.isLaptopBattery()) {
        rows.push_back({i18n::tr("power.battery.tooltip.status"), batteryStateLabel(dev.state.state)});

        if (dev.state.timeToEmpty > 0) {
          auto dur = formatDuration(std::chrono::seconds(dev.state.timeToEmpty));
          rows.push_back({i18n::tr("power.battery.tooltip.time-left"), std::move(dur)});
        } else if (dev.state.timeToFull > 0) {
          auto dur = formatDuration(std::chrono::seconds(dev.state.timeToFull));
          rows.push_back({i18n::tr("power.battery.tooltip.time-to-full"), std::move(dur)});
        }

        if (dev.state.energyRate > 0.0) {
          std::ostringstream oss;
          oss << std::fixed;
          oss.precision(1);
          oss << dev.state.energyRate << " W";
          rows.push_back({i18n::tr("power.battery.tooltip.rate"), oss.str()});
        }

        if (const std::optional<double> health = dev.healthPercent()) {
          rows.push_back({i18n::tr("power.battery.tooltip.health"), std::format("{:.0F}%", *health)});
        }
      }
    }
    if (!rows.empty()) {
      static_cast<InputArea*>(rootNode)->setTooltip(std::move(rows));
    } else {
      static_cast<InputArea*>(rootNode)->clearTooltip();
    }
  }

  requestRedraw();
}
