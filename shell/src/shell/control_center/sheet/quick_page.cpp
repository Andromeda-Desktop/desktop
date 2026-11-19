#include "shell/control_center/sheet/quick_page.h"
#include "ui/controls/context_menu.h"
#include "shell/session/session_action_meta.h"
#include "shell/control_center/sheet/power_menu_page.h"

#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/link/link_service.h"
#include "dbus/network/network_display.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/control_center/bluetooth_glyph.h"
#include "shell/phone/phone_state.h"
#include "system/brightness_service.h"
#include "system/rfkill_helper.h"
#include "notification/notification_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/scene/input_area.h"
#include "ui/controls/scroll_view.h"
#include "config/config_service.h"
#include "ui/controls/slider.h"
#include "ui/controls/spinner.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/input/key_modifiers.h"

#include <algorithm>
#include <limits>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <utility>

namespace {

  // Anatomy: docs/design-system.md "Control center"; values: protocol/design-tokens.toml [component.*].
  TOKENS_LIVE(kRoundIcon, Tokens::componentRoundIconSize);
  TOKENS_LIVE(kPillHeight, Tokens::componentSliderHeight);
  // The bus name andromeda-linkd exports a phone's player under, followed by the device id.
  constexpr std::string_view kPhonePlayerPrefix = "org.mpris.MediaPlayer2.andromeda_link_";
  constexpr float kChevronGlyph = 12.0F;
  constexpr float kTileDragSlop = 8.0F;
  constexpr std::size_t kDropsPerRow = 5;
  constexpr float kChevronHit = 12.0F; // the glyph sits at inset 12; the hit area grows by 6 each side

  // The slider fills the whole pill; the icon and the value label draw over it.
  // Split ink: the row repeats in on_accent inside `ink`, a clip as wide as the level, so a symbol or value
  // the fill edge crosses shows both inks.
  class SliderPill : public Flex {
  public:
    Slider* slider = nullptr;
    Glyph* icon = nullptr;
    Label* name = nullptr;
    Label* value = nullptr;
    Flex* ink = nullptr;
    Flex* inkRow = nullptr;
    Glyph* inkIcon = nullptr;
    Label* inkName = nullptr;
    Label* inkValue = nullptr;

    void clipInk() {
      if (ink != nullptr && slider != nullptr) {
        ink->setFrameSize(std::round(slider->normalizedValue() * width()), height());
      }
    }

  protected:
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      if (slider != nullptr) {
        slider->arrange(renderer, LayoutRect{.width = width(), .height = height()});
      }
      if (inkRow != nullptr) {
        inkIcon->setCodepoint(icon->codepoint());
        inkName->setText(name->text());
        inkName->setMaxWidth(name->width());
        inkValue->setText(value->text());
        inkValue->setMaxWidth(value->width());
        inkRow->arrange(renderer, LayoutRect{.width = width(), .height = height()});
        ink->setPosition(0.0F, 0.0F);
      }
      clipInk();
    }
  };

  // A tile with an edit badge: a remove badge at -4 / -4, or an add badge centred at the right inset.
  class BadgedCard : public Flex {
  public:
    Flex* badge = nullptr;
    bool addMode = false;
    float scale = 1.0F;

  protected:
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      if (badge == nullptr) {
        return;
      }
      const float size = (addMode ? Tokens::componentEditBadgeAdd : Tokens::componentEditBadgeRemove) * scale;
      const float x = addMode ? width() - Tokens::componentTilePadding * scale - size
                              : -Tokens::componentEditBadgeInset * scale;
      const float y = addMode ? (height() - size) * 0.5F : -Tokens::componentEditBadgeInset * scale;
      badge->arrange(renderer, LayoutRect{.x = x, .y = y, .width = size, .height = size});
    }
  };

  // A Button's own hit area has z-index 1 and covers its children; nested buttons must sit above it.
  constexpr std::int32_t kAboveCardArea = 2;

  std::string percent(float value) { return std::format("{} %", static_cast<int>(std::lround(value * 100.0F))); }

  std::unique_ptr<Label> makeLabel(Label*& out, const Tokens::Type& type, float scale, ColorRole role) {
    auto label = std::make_unique<Label>();
    label->setFontSize(type.size * scale);
    label->setLineHeight(type.line * scale);
    label->setFontWeight(static_cast<FontWeight>(Style::glassWeight(type.weight)));
    label->setColor(colorSpecFromRole(role));
    label->setMaxLines(1);
    out = label.get();
    return label;
  }

  std::unique_ptr<Button> makeGlyphButton(
      Button*& out, std::string_view glyph, float scale, std::function<void()> click, float size = kRoundIcon,
      ButtonVariant variant = ButtonVariant::Tonal
  ) {
    auto button = std::make_unique<Button>();
    button->setVariant(variant);
    button->setGlyph(glyph);
    button->setGlyphSize(Tokens::componentRoundIconGlyph * scale);
    button->setControlHeight(size * scale);
    button->setMinWidth(size * scale);
    button->setPadding(0.0F);
    button->setRadius(size * 0.5F * scale);
    button->setOnClick(std::move(click));
    out = button.get();
    return button;
  }

  // Card hover and press (docs: DESIGN.md "Quick-sheet row hover"): the card goes fill-1 -> fill-2 and scales 0.97.
  struct CardMotion {
    float hover = 0.0F;
    float press = 0.0F;
    AnimationManager::Id hoverId = 0;
    AnimationManager::Id pressId = 0;
  };

  desk::motion::SpringConfig springOf(const Tokens::Spring& token) {
    return {.damping = token.damping, .stiffness = token.stiffness};
  }

  // The row owns the animations (Node cancels them when the row dies); the state lives as long as a setter holds it.
  void driveCard(
      Button& row, const std::shared_ptr<CardMotion>& motion, float CardMotion::* value,
      AnimationManager::Id CardMotion::* idMember, float target, const Tokens::Spring& token,
      std::function<void(Flex&, float)> apply
  ) {
    AnimationManager::Id& id = motion.get()->*idMember;
    auto* card = dynamic_cast<Flex*>(row.parent());
    AnimationManager* manager = row.animationManager();
    if (card == nullptr || manager == nullptr) {
      return;
    }
    if (id != 0 && manager->retarget(id, target, springOf(token))) {
      return;
    }
    id = manager->animateSpring(
        motion.get()->*value, target, springOf(token),
        [card, motion, value, apply = std::move(apply)](float v) {
          motion.get()->*value = v;
          apply(*card, v);
        },
        {}, &row
    );
  }

  void attachCardMotion(Button& row, float opacity, Button* icon, std::function<void(float, float, bool)> onPressExtra = {}) {
    row.setStateFillless(true);
    auto motion = std::make_shared<CardMotion>();
    Button* rowPtr = &row;
    const auto hoverTo = [rowPtr, motion, opacity, icon](float target) {
      driveCard(*rowPtr, motion, &CardMotion::hover, &CardMotion::hoverId, target, Tokens::springFade, [opacity, icon](Flex& card, float t) {
        icon->setHoverMix(t);
        // A compositor glass piece has no fill of its own at rest, so every piece shows the same glass tint.
        const bool glass = Style::compositorDrawsTint();
        // At rest the card keeps the role spec, so a theme change after a hover repaints it.
        if (t <= 0.0F) {
          if (glass) {
            card.setFill(Color{0.0F, 0.0F, 0.0F, 0.0F});
          } else {
            card.setFill(colorSpecFromRole(ColorRole::GlassCard, opacity));
          }
          return;
        }
        const Color hover = resolveColorSpec(colorSpecFromRole(ColorRole::Fill2, opacity));
        const Color rest = glass ? Color{hover.r, hover.g, hover.b, 0.0F}
                                 : resolveColorSpec(colorSpecFromRole(ColorRole::GlassCard, opacity));
        card.setFill(lerpColor(rest, hover, t));
      });
    };
    row.setOnEnter([hoverTo] { hoverTo(1.0F); });
    row.setOnLeave([hoverTo] { hoverTo(0.0F); });
    row.setOnPress([rowPtr, motion, onPressExtra = std::move(onPressExtra)](float x, float y, bool pressed) {
      if (onPressExtra) {
        onPressExtra(x, y, pressed);
      }
      driveCard(
          *rowPtr, motion, &CardMotion::press, &CardMotion::pressId, pressed ? 1.0F : 0.0F,
          pressed ? Tokens::springPress : Tokens::springRelease,
          [](Flex& card, float t) {
            card.setTransformOrigin(card.width() * 0.5F, card.height() * 0.5F);
            card.setScale(1.0F - 0.03F * t);
          }
      );
    });
  }

} // namespace

QuickPage::QuickPage(const ControlCenterServices& services, OpenTabCallback openTab)
    : m_services(services), m_openTab(std::move(openTab)) {
  m_phoneRing = ShortcutRegistry::create("phone", services.shortcutServices());
}

std::unique_ptr<Flex> QuickPage::makeCard(bool badged) {
  const float s = contentScale();
  std::unique_ptr<Flex> card = badged ? std::make_unique<BadgedCard>() : std::make_unique<Flex>();
  card->setCardStyle(s, panelCardOpacity());
  card->setDirection(FlexDirection::Vertical);
  card->setAlign(FlexAlign::Stretch);
  card->setGap(Style::spaceSm * s);
  // The compositor draws each card as a glass piece; without it the card paints its own fill on the sheet.
  if (!Style::compositorDrawsTint()) {
    card->setFill(colorSpecFromRole(ColorRole::GlassCard, panelCardOpacity()));
    card->setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 0.5F * s);
  } else {
    card->setFill(Color{0.0F, 0.0F, 0.0F, 0.0F});
  }
  card->setRadius(Tokens::componentTileRadius * s);
  card->setGlassPiece(Tokens::componentTileRadius * s);
  card->setPadding(Tokens::componentTilePadding * s);
  card->setGap(Tokens::componentTileGap * s);
  card->setJustify(FlexJustify::Center);
  return card;
}

void QuickPage::addSingleRow(Flex& card, std::unique_ptr<Button> row) {
  // The row is the whole card, so its press fill covers the card instead of an inset rectangle.
  const float s = contentScale();
  card.setPadding(0.0F);
  row->setPadding(Tokens::componentTilePadding * s);
  row->setRadius(Tokens::componentTileRadius * s);
  row->setFlexGrow(1.0F);
  card.addChild(std::move(row));
}

std::unique_ptr<Button> QuickPage::makeToggleRow(
    ToggleRow& row, std::string_view detailTab, float iconTextGap, bool spinner
) {
  const float s = contentScale();
  auto container = std::make_unique<Button>();
  container->setVariant(ButtonVariant::Ghost);
  container->setDirection(FlexDirection::Horizontal);
  container->setAlign(FlexAlign::Center);
  container->setPadding(0.0F);
  container->setMinHeight(kRoundIcon * s);
  container->setGap(iconTextGap * s);
  container->setContentAlign(ButtonContentAlign::Start);
  Shortcut* shortcut = row.shortcut.get();
  // The round icon toggles; the rest of a row with a detail page opens it (macOS / iOS Control Center), so a tap on
  // the name never turns Wi-Fi off. An empty detail tab means the card itself is the toggle.
  container->setOnClick([this, shortcut, tab = std::string(detailTab)] {
    if (m_swallowClick) {
      return;
    }
    if (!tab.empty()) {
      m_openTab(tab);
    } else if (shortcut != nullptr) {
      shortcut->onClick();
    }
  });
  const std::string glyph = !row.glyph.empty() ? row.glyph : std::string(shortcut != nullptr ? shortcut->currentIcon() : "");
  auto icon = makeGlyphButton(row.icon, glyph, s, [this, shortcut] {
    if (m_swallowClick) {
      return;
    }
    if (shortcut != nullptr) {
      shortcut->onClick();
    }
  });
  icon->setZIndex(kAboveCardArea);
  icon->setStateFillless(true);
  // The tile is one focus stop (rulebook 11): Space toggles through the icon, Enter opens the detail.
  icon->inputArea()->setFocusable(false);
  Button* body = container.get();
  attachCardMotion(*container, panelCardOpacity(), icon.get(),
                   [this, body](float x, float y, bool pressed) { tilePress(body, x, y, pressed); });
  attachTileDrag(*icon);
  container->setOnPointerMotion([this, body](float x, float y) { tileMotion(body, x, y); });
  container->addChild(std::move(icon));

  auto text = std::make_unique<Flex>();
  text->setDirection(FlexDirection::Vertical);
  text->setAlign(FlexAlign::Start);
  text->setFlexGrow(1.0F);
  text->addChild(makeLabel(row.title, Tokens::typeHeadline, s, ColorRole::Label1));
  text->addChild(makeLabel(row.subtitle, Tokens::typeFootnote, s, ColorRole::Label2));
  container->addChild(std::move(text));
  if (spinner) {
    auto spinner = std::make_unique<Spinner>();
    spinner->setSpinnerSize(kChevronGlyph * 1.5F * s);
    spinner->setVisible(false);
    row.spinner = spinner.get();
    container->addChild(std::move(spinner));
  }
  row.body = container.get();
  if (!detailTab.empty()) {
    container->setOnRightClick([this, tab = std::string(detailTab)] { m_openTab(tab); });
  }
  return container;
}

void QuickPage::attachTileDrag(Button& button) {
  Button* source = &button;
  button.setOnPress([this, source](float x, float y, bool pressed) { tilePress(source, x, y, pressed); });
  button.setOnPointerMotion([this, source](float x, float y) { tileMotion(source, x, y); });
}

// A press that moves sideways past the slop hands the gesture to the pager; the tile then does not toggle.
void QuickPage::tilePress(Button* button, float x, float y, bool pressed) {
  if (m_pager == nullptr) {
    return;
  }
  if (!pressed && m_tileDragging) {
    m_pager->dragEnd(); // also when edit mode started mid-drag
  }
  if (m_editing) {
    m_tileDragging = false;
    m_tileSource = nullptr;
    return;
  }
  if (pressed) {
    m_tileSource = button;
    m_tileDragging = false;
    m_swallowClick = false;
    m_tilePressX = offsetIn(button, m_pager, false) + x;
    m_tilePressY = offsetIn(button, m_pager, true) + y;
    return;
  }
  m_tileDragging = false;
  m_tileSource = nullptr;
}

void QuickPage::tileMotion(Button* button, float x, float y) {
  if (m_tileSource != button || m_editing || m_pager == nullptr) {
    return;
  }
  const float position = offsetIn(button, m_pager, false) + x;
  if (!m_tileDragging) {
    const float dx = position - m_tilePressX;
    const float dy = offsetIn(button, m_pager, true) + y - m_tilePressY;
    if (std::abs(dx) < kTileDragSlop * contentScale() || std::abs(dx) < std::abs(dy)) {
      return;
    }
    m_tileDragging = true;
    m_swallowClick = true;
    m_pager->dragBegin(position);
  }
  m_pager->dragMove(position);
}

std::unique_ptr<Button> QuickPage::makeTileRow(std::string_view type, bool plain) {
  ToggleRow row;
  row.id = std::string(type);
  row.shortcut = ShortcutRegistry::create(type, m_services.shortcutServices());
  if (row.shortcut == nullptr) {
    return nullptr;
  }
  std::string_view detail;
  if (plain) {
  } else if (type == "wifi") {
    detail = "detail-wifi";
  } else if (type == "bluetooth") {
    detail = "detail-bluetooth";
  } else if (type == "notification") {
    detail = "detail-focus";
  } else if (type == "mic_mute") {
    detail = "detail-sound";
  }
  if (type == "notification") {
    row.glyph = "moon";
  }
  auto button = makeToggleRow(row, detail, Tokens::componentTileGap, !detail.empty());
  row.title->setText(type == "notification" ? i18n::tr("control-center.quick.focus")
                     : type == "dark_mode"  ? i18n::tr("control-center.quick.dark-mode")
                                            : row.shortcut->defaultLabel());
  row.detail = std::string(detail);
  m_extras.push_back(std::move(row));
  return button;
}

std::unique_ptr<Flex> QuickPage::makeTileCard(std::string_view type, bool plain) {
  auto row = makeTileRow(type, plain);
  if (row == nullptr) {
    return nullptr;
  }
  auto card = makeCard(plain);
  card->setFlexGrow(1.0F);
  addSingleRow(*card, std::move(row));
  m_extras.back().card = card.get();
  return card;
}

std::unique_ptr<Flex> QuickPage::makeDropTile(std::string_view type) {
  auto row = makeTileRow(type, true);
  if (row == nullptr) {
    return nullptr;
  }
  const float s = contentScale();
  const float size = Tokens::componentTileHeight * s;
  Button* body = row.get();
  ToggleRow& tile = m_extras.back();
  tile.drop = true;
  if (Node* text = tile.title->parent(); text != nullptr) {
    text->setVisible(false);
    text->setParticipatesInLayout(false);
  }
  auto card = makeCard(true);
  card->setMinWidth(size);
  card->setMaxWidth(size);
  card->setMinHeight(size);
  card->setMaxHeight(size);
  card->setRadius(size * 0.5F);
  card->setGlassPiece(size * 0.5F);
  addSingleRow(*card, std::move(row));
  // The toggle fills the whole drop, so its on state colours the drop (concept C), not a disc inside it.
  body->setPadding(0.0F);
  tile.icon->setControlHeight(size);
  tile.icon->setMinWidth(size);
  tile.icon->setGlyphSize(Tokens::componentTileDropGlyph * s);
  tile.icon->setRadius(size * 0.5F);
  tile.card = card.get();
  return card;
}

std::unique_ptr<Flex> QuickPage::makePill(std::string_view glyph, Slider*& slider, Label*& name, Label*& value) {
  const float s = contentScale();
  auto pill = std::make_unique<SliderPill>();
  pill->setDirection(FlexDirection::Horizontal);
  pill->setAlign(FlexAlign::Center);
  pill->setGap(Tokens::componentSliderInset * s);
  pill->setPadding(0.0F, Tokens::componentSliderInset * s, 0.0F, Tokens::componentSliderInset * s);
  pill->setMinHeight(kPillHeight * s);
  pill->setRadius(Tokens::componentSliderRadius * s);
  pill->setGlassPiece(Tokens::componentSliderRadius * s);
  if (!Style::compositorDrawsTint()) {
    pill->setFill(colorSpecFromRole(ColorRole::SliderTrack));
  }

  auto bar = std::make_unique<Slider>();
  bar->setRange(0.0, 1.0);
  bar->setStep(0.01);
  bar->setControlHeight(kPillHeight * s);
  bar->setFilled(true);
  bar->setTrackRadius(Tokens::componentSliderRadius * s);
  bar->setParticipatesInLayout(false);
  slider = bar.get();
  pill->slider = slider;
  pill->addChild(std::move(bar));

  auto icon = std::make_unique<Glyph>();
  icon->setGlyph(glyph);
  icon->setGlyphSize(Tokens::componentRoundIconGlyph * s);
  Glyph* iconPtr = icon.get();
  pill->addChild(std::move(icon));

  auto nameLabel = makeLabel(name, Tokens::typeHeadline, s, ColorRole::Label1);
  nameLabel->setFlexGrow(1.0F);
  pill->name = name;
  pill->addChild(std::move(nameLabel));

  auto label = makeLabel(value, Tokens::typeFootnote, s, ColorRole::Label2);
  label->setMaxWidth(120.0F * s);
  iconPtr->setColor(colorSpecFromRole(ColorRole::Label2));
  pill->icon = iconPtr;
  pill->value = label.get();
  pill->addChild(std::move(label));

  auto ink = std::make_unique<Flex>();
  ink->setParticipatesInLayout(false);
  ink->setClipChildren(true);
  ink->setHitTestVisible(false);
  auto inkRow = std::make_unique<Flex>();
  inkRow->setDirection(FlexDirection::Horizontal);
  inkRow->setAlign(FlexAlign::Center);
  inkRow->setGap(Tokens::componentSliderInset * s);
  inkRow->setPadding(0.0F, Tokens::componentSliderInset * s, 0.0F, Tokens::componentSliderInset * s);
  auto inkIcon = std::make_unique<Glyph>();
  inkIcon->setGlyphSize(Tokens::componentRoundIconGlyph * s);
  inkIcon->setColor(colorSpecFromRole(ColorRole::OnAccent));
  pill->inkIcon = inkIcon.get();
  inkRow->addChild(std::move(inkIcon));
  Label* inkName = nullptr;
  auto inkNameLabel = makeLabel(inkName, Tokens::typeHeadline, s, ColorRole::OnAccent);
  inkNameLabel->setFlexGrow(1.0F);
  pill->inkName = inkName;
  inkRow->addChild(std::move(inkNameLabel));
  Label* inkValue = nullptr;
  auto inkLabel = makeLabel(inkValue, Tokens::typeFootnote, s, ColorRole::OnAccent);
  pill->inkValue = inkValue;
  inkRow->addChild(std::move(inkLabel));
  pill->inkRow = inkRow.get();
  ink->addChild(std::move(inkRow));
  pill->ink = ink.get();
  pill->addChild(std::move(ink));
  slider->setOnLevelChanged([pillPtr = pill.get()] { pillPtr->clipInk(); });
  return pill;
}

std::unique_ptr<Flex> QuickPage::create() {
  const float s = contentScale();
  m_extras.clear();
  m_editing = false;
  m_dragIndex = -1;
  m_pressedIndex = -1;
  m_pressedBadge = false;
  m_tileSource = nullptr;
  m_tileDragging = false;
  m_swallowClick = false;
  m_flipFrom.clear();
  m_staggerPending = true;
  for (Timer& timer : m_staggerTimers) {
    timer.stop();
  }
  m_editReflowPending = false;
  m_editRebuildPending = false;
  m_editArea = nullptr;
  m_galleryArea = nullptr;
  m_editCards.clear();
  m_liftShadow = nullptr;
  m_editRows.clear();
  m_editHalf.clear();
  m_galleryHalf.clear();
  m_galleryIds.clear();
  m_galleryCards.clear();
  m_editOrder.clear();
  m_page = 0;
  m_pageStep = 0.0F;
  m_halfWidth.clear();
  auto root = std::make_unique<Flex>();
  m_root = root.get();
  root->setDirection(FlexDirection::Vertical);
  root->setAlign(FlexAlign::Stretch);
  root->setGap(Tokens::componentSheetGap * s);

  m_pages.clear();
  m_dots.clear();
  m_pager = nullptr;
  auto pager = std::make_unique<ScrollView>();
  pager->setOrientation(ScrollOrientation::Horizontal);
  pager->setScrollbarVisible(false);
  pager->setViewportPaddingH(0.0F);
  pager->setViewportPaddingV(0.0F);
  pager->setContentScale(s);
  pager->setOnScrollChanged([this](float offset) { refreshDots(offset); });
  m_pager = pager.get();
  Flex* strip = pager->content();
  strip->setDirection(FlexDirection::Horizontal);
  strip->setAlign(FlexAlign::Stretch);
  strip->setGap(2.0F * Tokens::componentSheetPadding * s);
  root->addChild(std::move(pager));
  root->addChild(makeDots());
  buildPages(tileOrder());

  auto brightness = makePill("sun", m_brightnessSlider, m_brightnessName, m_brightnessValue);
  m_brightnessName->setText(i18n::tr("control-center.detail.display.brightness"));
  m_brightnessPill = brightness.get();
  m_brightnessSlider->setOnValueChanged([this](double value) {
    if (m_syncingSliders) {
      return;
    }
    if (m_services.brightness == nullptr) {
      return;
    }
    for (const BrightnessDisplay& display : m_services.brightness->displays()) {
      if (display.controllable) {
        m_services.brightness->setBrightness(display.id, static_cast<float>(value));
      }
    }
    m_brightnessValue->setText(percent(static_cast<float>(value)));
  });
  root->addChild(std::move(brightness));

  auto volume = makePill("volume", m_volumeSlider, m_volumeName, m_volumeValue);
  m_volumePill = volume.get();
  m_volumeSlider->setOnValueChanged([this](double value) {
    if (m_syncingSliders) {
      return;
    }
    if (m_services.audio != nullptr) {
      if (const AudioNode* sink = m_services.audio->defaultSink(); sink != nullptr) {
        m_services.audio->setSinkVolume(sink->id, static_cast<float>(value));
      }
    }
    m_volumeValue->setText(percent(static_cast<float>(value)));
  });
  // A tap or a right-click on the output device name opens the Sound page.
  m_volumeName->setEllipsize(TextEllipsize::End);
  m_brightnessName->setEllipsize(TextEllipsize::End);
  m_volumeName->setOnPress([this](const InputArea::PointerData& d) {
    if (d.pressed && d.button == BTN_RIGHT) {
      m_openTab("detail-sound");
    }
  });
  m_volumeName->setOnClick([this](const InputArea::PointerData& d) {
    if (d.button == BTN_LEFT) {
      m_openTab("detail-sound");
    }
  });
  root->addChild(std::move(volume));

  auto media = makeCard();
  media->setDirection(FlexDirection::Horizontal);
  media->setAlign(FlexAlign::Center);
  m_mediaCard = media.get();
  media->setRadius(Tokens::componentMediaHeight * s * 0.5F);
  media->setGlassPiece(Tokens::componentMediaHeight * s * 0.5F);
  media->setPadding(Tokens::componentMediaPadding * s);
  media->setMinHeight(Tokens::componentMediaHeight * s);
  // fill_3 shows while there is no artwork.
  const float art = Tokens::componentMediaArt * s;
  auto artBox = std::make_unique<Flex>();
  artBox->setFill(colorSpecFromRole(ColorRole::Fill3));
  artBox->setRadius(Tokens::componentMediaArtRadius * s);
  artBox->setMinWidth(art);
  artBox->setMinHeight(art);
  auto artImage = std::make_unique<Image>();
  artImage->setFit(ImageFit::Cover);
  artImage->setRadius(Tokens::componentMediaArtRadius * s);
  artImage->setSize(art, art);
  artImage->setParticipatesInLayout(false);
  m_mediaArt = artImage.get();
  artBox->addChild(std::move(artImage));
  media->addChild(std::move(artBox));
  auto mediaText = std::make_unique<Flex>();
  mediaText->setDirection(FlexDirection::Vertical);
  mediaText->setAlign(FlexAlign::Start);
  mediaText->setFlexGrow(1.0F);
  mediaText->addChild(makeLabel(m_mediaTitle, Tokens::typeHeadline, s, ColorRole::Label1));
  mediaText->addChild(makeLabel(m_mediaArtist, Tokens::typeFootnote, s, ColorRole::Label2));
  media->addChild(std::move(mediaText));
  // Bare glyphs; each button is glyph + gap wide, so glyph centers sit glyph + controls_gap apart as in the reference.
  TOKENS_LIVE(kMediaControl, Tokens::componentRoundIconGlyph + Tokens::componentMediaControlsGap);
  auto controls = std::make_unique<Flex>();
  controls->setDirection(FlexDirection::Horizontal);
  controls->setAlign(FlexAlign::Center);
  Button* unused = nullptr;
  controls->addChild(makeGlyphButton(m_mediaKeys[0], "player-skip-back", s, [this] {
    if (m_services.mpris != nullptr) {
      m_services.mpris->previousActive();
    }
  }, kMediaControl, ButtonVariant::Ghost));
  controls->addChild(makeGlyphButton(m_mediaKeys[1], "player-play", s, [this] {
    if (m_services.mpris != nullptr) {
      m_services.mpris->playPauseActive();
    }
  }, kMediaControl, ButtonVariant::Ghost));
  controls->addChild(makeGlyphButton(m_mediaKeys[2], "player-skip-forward", s, [this] {
    if (m_services.mpris != nullptr) {
      m_services.mpris->nextActive();
    }
  }, kMediaControl, ButtonVariant::Ghost));
  m_playButton = m_mediaKeys[1];
  media->addChild(std::move(controls));
  root->addChild(std::move(media));

  auto phone = makeCard();
  phone->setDirection(FlexDirection::Horizontal);
  phone->setAlign(FlexAlign::Center);
  m_phoneCard = phone.get();
  Button* phoneIcon = nullptr;
  phone->addChild(makeGlyphButton(phoneIcon, "device-mobile", s, {}, kRoundIcon, ButtonVariant::Primary));
  auto phoneText = std::make_unique<Flex>();
  phoneText->setDirection(FlexDirection::Vertical);
  phoneText->setAlign(FlexAlign::Start);
  phoneText->setFlexGrow(1.0F);
  phoneText->addChild(makeLabel(m_phoneName, Tokens::typeHeadline, s, ColorRole::Label1));
  phoneText->addChild(makeLabel(m_phoneStatus, Tokens::typeFootnote, s, ColorRole::Label2));
  phone->addChild(std::move(phoneText));
  auto actions = std::make_unique<Flex>();
  actions->setDirection(FlexDirection::Horizontal);
  actions->setAlign(FlexAlign::Center);
  actions->setGap(Tokens::componentRoundIconActionGap * s);
  actions->addChild(makeGlyphButton(m_phoneKeys[0], "bell-ringing", s, [this] {
    if (m_phoneRing != nullptr) {
      m_phoneRing->onClick();
    }
  }, Tokens::componentRoundIconAction));
  actions->addChild(makeGlyphButton(m_phoneKeys[1], "screen-share", s, [this] {
    if (m_services.link != nullptr && !m_phoneId.empty()) {
      m_services.link->mirror(m_phoneId);
    }
  }, Tokens::componentRoundIconAction));
  phone->addChild(std::move(actions));
  auto phoneOpen = makeGlyphButton(unused, "chevron-right", s, [this] { m_openTab("detail-phone"); }, kChevronHit,
                                   ButtonVariant::Ghost);
  phoneOpen->setGlyphSize(kChevronGlyph * s);
  if (phoneOpen->inputArea() != nullptr) {
    constexpr float kOutset = 6.0F;
    phoneOpen->inputArea()->setHitTestOutset({.left = kOutset * s, .top = kOutset * s, .right = kOutset * s, .bottom = kOutset * s});
  }
  if (phoneOpen->glyph() != nullptr) {
    phoneOpen->glyph()->setColor(colorSpecFromRole(ColorRole::Label3));
  }
  m_phoneKeys[2] = phoneOpen.get();
  phone->addChild(std::move(phoneOpen));
  const auto openOnRightClick = [this](const InputArea::PointerData& d) {
    if (d.pressed && d.button == BTN_RIGHT) {
      m_openTab("detail-phone");
    }
  };
  m_phoneName->setOnPress(openOnRightClick);
  m_phoneStatus->setOnPress(openOnRightClick);
  phoneIcon->setOnRightClick([this] { m_openTab("detail-phone"); });
  root->addChild(std::move(phone));

  root->addChild(makeFooter());
  m_normalParts.clear();
  for (const auto& child : root->children()) {
    m_normalParts.push_back(child.get());
  }
  buildEditParts(*root);

  return root;
}

std::unique_ptr<Flex> QuickPage::makeDots() {
  const float s = contentScale();
  auto row = std::make_unique<Flex>();
  row->setDirection(FlexDirection::Horizontal);
  row->setAlign(FlexAlign::Center);
  row->setJustify(FlexJustify::Center);
  row->setGap(Tokens::componentPageDotsGap * s);
  row->setMinHeight(Tokens::componentPageDotsRow * s);
  m_dotsRow = row.get();
  return row;
}

void QuickPage::fillDots(std::size_t pages) {
  const float s = contentScale();
  const float kDot = Tokens::componentPageDotsDot;
  const float kDotGap = Tokens::componentPageDotsGap;
  while (!m_dotsRow->children().empty()) {
    m_dotsRow->removeChild(m_dotsRow->children().back().get());
  }
  m_dots.clear();
  m_dotsRow->setVisible(pages > 1);
  for (std::size_t i = 0; i < pages; ++i) {
    auto dot = std::make_unique<Flex>();
    dot->setMinWidth(kDot * s);
    dot->setMinHeight(kDot * s);
    dot->setRadius(kDot * 0.5F * s);
    dot->setFill(colorSpecFromRole(ColorRole::Fill3));
    auto area = std::make_unique<InputArea>();
    area->setParticipatesInLayout(false);
    area->setFrameSize(kDot * s, kDot * s);
    area->setHitTestOutset({.left = kDotGap * 0.5F * s, .top = 6.0F * s, .right = kDotGap * 0.5F * s, .bottom = 6.0F * s});
    area->setOnClick([this, i](const InputArea::PointerData&) { goToPage(i); });
    dot->addChild(std::move(area));
    m_dots.push_back(dot.get());
    m_dotsRow->addChild(std::move(dot));
  }
}

void QuickPage::goToPage(std::size_t page) {
  if (m_pager != nullptr && m_pageStep > 0.0F && page < m_pages.size()) {
    m_pager->scrollToPage(page);
  }
}

const std::vector<std::string>& QuickPage::tileOrderDefault() {
  // concept-c.html:165-175: the Wi-Fi and Bluetooth cards, then Focus, Dark Mode, Airplane, Night Light, Caffeine.
  static const std::vector<std::string> kDefault = {"wifi",     "bluetooth",  "notification", "dark_mode",
                                                    "airplane", "nightlight", "caffeine"};
  return kDefault;
}

const std::vector<std::string>& QuickPage::tileIdsAll() {
  static const std::vector<std::string> kAll = [] {
    std::vector<std::string> ids = tileOrderDefault();
    ids.insert(ids.end(), {"hotspot", "screen-record", "mic_mute"});
    return ids;
  }();
  return kAll;
}

std::vector<std::string> QuickPage::savedTileOrder() const {
  const auto* config = m_services.config;
  const auto& stored = config != nullptr ? config->config().controlCenter.quickTiles : std::vector<std::string>{};
  std::vector<std::string> order;
  for (const std::string& id : stored.empty() ? tileOrderDefault() : stored) {
    if (std::ranges::find(order, id) == order.end()) {
      order.push_back(id);
    }
  }
  return order;
}

std::vector<std::string> QuickPage::tileOrder() const {
  std::vector<std::string> order = savedTileOrder();
  if (std::ranges::none_of(order, [this](const std::string& id) { return tileAvailable(id); })) {
    return tileOrderDefault();
  }
  return order;
}

bool QuickPage::tileAvailable(std::string_view id) const {
  if (id == "airplane") {
    return hasRfkillRadios();
  }
  if (id == "hotspot") {
    return m_services.hotspot != nullptr;
  }
  if (id == "nightlight") {
    return m_services.nightLight != nullptr;
  }
  if (id == "caffeine") {
    return m_services.idleInhibitor != nullptr;
  }
  if (id == "screen-record") {
    return m_services.screenRecorder != nullptr;
  }
  if (id == "mic_mute") {
    return m_services.audio != nullptr && m_services.audio->defaultSource() != nullptr;
  }
  return id == "wifi" || id == "bluetooth" || id == "notification" || id == "dark_mode" || id == "session";
}

void QuickPage::buildPages(const std::vector<std::string>& order) {
  const float s = contentScale();
  Flex* strip = m_pager->content();
  while (!strip->children().empty()) {
    strip->removeChild(strip->children().back().get());
  }
  m_pages.clear();
  m_halfWidth.clear();
  m_extras.clear();
  std::vector<std::string> connectivity;
  std::vector<std::string> others;
  for (const std::string& id : order) {
    if (!tileAvailable(id)) {
      continue;
    }
    (id == "wifi" || id == "bluetooth" ? connectivity : others).push_back(id);
  }
  std::ranges::sort(connectivity, std::greater<>{}); // "wifi" before "bluetooth"

  // Concept C: the Wi-Fi + Bluetooth pair, then rows of round toggles; one page, each card its own glass piece.
  auto page = std::make_unique<Flex>();
  page->setDirection(FlexDirection::Vertical);
  page->setAlign(FlexAlign::Stretch);
  page->setGap(Tokens::componentSheetGap * s);
  if (!connectivity.empty()) {
    auto pair = std::make_unique<Flex>();
    pair->setDirection(FlexDirection::Horizontal);
    pair->setAlign(FlexAlign::Stretch);
    pair->setGap(Tokens::componentSheetGap * s);
    const float pill = Tokens::componentTileHeight * s * 0.5F;
    for (const std::string& id : connectivity) {
      if (auto cap = makeTileCard(id); cap != nullptr) {
        cap->setRadius(pill);
        cap->setGlassPiece(pill);
        m_halfWidth.push_back(cap.get());
        pair->addChild(std::move(cap));
      }
    }
    page->addChild(std::move(pair));
  }
  std::unique_ptr<Flex> row;
  const auto closeRow = [&] {
    // Empty slots keep the last row on the same columns as the full rows above it.
    while (row->children().size() < kDropsPerRow) {
      auto slot = std::make_unique<Flex>();
      slot->setMinWidth(Tokens::componentTileHeight * s);
      row->addChild(std::move(slot));
    }
    page->addChild(std::move(row));
  };
  for (const std::string& id : others) {
    auto drop = makeDropTile(id);
    if (drop == nullptr) {
      continue;
    }
    if (row == nullptr) {
      row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setJustify(FlexJustify::SpaceBetween);
    }
    row->addChild(std::move(drop));
    if (row->children().size() == kDropsPerRow) {
      closeRow();
    }
  }
  if (row != nullptr) {
    closeRow();
  }
  m_pages.push_back(page.get());
  strip->addChild(std::move(page));
  fillDots(m_pages.size());
}

std::unique_ptr<Flex> QuickPage::makeFooter() {
  const float s = contentScale();
  const float kFooterHeight = Tokens::componentRoundIconSize;
  auto footer = std::make_unique<Flex>();
  footer->setDirection(FlexDirection::Horizontal);
  footer->setAlign(FlexAlign::Center);
  footer->setMinHeight(kFooterHeight * s);
  footer->setGap(Tokens::componentSheetGap * s);
  const float footPill = kFooterHeight * s * 0.5F;
  // Battery and power profile share one glass pill at the start; the round buttons end the row.
  auto status = std::make_unique<Flex>();
  status->setDirection(FlexDirection::Horizontal);
  status->setAlign(FlexAlign::Center);
  status->setGap(Tokens::spaceP8 * s);
  status->setMinHeight(kFooterHeight * s);
  status->setPadding(0.0F, Tokens::componentTilePadding * s, 0.0F, Tokens::componentTilePadding * s);
  status->setRadius(footPill);
  status->setGlassPiece(footPill);
  m_footer = footer.get();

  const auto makeGlyph = [&](Glyph*& out) {
    auto glyph = std::make_unique<Glyph>();
    glyph->setGlyphSize(Tokens::componentRoundIconGlyph * s);
    glyph->setColor(colorSpecFromRole(ColorRole::Label2));
    out = glyph.get();
    return glyph;
  };

  auto battery = std::make_unique<Flex>();
  battery->setDirection(FlexDirection::Horizontal);
  battery->setAlign(FlexAlign::Center);
  battery->setGap(Tokens::spaceP8 * s);
  m_batteryBox = battery.get();
  battery->addChild(makeGlyph(m_batteryGlyph));
  battery->addChild(makeLabel(m_batteryText, Tokens::typeFootnote, s, ColorRole::Label2));
  status->addChild(std::move(battery));

  m_profile = ShortcutRegistry::create("power_profile", m_services.shortcutServices());
  auto profile = std::make_unique<Button>();
  // Plain footer text like the battery status, not a filled chip.
  profile->setVariant(ButtonVariant::Ghost);
  profile->setStateFillless(true);
  profile->setDirection(FlexDirection::Horizontal);
  profile->setAlign(FlexAlign::Center);
  profile->setPadding(0.0F);
  profile->setGap(Tokens::spaceP8 * s);
  profile->setOnClick([this] { m_openTab("detail-power"); });
  profile->setOnRightClick([this] {
    if (m_profile != nullptr) {
      m_profile->onClick();
    }
  });
  m_profileBox = profile.get();
  profile->addChild(makeLabel(m_profileText, Tokens::typeFootnote, s, ColorRole::Label2));
  status->addChild(std::move(profile));
  footer->addChild(std::move(status));
  auto spacer = std::make_unique<Flex>();
  spacer->setFlexGrow(1.0F);
  footer->addChild(std::move(spacer));

  const auto iconButton = [&](Button*& out, std::string_view glyph, std::function<void()> click) {
    Button* unused = nullptr;
    auto b = makeGlyphButton(out, glyph, s, std::move(click), kFooterHeight, ButtonVariant::Ghost);
    b->setRadius(footPill);
    b->setGlassPiece(footPill);
    footer->addChild(std::move(b));
  };
  iconButton(m_notificationsButton, "bell", [] { PanelManager::instance().togglePanel("notification-center"); });
  m_notificationsButton->setTooltip(i18n::tr("control-center.tabs.notifications"));
  {
    auto badge = ui::box({
        .out = &m_unreadBadge,
        .fill = colorSpecFromRole(ColorRole::Accent),
        .visible = false,
        .participatesInLayout = false,
    });
    badge->setHitTestVisible(false);
    badge->setZIndex(2);
    m_notificationsButton->addChild(std::move(badge));
  }
  iconButton(m_editButton, "pencil", [this] { setEditing(true); });
  m_editButton->setTooltip(i18n::tr("control-center.quick.edit-tooltip"));
  Button* settings = nullptr;
  iconButton(settings, "settings", [] { PanelManager::instance().openSettingsWindow(); });
  settings->setTooltip(i18n::tr("control-center.quick.settings-tooltip"));
  Button* power = nullptr;
  iconButton(power, "shutdown", [this] {
    TooltipManager::instance().forceDestroy();
    // The power popover (proto overlays.js): Lock, Sleep, Log Out | Restart…, Shut Down….
    static constexpr std::array<std::string_view, 5> kActions{"lock", "suspend", "logout", "reboot", "shutdown"};
    const std::array<std::string, 5> labels{
        i18n::tr("control-center.power-menu.lock"), i18n::tr("control-center.power-menu.sleep"),
        i18n::tr("control-center.power-menu.log-out"), i18n::tr("control-center.power-menu.restart-menu"),
        i18n::tr("control-center.power-menu.shut-down-menu")};
    std::vector<ContextMenuControlEntry> entries;
    for (std::size_t i = 0; i < kActions.size(); ++i) {
      if (i == 3) {
        entries.push_back({.id = -1, .separator = true});
      }
      entries.push_back({.id = static_cast<std::int32_t>(i), .label = labels[i],
                         .glyph = std::string(i == 1 ? std::string_view("moon") : session_action::defaultGlyph(kActions[i]))});
    }
    PanelManager::instance().openPanelMenu(std::move(entries), *m_powerButton, Tokens::componentMenuFixedWidth * contentScale(),
                                           [services = m_services](std::int32_t id) {
                                             runSessionAction(std::string(kActions[static_cast<std::size_t>(id)]), services);
                                           });
  });
  m_powerButton = power;
  power->setTooltip(i18n::tr("control-center.power-menu.title"));
  return footer;
}

// Badge overhang and lift growth (about 7 px), rounded up to the 4 grid.
float QuickPage::editGridMargin() const { return Tokens::spaceP8 * contentScale(); }

void QuickPage::sizePages(Renderer& renderer, float width) {
  if (m_pager == nullptr || m_pages.empty()) {
    return;
  }
  m_pageStep = width + 2.0F * Tokens::componentSheetPadding * contentScale();
  m_pager->setPaging(m_pageStep);
  for (Flex* page : m_pages) {
    page->setMinWidth(width);
    page->setMaxWidth(width);
    page->setMinHeight(0.0F);
  }
  const float half = std::floor((width - Tokens::componentSheetGap * contentScale()) * 0.5F);
  for (Flex* cell : m_halfWidth) {
    cell->setMinWidth(half);
    cell->setMaxWidth(half);
  }
  // The edit grid keeps a margin for the remove badges (they overhang the tile) and the 1.04 lift.
  const float editMargin = editGridMargin();
  const float editHalf = std::floor((width - 2.0F * editMargin - Tokens::componentSheetGap * contentScale()) * 0.5F);
  for (Flex* cell : m_editHalf) {
    cell->setMinWidth(editHalf);
    cell->setMaxWidth(editHalf);
  }
  for (Flex* cell : m_galleryHalf) {
    cell->setMinWidth(half);
    cell->setMaxWidth(half);
  }
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  const float height = m_pages.front()->measure(renderer, constraints).height;
  for (Flex* page : m_pages) {
    page->setMinHeight(height);
  }
  m_pager->setMinHeight(height);
  m_pager->setMaxHeight(height);
  // A horizontal ScrollView keeps its first height; page 1 grows when a Wi-Fi/Bluetooth list opens.
  if (std::abs(m_pager->height() - height) >= 0.5F) {
    m_pager->setSize(m_pager->width(), height);
  }
  m_volumeName->setMaxWidth(width * 0.5F);
  m_brightnessName->setMaxWidth(width * 0.5F);
}

void QuickPage::refreshDots(float offset) {
  if (m_pageStep <= 0.0F || m_pages.empty()) {
    return;
  }
  m_page = std::min(static_cast<std::size_t>(std::lround(std::max(offset, 0.0F) / m_pageStep)), m_pages.size() - 1);
  for (std::size_t i = 0; i < m_dots.size(); ++i) {
    m_dots[i]->setFill(colorSpecFromRole(i == m_page ? ColorRole::Label1 : ColorRole::Label3));
  }
}

void QuickPage::syncPager() {
  if (m_pager == nullptr || m_pageStep <= 0.0F) {
    return;
  }
  refreshDots(m_pager->scrollOffset());
}

float QuickPage::measureHeight(Renderer& renderer, float width) {
  if (m_root == nullptr) {
    return 0.0F;
  }
  sizePages(renderer, width);
  m_root->setSize(width, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  return m_root->measure(renderer, constraints).height;
}

void QuickPage::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  sizePages(renderer, contentWidth);
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
  if (m_editArea != nullptr) {
    m_editArea->setFrameSize(m_editGrid->width(), m_editGrid->height());
  }
  if (m_galleryArea != nullptr) {
    m_galleryArea->setFrameSize(m_editGallery->width(), m_editGallery->height());
  }
  m_editReflowPending = false;
  m_slotPos.clear();
  for (const Flex* card : m_editCards) {
    m_slotPos.emplace_back(offsetIn(card, m_editGrid, false), offsetIn(card, m_editGrid, true));
  }
  if (m_dragIndex >= 0 && static_cast<std::size_t>(m_dragIndex) < m_editCards.size()) {
    const Flex* dragged = m_editCards[static_cast<std::size_t>(m_dragIndex)];
    std::erase_if(m_flipFrom, [dragged](const auto& entry) { return entry.first == dragged; });
  }
  animateFlip();
  placeDraggedTile();
}

// The dragged tile sits under the pointer, above the other rows; layout and FLIP never move it.
void QuickPage::placeDraggedTile() {
  if (m_dragIndex < 0 || static_cast<std::size_t>(m_dragIndex) >= m_editCards.size() || m_editGrid == nullptr) {
    return;
  }
  Flex* card = m_editCards[static_cast<std::size_t>(m_dragIndex)];
  Node* row = card->parent();
  if (row == nullptr) {
    return;
  }
  row->setZIndex(5);
  card->setPosition(
      m_dragX - m_grabX - offsetIn(row, m_editGrid, false), m_dragY - m_grabY - offsetIn(row, m_editGrid, true)
  );
}

void QuickPage::animateScale(Flex& card, float to) {
  AnimationManager* manager = card.animationManager();
  if (manager == nullptr) {
    card.setScale(to);
    return;
  }
  Flex* target = &card;
  manager->animateSpring(
      card.scaleX(), to, springOf(Tokens::springSnap), [target](float v) { target->setScale(v); }, {}, target
  );
}

// The tiles that changed slot slide from where they were to where layout put them.
void QuickPage::animateFlip() {
  for (const auto& [card, from] : m_flipFrom) {
    AnimationManager* manager = card->animationManager();
    const float toX = card->x();
    const float toY = card->y();
    const float dx = from.first - offsetIn(card, m_editGrid, false) + toX;
    const float dy = from.second - offsetIn(card, m_editGrid, true) + toY;
    if (manager == nullptr || (std::abs(dx - toX) < 0.5F && std::abs(dy - toY) < 0.5F)) {
      continue;
    }
    Flex* target = card;
    card->setPosition(dx, dy);
    manager->animateSpring(
        0.0F, 1.0F, springOf(Tokens::springMove),
        [this, target, dx, dy, toX, toY](float t) {
          if (m_dragIndex >= 0 && static_cast<std::size_t>(m_dragIndex) < m_editCards.size()
              && m_editCards[static_cast<std::size_t>(m_dragIndex)] == target) {
            return;
          }
          target->setPosition(dx + (toX - dx) * t, dy + (toY - dy) * t);
        },
        {},
        target, 0.0F, Unit::Progress
    );
  }
  m_flipFrom.clear();
}

void QuickPage::fadeIn(Node& node, float delayMs) {
  (void)delayMs;
  AnimationManager* manager = node.animationManager();
  if (manager == nullptr) {
    return;
  }
  Node* target = &node;
  target->setOpacity(0.0F);
  manager->animateSpring(0.0F, 1.0F, springOf(Tokens::springFade), [target](float v) { target->setOpacity(v); }, {}, target, 0.0F, Unit::Progress);
}

void QuickPage::startStagger() {
  // Rulebook P5: tiles stagger in after the content gate (35 % of the open spring), 8 ms apart, six at most.
  const int kGateMs = static_cast<int>(PanelManager::openContentGate().count());
  constexpr int kStaggerMs = 8;
  for (std::size_t i = 0; i < std::min(m_extras.size(), m_staggerTimers.size()); ++i) {
    Flex* card = m_extras[i].card;
    if (card == nullptr) {
      continue;
    }
    card->setOpacity(0.0F);
    m_staggerTimers[i].start(std::chrono::milliseconds(kGateMs + kStaggerMs * static_cast<int>(i)), [this, i] {
      if (i < m_extras.size() && m_extras[i].card != nullptr && !PanelManager::instance().pieceInMotion(*m_extras[i].card)) {
        fadeIn(*m_extras[i].card);
        PanelManager::instance().requestRedraw();
      }
    });
  }
}

void QuickPage::syncToggleRow(ToggleRow& row, const std::string& subtitle) {
  if (row.shortcut == nullptr || row.icon == nullptr) {
    return;
  }
  const bool active = row.shortcut->active();
  row.icon->setGlyph(!row.glyph.empty() ? row.glyph : row.shortcut->currentIcon());
  row.icon->setVariant(active ? ButtonVariant::Primary : (row.drop ? ButtonVariant::Ghost : ButtonVariant::Tonal));
  row.icon->setEnabled(row.shortcut->enabled());
  row.subtitle->setText(subtitle);
  if (row.body != nullptr) {
    row.body->setTooltip(row.title->text() + "\n" + subtitle);
  }
}

void QuickPage::syncExtras() {
  for (ToggleRow& row : m_extras) {
    Shortcut* shortcut = row.shortcut.get();
    std::string subtitle;
    if (shortcut->id() == "session") {
      subtitle = i18n::tr("control-center.quick.power-hint");
    } else if (shortcut->id() == "mic_mute") {
      subtitle = i18n::tr(shortcut->active() ? "control-center.quick.muted" : "control-center.quick.on");
    } else {
      const std::string label = shortcut->displayLabel();
      const bool named = shortcut->id() != "notification" && shortcut->id() != "dark_mode";
      subtitle = shortcut->active() && named && label != shortcut->defaultLabel()
          ? label
          : i18n::tr(shortcut->active() ? "control-center.quick.on" : "control-center.quick.off");
    }
    const bool connecting = shortcut->id() == "wifi" && m_services.network != nullptr && m_services.network->state().resolving;
    if (connecting) {
      subtitle = i18n::tr("control-center.quick.connecting");
    }
    syncToggleRow(row, subtitle);
    if (row.spinner != nullptr) {
      row.spinner->setVisible(connecting);
      connecting ? row.spinner->start() : row.spinner->stop();
    }
  }
}

void QuickPage::syncFooter() {
  const UPowerState* state = m_services.upower != nullptr ? &m_services.upower->state() : nullptr;
  const bool battery = state != nullptr && state->isPresent;
  const bool profile = m_services.powerProfiles != nullptr && !m_services.powerProfiles->activeProfile().empty();
  m_batteryBox->setVisible(battery);
  m_profileBox->setVisible(profile);
  m_batteryBox->parent()->setVisible(battery || profile);
  m_footer->setVisible(true);
  if (battery) {
    m_batteryGlyph->setGlyph(batteryGlyphName(state->percentage, state->state));
    // The glyph already shows the charge state (plugged in, charging), so the text is the level only.
    m_batteryText->setText(std::format("{:.0F} %", state->percentage));
  }
  const std::size_t unread = m_services.notifications != nullptr ? m_services.notifications->unreadNotificationCount() : 0;
  m_unreadBadge->setVisible(unread > 0);
  if (unread > 0) {
    // A dot at the bell's top right, as on the bar bell; no count.
    const float s = contentScale();
    const float d = 7.0F * s;
    const float b = Tokens::componentButtonHeightM * s;
    m_unreadBadge->setSize(d, d);
    m_unreadBadge->setRadius(d * 0.5F);
    m_unreadBadge->setPosition(std::round(b * 0.5F + 3.0F * s), std::round(b * 0.5F - 9.0F * s));
  }
  if (profile) {
    m_profileText->setText(m_profile->displayLabel());
  }
}

void QuickPage::syncSliders() {
  if (m_brightnessSlider->dragging() || m_volumeSlider->dragging()) {
    return;
  }
  m_syncingSliders = true;
  const BrightnessDisplay* display = nullptr;
  if (m_services.brightness != nullptr) {
    for (const BrightnessDisplay& candidate : m_services.brightness->displays()) {
      if (candidate.controllable) {
        display = &candidate;
        break;
      }
    }
  }
  m_brightnessPill->setVisible(display != nullptr);
  if (display != nullptr) {
    m_brightnessSlider->setValue(display->brightness);
    m_brightnessValue->setText(percent(display->brightness));
  }

  const AudioNode* sink = m_services.audio != nullptr ? m_services.audio->defaultSink() : nullptr;
  m_volumePill->setVisible(sink != nullptr);
  m_volumeSlider->setValue(sink != nullptr ? sink->volume : 0.0);
  m_volumeName->setText(sink == nullptr ? std::string{} : (!sink->description.empty() ? sink->description : sink->name));
  m_volumeValue->setText(sink == nullptr ? std::string{} : percent(static_cast<float>(sink->volume)));

  m_syncingSliders = false;
}

void QuickPage::syncMedia(Renderer& renderer) {
  const auto player = m_services.mpris != nullptr ? m_services.mpris->activePlayer() : std::nullopt;
  m_mediaCard->setVisible(player.has_value());
  if (!player.has_value()) {
    return;
  }
  m_mediaTitle->setText(player->title);
  std::string artist = joinedArtists(player->artists);
  if (m_services.link != nullptr && player->busName.starts_with(kPhonePlayerPrefix)) {
    const std::string_view id = std::string_view(player->busName).substr(kPhonePlayerPrefix.size());
    for (const LinkDevice& device : m_services.link->devices()) {
      if (device.id == id) {
        const std::string on = i18n::tr("control-center.detail.phone.on-phone", "phone", device.name);
        artist = artist.empty() ? on : artist + " · " + on;
        break;
      }
    }
  }
  m_mediaArtist->setText(artist);
  m_playButton->setGlyph(player->playbackStatus == "Playing" ? "player-pause" : "player-play");

  const std::string url = mpris::effectiveArtUrl(*player);
  if (url == m_mediaArtUrl && (url.empty() || m_mediaArt->hasImage())) {
    return;
  }
  const std::string path = mpris::resolveArtworkSource(
      m_services.httpClient, m_pendingArt, url, [this] {
        m_mediaArtUrl.clear();
        PanelManager::instance().refresh();
      }, m_aliveGuard
  );
  const auto decode = static_cast<int>(std::ceil(Tokens::componentMediaArt * contentScale() * 2.0F));
  if (path.empty() || !m_mediaArt->setSourceFile(renderer, path, decode, true, true)) {
    m_mediaArt->clear(renderer);
    m_mediaArtUrl = path.empty() && !url.empty() ? std::string{} : url;
    return;
  }
  m_mediaArtUrl = url;
}

void QuickPage::syncPhone() {
  const LinkDevice* phone = m_services.link != nullptr ? phone::primary(*m_services.link) : nullptr;
  m_phoneCard->setVisible(phone != nullptr);
  m_phoneId = phone != nullptr && phone->connected ? phone->id : std::string{};
  if (phone == nullptr) {
    return;
  }
  m_phoneName->setText(phone->name);
  m_phoneCard->setOpacity(phone->connected ? 1.0F : 0.6F);
  m_phoneKeys[0]->setEnabled(phone->connected);
  m_phoneKeys[1]->setEnabled(phone->connected);
  std::string line;
  const auto append = [&line](const std::string& part) { line += line.empty() ? part : " · " + part; };
  if (!phone->connected) {
    append(i18n::tr(m_services.link->paused(phone->id)       ? "control-center.detail.phone.paused"
                    : m_services.link->connecting(phone->id) ? "control-center.detail.phone.connecting"
                                                             : "bar.widgets.phone.not-connected"));
  } else if (const LinkStatus* status = m_services.link->status(phone->id); status != nullptr) {
    append(std::format("{}%", status->battery));
  }
  if (const std::uint32_t unread = m_services.link->notificationCount(phone->id); phone->connected && unread > 0) {
    append(i18n::tr("control-center.quick.notifications", "count", unread));
  }
  m_phoneStatus->setText(line);
}

float QuickPage::offsetIn(const Node* node, const Node* ancestor, bool y) {
  float total = 0.0F;
  for (; node != nullptr && node != ancestor; node = node->parent()) {
    total += y ? node->y() : node->x();
  }
  return total;
}

namespace {
  // Removes every child except `keep`: the persistent input area outlives the rebuilds its callbacks trigger.
  void clearChildren(Node& node, const Node* keep = nullptr) {
    std::vector<Node*> doomed;
    for (const auto& child : node.children()) {
      if (child.get() != keep) {
        doomed.push_back(child.get());
      }
    }
    for (Node* child : doomed) {
      node.removeChild(child);
    }
  }

  std::unique_ptr<Flex> makeBadge(bool add, float s) {
    const float size = (add ? Tokens::componentEditBadgeAdd : Tokens::componentEditBadgeRemove) * s;
    auto badge = std::make_unique<Flex>();
    badge->setDirection(FlexDirection::Horizontal);
    badge->setAlign(FlexAlign::Center);
    badge->setJustify(FlexJustify::Center);
    badge->setFill(colorSpecFromRole(add ? ColorRole::Accent : ColorRole::Label2));
    badge->setRadius(size * 0.5F);
    badge->setParticipatesInLayout(false);
    badge->setZIndex(3);
    auto glyph = std::make_unique<Glyph>();
    glyph->setGlyph(add ? "plus" : "minus");
    glyph->setGlyphSize((add ? 16.0F : 12.0F) * s);
    glyph->setColor(colorSpecFromRole(ColorRole::OnAccent));
    badge->addChild(std::move(glyph));
    return badge;
  }
} // namespace

void QuickPage::buildEditParts(Flex& root) {
  const float s = contentScale();
  auto header = std::make_unique<Flex>();
  header->setDirection(FlexDirection::Horizontal);
  header->setAlign(FlexAlign::Center);
  header->setMinHeight(Tokens::componentButtonHeightM * s);
  header->setPadding(0.0F, Tokens::componentTilePadding * s, 0.0F, Tokens::componentTilePadding * s);
  header->setVisible(false);
  Label* title = nullptr;
  auto titleLabel = makeLabel(title, Tokens::typeTitle3, s, ColorRole::Label1);
  titleLabel->setFlexGrow(1.0F);
  title->setText(i18n::tr("control-center.quick.edit-controls"));
  header->addChild(std::move(titleLabel));
  auto done = std::make_unique<Button>();
  done->setVariant(ButtonVariant::Primary);
  done->setText(i18n::tr("control-center.quick.done"));
  done->setControlHeight(Tokens::componentButtonHeightS * s);
  done->setRadius(Tokens::componentButtonHeightS * 0.5F * s);
  done->setPadding(0.0F, Tokens::componentButtonInsetS * s, 0.0F, Tokens::componentButtonInsetS * s);
  done->setOnClick([this] { setEditing(false); });
  header->addChild(std::move(done));
  m_editHeader = header.get();
  root.addChild(std::move(header));

  auto grid = std::make_unique<Flex>();
  grid->setDirection(FlexDirection::Vertical);
  grid->setAlign(FlexAlign::Stretch);
  grid->setGap(Tokens::componentSheetGap * s);
  grid->setPadding(editGridMargin(), editGridMargin(), 0.0F, editGridMargin());
  grid->setVisible(false);
  m_editGrid = grid.get();
  auto area = std::make_unique<InputArea>();
  area->setParticipatesInLayout(false);
  area->setZIndex(10);
  m_editArea = area.get();
  area->setOnPress([this, s](const InputArea::PointerData& d) {
    if (d.button != BTN_LEFT) {
      return;
    }
    if (d.pressed) {
      m_pressedIndex = editSlotAt(d.localX, d.localY);
      m_pressedBadge = false;
      m_dragMoved = false;
      m_pressX = d.localX;
      m_pressY = d.localY;
      if (m_pressedIndex >= 0) {
        const Flex* card = m_editCards[static_cast<std::size_t>(m_pressedIndex)];
        const float reach = (Tokens::componentEditBadgeRemove - Tokens::componentEditBadgeInset) * s;
        const float cx = offsetIn(card, m_editGrid, false);
        const float cy = offsetIn(card, m_editGrid, true);
        m_pressedBadge = d.localX < cx + reach + 4.0F * s && d.localY < cy + reach + 4.0F * s;
      }
      return;
    }
    const int index = m_pressedIndex;
    const bool badge = m_pressedBadge;
    if (m_dragIndex >= 0 && static_cast<std::size_t>(m_dragIndex) < m_editCards.size()) {
      Flex* card = m_editCards[static_cast<std::size_t>(m_dragIndex)];
      animateScale(*card, 1.0F);
      if (card->parent() != nullptr) {
        card->parent()->setZIndex(0);
      }
      m_flipFrom.clear();
      m_flipFrom.push_back({card, {offsetIn(card, m_editGrid, false), offsetIn(card, m_editGrid, true)}});
      PanelManager::instance().requestLayout();
    }
    m_dragIndex = -1;
    m_pressedIndex = -1;
    if (index >= 0 && badge && !m_dragMoved && m_editOrder.size() > 1) {
      m_editOrder.erase(m_editOrder.begin() + index);
      m_editRebuildPending = true;
      PanelManager::instance().refresh();
    }
  });
  area->setOnMotion([this, s](const InputArea::PointerData& d) {
    if (m_pressedIndex < 0 || m_pressedBadge || m_editArea == nullptr || !m_editArea->pressed() || m_editReflowPending) {
      return;
    }
    if (m_dragIndex < 0) {
      if (std::hypot(d.localX - m_pressX, d.localY - m_pressY) < 4.0F * s) {
        return;
      }
      m_dragIndex = m_pressedIndex;
      m_dragMoved = true;
      Flex* card = m_editCards[static_cast<std::size_t>(m_dragIndex)];
      m_grabX = m_pressX - offsetIn(card, m_editGrid, false);
      m_grabY = m_pressY - offsetIn(card, m_editGrid, true);
      card->setTransformOrigin(card->width() * 0.5F, card->height() * 0.5F);
      animateScale(*card, Tokens::componentEditDragScale);
      card->setZIndex(5);
    }
    m_dragX = d.localX;
    m_dragY = d.localY;
    placeDraggedTile();
    syncLiftShadow();
    PanelManager::instance().requestRedraw();
    const int slot = editSlotAt(d.localX, d.localY);
    if (slot < 0 || slot == m_dragIndex) {
      return;
    }
    const auto from = static_cast<std::ptrdiff_t>(m_dragIndex);
    const auto to = static_cast<std::ptrdiff_t>(slot);
    if (from < to) {
      std::rotate(m_editOrder.begin() + from, m_editOrder.begin() + from + 1, m_editOrder.begin() + to + 1);
      std::rotate(m_editCards.begin() + from, m_editCards.begin() + from + 1, m_editCards.begin() + to + 1);
    } else {
      std::rotate(m_editOrder.begin() + to, m_editOrder.begin() + from, m_editOrder.begin() + from + 1);
      std::rotate(m_editCards.begin() + to, m_editCards.begin() + from, m_editCards.begin() + from + 1);
    }
    m_dragIndex = slot;
    m_editReflowPending = true;
    m_flipFrom.clear();
    for (Flex* card : m_editCards) {
      m_flipFrom.push_back({card, {offsetIn(card, m_editGrid, false), offsetIn(card, m_editGrid, true)}});
    }
    reflowEditGrid();
    m_editCards[static_cast<std::size_t>(slot)]->setScale(Tokens::componentEditDragScale);
    PanelManager::instance().requestLayout();
  });
  grid->addChild(std::move(area));
  root.addChild(std::move(grid));

  auto gallery = std::make_unique<Flex>();
  gallery->setDirection(FlexDirection::Vertical);
  gallery->setAlign(FlexAlign::Stretch);
  gallery->setGap(Tokens::componentSheetGap * s);
  gallery->setVisible(false);
  m_editGallery = gallery.get();
  auto galleryArea = std::make_unique<InputArea>();
  galleryArea->setParticipatesInLayout(false);
  galleryArea->setZIndex(10);
  m_galleryArea = galleryArea.get();
  galleryArea->setOnClick([this](const InputArea::PointerData& d) {
    for (std::size_t i = 0; i < m_galleryCards.size(); ++i) {
      const Flex* card = m_galleryCards[i];
      const float cx = offsetIn(card, m_editGallery, false);
      const float cy = offsetIn(card, m_editGallery, true);
      if (d.localX >= cx && d.localX < cx + card->width() && d.localY >= cy && d.localY < cy + card->height()) {
        m_editOrder.push_back(m_galleryIds[i]);
        m_editRebuildPending = true;
        PanelManager::instance().refresh();
        return;
      }
    }
  });
  gallery->addChild(std::move(galleryArea));
  root.addChild(std::move(gallery));
}

// The dragged tile lifts to E3 (rulebook 9.16): a soft shadow under it, behind the rows of the grid.
void QuickPage::syncLiftShadow() {
  const bool lifted = m_editing && m_editGrid != nullptr && m_dragIndex >= 0
      && static_cast<std::size_t>(m_dragIndex) < m_editCards.size();
  if (!lifted) {
    if (m_liftShadow != nullptr && m_liftShadow->parent() != nullptr) {
      m_liftShadow->setVisible(false);
    }
    return;
  }
  const float s = contentScale();
  if (m_liftShadow == nullptr || m_liftShadow->parent() != m_editGrid) {
    auto shadow = ui::box({.visible = false, .participatesInLayout = false});
    shadow->setSoftness(Tokens::elevationE3.keyBlur * s);
    shadow->setFill(colorSpecFromRole(
        ColorRole::Shadow, isResolvedLightTheme() ? Tokens::elevationE3.keyLight : Tokens::elevationE3.keyDark
    ));
    shadow->setZIndex(-1);
    m_liftShadow = static_cast<Box*>(m_editGrid->addChild(std::move(shadow)));
  }
  const Flex* card = m_editCards[static_cast<std::size_t>(m_dragIndex)];
  const float scale = card->scaleX();
  const float w = card->width() * scale;
  const float h = card->height() * scale;
  m_liftShadow->setVisible(true);
  m_liftShadow->setRadius(Tokens::componentTileRadius * s * scale);
  m_liftShadow->setPosition(
      offsetIn(card, m_editGrid, false) + (card->width() - w) * 0.5F,
      offsetIn(card, m_editGrid, true) + (card->height() - h) * 0.5F + Tokens::elevationE3.keyY * s
  );
  m_liftShadow->setFrameSize(w, h);
}

void QuickPage::reflowEditGrid() {
  const float s = contentScale();
  std::vector<std::unique_ptr<Flex>> held;
  for (Flex* card : m_editCards) {
    auto node = card->parent()->removeChild(card);
    held.emplace_back(static_cast<Flex*>(node.release()));
  }
  clearChildren(*m_editGrid, m_editArea);
  m_liftShadow = nullptr;
  m_editRows.clear();
  m_editCards.clear();
  m_editHalf.clear();
  // Edit mode lays every tile out as a drop, kDropsPerRow to a row; empty slots keep the columns.
  const float drop = Tokens::componentTileHeight * s;
  for (std::size_t i = 0; i < held.size(); i += kDropsPerRow) {
    auto row = std::make_unique<Flex>();
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Center);
    row->setJustify(FlexJustify::SpaceBetween);
    for (std::size_t j = i; j < i + kDropsPerRow; ++j) {
      if (j < held.size()) {
        m_editCards.push_back(held[j].get());
        row->addChild(std::move(held[j]));
      } else {
        auto slot = std::make_unique<Flex>();
        slot->setMinWidth(drop);
        row->addChild(std::move(slot));
      }
    }
    m_editRows.push_back(row.get());
    m_editGrid->addChild(std::move(row));
  }
}

int QuickPage::editSlotAt(float x, float y) const {
  const float pad = Tokens::componentSheetGap * contentScale() * 0.5F;
  const bool laidOut = m_slotPos.size() == m_editCards.size();
  for (std::size_t i = 0; i < m_editCards.size(); ++i) {
    const Flex* card = m_editCards[i];
    // Slots come from layout, not from tiles that are sliding or under the pointer, so a swap cannot undo itself.
    const float cx = laidOut ? m_slotPos[i].first : offsetIn(card, m_editGrid, false);
    const float cy = laidOut ? m_slotPos[i].second : offsetIn(card, m_editGrid, true);
    if (x >= cx - pad && x < cx + card->width() + pad && y >= cy - pad && y < cy + card->height() + pad) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void QuickPage::rebuildEdit() {
  const float s = contentScale();
  m_extras.clear();
  m_galleryCards.clear();
  m_galleryIds.clear();
  m_editCards.clear();
  clearChildren(*m_editGrid, m_editArea);
  m_liftShadow = nullptr;
  clearChildren(*m_editGallery, m_galleryArea);
  m_editRows.clear();
  m_editHalf.clear();
  m_galleryHalf.clear();

  std::vector<std::unique_ptr<Flex>> cards;
  std::vector<std::string> built; // ids that have a card: card index == order index
  for (const std::string& id : m_editOrder) {
    auto card = makeDropTile(id);
    if (card == nullptr) {
      continue;
    }
    built.push_back(id);
    auto* badged = static_cast<BadgedCard*>(card.get());
    badged->scale = s;
    auto badge = makeBadge(false, s);
    badged->badge = badge.get();
    card->addChild(std::move(badge));
    m_editCards.push_back(card.get());
    cards.push_back(std::move(card));
  }
  m_editOrder = std::move(built);
  m_editCards.clear();
  // Park the cards in a temporary row so reflowEditGrid() can take them over.
  auto parking = std::make_unique<Flex>();
  for (auto& card : cards) {
    m_editCards.push_back(card.get());
    parking->addChild(std::move(card));
  }
  m_editGrid->addChild(std::move(parking));
  reflowEditGrid();

  std::vector<std::unique_ptr<Flex>> gallery;
  // The gallery offers every tile plus controls that are not tiles.
  std::vector<std::string> galleryIds = tileIdsAll();
  galleryIds.emplace_back("session");
  for (const std::string& id : galleryIds) {
    if (!tileAvailable(id) || std::ranges::find(m_editOrder, id) != m_editOrder.end()) {
      continue;
    }
    auto card = makeDropTile(id);
    if (card == nullptr) {
      continue;
    }
    auto* badged = static_cast<BadgedCard*>(card.get());
    badged->scale = s;
    badged->addMode = true;
    card->setFill(colorSpecFromRole(ColorRole::Fill1, 0.0F));
    card->setBorder(colorSpecFromRole(ColorRole::Separator), 1.0F);
    card->setOpacity(0.72F);
    auto badge = makeBadge(true, s);
    badged->badge = badge.get();
    card->addChild(std::move(badge));
    m_galleryIds.push_back(id);
    m_galleryCards.push_back(card.get());
    gallery.push_back(std::move(card));
  }
  if (!gallery.empty()) {
    Label* title = nullptr;
    auto titleLabel = makeLabel(title, Tokens::typeFootnote, s, ColorRole::Label1);
    title->setText(i18n::tr("control-center.quick.add-controls"));
    m_editGallery->addChild(std::move(titleLabel));
    const float drop = Tokens::componentTileHeight * s;
    for (std::size_t i = 0; i < gallery.size(); i += kDropsPerRow) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setJustify(FlexJustify::SpaceBetween);
      for (std::size_t j = i; j < i + kDropsPerRow; ++j) {
        if (j < gallery.size()) {
          row->addChild(std::move(gallery[j]));
        } else {
          auto slot = std::make_unique<Flex>();
          slot->setMinWidth(drop);
          row->addChild(std::move(slot));
        }
      }
      m_editGallery->addChild(std::move(row));
    }
  }
  Label* hint = nullptr;
  auto hintLabel = makeLabel(hint, Tokens::typeFootnote, s, ColorRole::Label3);
  hint->setText(i18n::tr("control-center.quick.edit-hint"));
  m_editGallery->addChild(std::move(hintLabel));
  syncExtras();
  m_editReflowPending = true;
  PanelManager::instance().requestLayout();
}

void QuickPage::saveEdit() {
  if (m_services.config == nullptr) {
    return;
  }
  const std::vector<std::string> saved = savedTileOrder();
  std::vector<std::string> next = m_editOrder;
  for (std::size_t i = 0; i < saved.size(); ++i) {
    const std::string& id = saved[i];
    if (!tileAvailable(id) && std::ranges::find(tileIdsAll(), id) != tileIdsAll().end() &&
        std::ranges::find(next, id) == next.end()) {
      next.insert(next.begin() + static_cast<std::ptrdiff_t>(std::min(i, next.size())), id);
    }
  }
  if (next != saved) {
    m_services.config->setOverride({"control_center", "quick_tiles"}, next);
  }
}

void QuickPage::onClose() {
  for (Timer& timer : m_staggerTimers) {
    timer.stop();
  }
  if (m_editing) {
    saveEdit();
    m_editing = false;
  }
  m_mediaKeys = {};
  m_phoneKeys = {};
  m_liftShadow = nullptr;
}

void QuickPage::setEditing(bool editing) {
  if (m_editing == editing || m_root == nullptr || m_editGrid == nullptr) {
    return;
  }
  m_editing = editing;
  m_editRebuildPending = false;
  if (editing) {
    m_editOrder.clear();
    for (const std::string& id : tileOrder()) {
      if (tileAvailable(id)) {
        m_editOrder.push_back(id);
      }
    }
    rebuildEdit();
    for (const std::vector<Flex*>* cards : {&m_editCards, &m_galleryCards}) {
      for (Flex* card : *cards) {
        if (auto* badge = static_cast<BadgedCard*>(card)->badge; badge != nullptr) {
          fadeIn(*badge);
        }
      }
    }
  } else {
    saveEdit();
    m_extras.clear();
    clearChildren(*m_editGrid, m_editArea);
    clearChildren(*m_editGallery, m_galleryArea);
    m_editCards.clear();
    m_editRows.clear();
    m_editHalf.clear();
    m_galleryHalf.clear();
    m_galleryCards.clear();
    m_galleryIds.clear();
    buildPages(m_editOrder);
    m_pager->setScrollOffset(0.0F);
    m_page = 0;
    refreshDots(0.0F);
    syncSliders();
    syncPhone();
    if (m_mediaCard != nullptr) {
      m_mediaCard->setVisible(m_services.mpris != nullptr && m_services.mpris->activePlayer().has_value());
    }
  }
  for (Node* part : m_normalParts) {
    // The dots row, the sliders, media and phone cards own their visibility (fillDots and the sync functions).
    if (!editing && (part == m_dotsRow || part == m_brightnessPill || part == m_mediaCard || part == m_phoneCard)) {
      continue;
    }
    part->setVisible(!editing);
  }
  m_editHeader->setVisible(editing);
  m_editGrid->setVisible(editing);
  m_editGallery->setVisible(editing);
  PanelManager::instance().requestLayout();
}

Label* QuickPage::sharedTitleFor(std::string_view detailKey) {
  if (detailKey == "detail-phone") {
    return m_phoneCard != nullptr && m_phoneCard->visible() ? m_phoneName : nullptr;
  }
  for (const ToggleRow& row : m_extras) {
    if (row.detail == detailKey && row.title != nullptr && row.body != nullptr && row.body->visible()) {
      return row.title;
    }
  }
  return nullptr;
}

void QuickPage::setActive(bool active) {
  if (active && m_staggerPending) {
    m_staggerPending = false;
    startStagger();
  }
  // Focus moves now, not in the next update: a key that arrives first would reach the hidden page's text field.
  m_focusFirstTile = active && !m_editing;
  if (m_focusFirstTile && m_root != nullptr && !m_extras.empty()) {
    m_focusFirstTile = false;
    moveFocus(0);
  }
}

bool QuickPage::focusSlider(Slider* slider) {
  if (slider != nullptr && slider->inputArea() != nullptr) {
    PanelManager::instance().inputDispatcher().setKeyboardFocus(slider->inputArea());
  }
  return true;
}

bool QuickPage::handleSliderKey(std::uint32_t sym, std::uint32_t modifiers, const InputArea* focused) {
  Slider* slider = nullptr;
  if (m_brightnessSlider != nullptr && focused == m_brightnessSlider->inputArea()) {
    slider = m_brightnessSlider;
  } else if (m_volumeSlider != nullptr && focused == m_volumeSlider->inputArea()) {
    slider = m_volumeSlider;
  }
  if (slider == nullptr) {
    return false;
  }
  const double step = (modifiers & KeyMod::Shift) != 0 ? 0.01 : 0.05;
  switch (sym) {
  case XKB_KEY_Left:
    slider->setValue(slider->value() - step);
    return true;
  case XKB_KEY_Right:
    slider->setValue(slider->value() + step);
    return true;
  case XKB_KEY_Down:
    if (slider == m_brightnessSlider) {
      return focusSlider(m_volumeSlider);
    }
    (void)handleCardKey(sym, focused);
    return true;
  case XKB_KEY_Up:
    if (slider == m_volumeSlider && m_brightnessPill->visible()) {
      return focusSlider(m_brightnessSlider);
    }
    return moveFocus(tileToward(-1, 0, -1), true);
  default:
    return false;
  }
}

bool QuickPage::moveFocus(int index, bool keyboard) {
  if (index < 0 || static_cast<std::size_t>(index) >= m_extras.size() || m_extras[static_cast<std::size_t>(index)].body == nullptr) {
    return true;
  }
  const Node* card = m_extras[static_cast<std::size_t>(index)].card;
  for (std::size_t page = 0; page < m_pages.size(); ++page) {
    for (const Node* n = card; n != nullptr; n = n->parent()) {
      if (n == m_pages[page]) {
        goToPage(page);
      }
    }
  }
  InputArea* area = m_extras[static_cast<std::size_t>(index)].body->inputArea();
  auto& dispatcher = PanelManager::instance().inputDispatcher();
  if (keyboard) {
    dispatcher.setKeyboardFocus(area);
  } else {
    dispatcher.setFocus(area);
  }
  return true;
}

// The tile nearest to tile `from` in direction (dx, dy) by card centres: the nearest row first, then the nearest
// column; -1 when none. from -1 with dy -1 starts below the grid (Up from the sliders reaches the last row).
int QuickPage::tileToward(int from, int dx, int dy) const {
  const auto centre = [](const Node* n) {
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(n, x, y);
    return std::pair{x + n->width() * 0.5F, y + n->height() * 0.5F};
  };
  const auto shown = [this](std::size_t i) {
    const Node* card = m_extras[i].card;
    return card != nullptr && m_extras[i].body != nullptr && card->visible() && card->width() > 0.0F;
  };
  float fx = 0.0F;
  float fy = 1.0e6F;
  float rowHalf = 0.0F;
  if (from >= 0) {
    const Node* card = m_extras[static_cast<std::size_t>(from)].card;
    std::tie(fx, fy) = centre(card);
    rowHalf = card->height() * 0.5F;
  }
  int best = -1;
  float bestMain = std::numeric_limits<float>::max();
  float bestCross = std::numeric_limits<float>::max();
  for (std::size_t i = 0; i < m_extras.size(); ++i) {
    if (static_cast<int>(i) == from || !shown(i)) {
      continue;
    }
    const auto [cx, cy] = centre(m_extras[i].card);
    const float ddx = cx - fx;
    const float ddy = cy - fy;
    const bool sameRow = std::abs(ddy) < std::max(rowHalf, m_extras[i].card->height() * 0.5F);
    float main = 0.0F;
    float cross = 0.0F;
    if (dx != 0) {
      if (!sameRow || ddx * static_cast<float>(dx) <= 0.0F) {
        continue;
      }
      main = std::abs(ddx);
    } else {
      if (sameRow || ddy * static_cast<float>(dy) <= 0.0F) {
        continue;
      }
      main = std::round(std::abs(ddy));
      cross = from >= 0 ? std::abs(ddx) : cx;
    }
    if (main < bestMain || (main == bestMain && cross < bestCross)) {
      best = static_cast<int>(i);
      bestMain = main;
      bestCross = cross;
    }
  }
  return best;
}

// The media and phone cards are rows of buttons below the volume pill: Down reaches them, Left and Right move along a row.
bool QuickPage::handleCardKey(std::uint32_t sym, const InputArea* focused) {
  const std::array<std::array<Button*, 3>*, 2> rows{&m_mediaKeys, &m_phoneKeys};
  const std::array<const Node*, 2> cards{m_mediaCard, m_phoneCard};
  const auto shown = [&](std::size_t row) { return cards[row] != nullptr && cards[row]->visible(); };
  // Focuses the enabled button nearest to `column` in the row, none when the row has none.
  const auto focusRow = [&](std::size_t row, std::size_t column) {
    Button* best = nullptr;
    std::size_t bestDistance = rows[row]->size();
    for (std::size_t i = 0; i < rows[row]->size(); ++i) {
      Button* candidate = (*rows[row])[i];
      const std::size_t distance = i > column ? i - column : column - i;
      if (candidate != nullptr && candidate->enabled() && candidate->inputArea() != nullptr && distance < bestDistance) {
        best = candidate;
        bestDistance = distance;
      }
    }
    if (best == nullptr) {
      return false;
    }
    PanelManager::instance().inputDispatcher().setKeyboardFocus(best->inputArea());
    return true;
  };
  if (m_volumeSlider != nullptr && focused == m_volumeSlider->inputArea() && sym == XKB_KEY_Down) {
    for (std::size_t row = 0; row < rows.size(); ++row) {
      if (shown(row) && focusRow(row, 0)) {
        return true;
      }
    }
    return false;
  }
  for (std::size_t row = 0; row < rows.size(); ++row) {
    for (std::size_t column = 0; column < rows[row]->size(); ++column) {
      Button* button = (*rows[row])[column];
      if (button == nullptr || button->inputArea() != focused) {
        continue;
      }
      switch (sym) {
      case XKB_KEY_Left:
        for (std::size_t next = column; next-- > 0;) {
          Button* candidate = (*rows[row])[next];
          if (candidate != nullptr && candidate->enabled()) {
            return focusRow(row, next);
          }
        }
        return true;
      case XKB_KEY_Right:
        for (std::size_t next = column + 1; next < rows[row]->size(); ++next) {
          Button* candidate = (*rows[row])[next];
          if (candidate != nullptr && candidate->enabled()) {
            return focusRow(row, next);
          }
        }
        return true;
      case XKB_KEY_Down:
        for (std::size_t next = row + 1; next < rows.size(); ++next) {
          if (shown(next) && focusRow(next, column)) {
            return true;
          }
        }
        return true;
      case XKB_KEY_Up:
        for (std::size_t previous = row; previous-- > 0;) {
          if (shown(previous) && focusRow(previous, column)) {
            return true;
          }
        }
        return focusSlider(m_volumeSlider);
      default:
        return false;
      }
    }
  }
  return false;
}

bool QuickPage::handleKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }
  if (m_editing) {
    if (sym == XKB_KEY_Escape) {
      setEditing(false);
      return true;
    }
    return false;
  }
  const InputArea* focused = PanelManager::instance().inputDispatcher().focusedArea();
  if (focused != nullptr && handleSliderKey(sym, modifiers, focused)) {
    return true;
  }
  if (focused != nullptr && handleCardKey(sym, focused)) {
    return true;
  }
  int at = -1;
  for (std::size_t i = 0; i < m_extras.size(); ++i) {
    if (m_extras[i].body != nullptr && focused != nullptr && m_extras[i].body->inputArea() == focused) {
      at = static_cast<int>(i);
    }
  }
  if (at >= 0) {
    const auto index = static_cast<std::size_t>(at);
    switch (sym) {
    case XKB_KEY_Left:
      return moveFocus(tileToward(at, -1, 0), true);
    case XKB_KEY_Right:
      return moveFocus(tileToward(at, 1, 0), true);
    case XKB_KEY_Up:
      return moveFocus(tileToward(at, 0, -1), true);
    case XKB_KEY_Down:
      if (const int below = tileToward(at, 0, 1); below >= 0) {
        return moveFocus(below, true);
      }
      if (m_brightnessPill->visible()) {
        return focusSlider(m_brightnessSlider);
      }
      return m_volumePill->visible() ? focusSlider(m_volumeSlider) : true;
    case XKB_KEY_space:
      if (m_extras[index].shortcut != nullptr) {
        m_extras[index].shortcut->onClick();
      }
      return true;
    case XKB_KEY_Menu:
      if (!m_extras[index].detail.empty()) {
        m_openTab(m_extras[index].detail);
      }
      return true;
    default:
      break;
    }
  }
  if (sym == XKB_KEY_slash && (modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == 0) {
    m_openTab("detail-search");
    return true;
  }
  if (sym == XKB_KEY_Page_Down) {
    goToPage(m_page + 1);
    return true;
  }
  if (sym == XKB_KEY_Page_Up && m_page > 0) {
    goToPage(m_page - 1);
    return true;
  }
  return false;
}


void QuickPage::doUpdate(Renderer& renderer) {
  // A drag swallows only the click of its own release; keyboard activation later must work.
  if (m_swallowClick && !m_tileDragging && m_tileSource == nullptr) {
    m_swallowClick = false;
  }
  if (m_focusFirstTile && !m_editing && !m_extras.empty()) {
    m_focusFirstTile = false;
    moveFocus(0);
  }
  if (m_editRebuildPending) {
    m_editRebuildPending = false;
    rebuildEdit();
  }
  syncLiftShadow();
  if (m_editing) {
    syncExtras();
    return;
  }
  syncExtras();
  syncSliders();
  syncFooter();
  syncPager();
  syncMedia(renderer);
  syncPhone();
}

