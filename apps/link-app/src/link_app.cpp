#include "link_app.h"

#include "app/app_window.h"
#include "wayland/drag_and_drop.h"
#include "wayland/wayland_connection.h"
#include "core/text_clipboard.h"
#include "ui/controls/input.h"
#include "ui/controls/image.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/selection_list.h"
#include "ui/controls/status_row.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <chrono>
#include <cmath>
#include <linux/input-event-codes.h>
#include <ctime>
#include <filesystem>
#include <format>
#include <glib.h>
#include <poll.h>
#include <unistd.h>
#include <qrencode.h>

namespace linkapp {

  namespace {

    struct PageInfo {
      Page page;
      const char* name;
      const char* glyph;
    };

    constexpr std::array kPages{
        PageInfo{Page::Waiting, "Waiting", "bell"},        PageInfo{Page::Messages, "Messages", "message"},
        PageInfo{Page::Calls, "Calls", "phone"},           PageInfo{Page::Photos, "Photos", "photo"},
        PageInfo{Page::Files, "Files", "folder"},          PageInfo{Page::Activity, "Activity", "arrows-down-up"},
        PageInfo{Page::Mirror, "Mirror", "device-mobile"}, PageInfo{Page::Settings, "Settings", "settings"},
    };

    std::int64_t nowMs() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string clock(std::int64_t ms) {
      const std::time_t t = static_cast<std::time_t>(ms / 1000);
      std::tm tm{};
      localtime_r(&t, &tm);
      return std::format("{:02}:{:02}", tm.tm_hour, tm.tm_min);
    }

    // A list row's time: the clock today, else "Yesterday", a short weekday, or the date.
    std::string when(std::int64_t ms) {
      const std::string day = dayLabel(ms, nowMs());
      if (day == "Today") return clock(ms);
      const bool weekday = day != "Yesterday" && std::ranges::none_of(day, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
      return weekday ? day.substr(0, 3) : day;
    }

    std::string initials(const std::string& name) {
      std::string out;
      bool start = true;
      for (const char c : name) {
        if (c == ' ') {
          start = true;
        } else if (start && out.size() < 2 && std::isalpha(static_cast<unsigned char>(c)) != 0) {
          out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          start = false;
        } else {
          start = false;
        }
      }
      return out;
    }

    std::unique_ptr<Label> text(std::string value, const Tokens::Type& type, ColorRole role, float s, int lines = 1) {
      return ui::label({.text = std::move(value), .fontSize = type.size * s,
                        .fontWeight = static_cast<FontWeight>(type.weight), .color = colorSpecFromRole(role),
                        .maxLines = lines, .ellipsize = TextEllipsize::End});
    }

    // Rulebook §9.13: symbol 48 label_3, title_3, paragraph label_2, at most one button.
    std::unique_ptr<Node> emptyState(const std::string& glyph, const std::string& title, const std::string& body, float s,
                                     std::unique_ptr<Node> action = nullptr) {
      auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP8 * s,
                             .flexGrow = 1.0F});
      box->addChild(ui::glyph({.glyph = glyph, .glyphSize = Tokens::componentEmptySymbol * s,
                               .color = colorSpecFromRole(ColorRole::Label3)}));
      box->addChild(text(title, Tokens::typeTitle3, ColorRole::Label1, s));
      auto paragraph = text(body, Tokens::typeParagraph, ColorRole::Label2, s, 3);
      paragraph->setMaxWidth(Tokens::componentEmptyMaxWidth * s);
      paragraph->setTextAlign(TextAlign::Center);
      box->addChild(std::move(paragraph));
      if (action) box->addChild(std::move(action));
      return box;
    }

    // Avatar (handoff.yaml): a 32 circle in fill_2 around initials or a symbol.
    std::unique_ptr<Flex> avatar(std::unique_ptr<Node> inner, float s) {
      const float size = Tokens::sizeControlM * s;
      auto circle = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .fill = colorSpecFromRole(ColorRole::Fill2),
                             .radius = size / 2.0F, .width = size, .height = size});
      circle->addChild(std::move(inner));
      return circle;
    }

    std::unique_ptr<Flex> initialsAvatar(const std::string& name, float s) {
      const std::string letters = initials(name);
      auto label = text(letters.empty() ? "#" : letters, Tokens::typeCaption, ColorRole::Label2, s);
      label->setFontWeight(FontWeight::SemiBold);
      return avatar(std::move(label), s);
    }

    std::unique_ptr<Flex> glyphAvatar(const char* glyph, float s, ColorRole ink = ColorRole::Label2) {
      return avatar(ui::glyph({.glyph = std::string(glyph), .glyphSize = Tokens::sizeSymbol * s,
                               .color = colorSpecFromRole(ink)}),
                    s);
    }

    // ReplyCapsule (handoff.yaml): a fill_1 capsule with the field and a round Primary send button.
    std::unique_ptr<Flex> replyCapsule(const std::string& value, const std::string& placeholder, float height, float s,
                                       std::function<void(const std::string&)> change, std::function<void()> send) {
      const float key = height - 2.0F * Tokens::spaceP4 * s;
      auto capsule = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .fill = colorSpecFromRole(ColorRole::Fill1),
                              .radius = height / 2.0F, .height = height, .flexGrow = 1.0F});
      capsule->setPadding(0.0F, Tokens::spaceP4 * s, 0.0F, Tokens::spaceP12 * s);
      capsule->addChild(ui::input({.value = value, .placeholder = placeholder, .fontSize = Tokens::typeBody.size * s,
                                   .frameVisible = false, .flexGrow = 1.0F, .onChange = std::move(change),
                                   .onSubmit = [send](const std::string&) { send(); }}));
      capsule->addChild(ui::button({.glyph = std::string("send"), .glyphSize = Tokens::sizeSymbolInline * s,
                                    .variant = ButtonVariant::Primary, .tooltip = std::string("Send"), .radius = key / 2.0F,
                                    .width = key, .height = key, .onClick = std::move(send)}));
      return capsule;
    }

    constexpr const char* kLockedText = "Unlock your phone to see the text";

  } // namespace

  std::size_t FeedAdapter::itemCount() const { return m_client.feed().items().size(); }

  std::uint64_t FeedAdapter::itemKey(std::size_t index) const {
    return std::hash<std::string>{}(m_client.feed().items()[index].key);
  }

  float FeedAdapter::measureItem(Renderer& renderer, std::size_t index, float width) {
    return build(nullptr, index, width)->measure(renderer, LayoutConstraints::available(width, 1.0e6F)).height;
  }

  std::unique_ptr<Node> FeedAdapter::createItem() { return ui::column({.align = FlexAlign::Stretch}); }

  void FeedAdapter::bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool /*hovered*/) {
    auto& holder = static_cast<Flex&>(item);
    while (!holder.children().empty()) holder.removeChild(holder.children().back().get());
    holder.addChild(build(&renderer, index, width));
  }

  // FeedItem (handoff.yaml): avatar 32 | who headline + time footnote; body paragraph, 2 lines; actions row g 8.
  std::unique_ptr<Node> FeedAdapter::build(Renderer* renderer, std::size_t index, float width) {
    const float s = m_scale;
    const Feed& feed = m_client.feed();
    const FeedItem& item = feed.items()[index];
    const bool locked = m_client.presence().kind == Presence::Kind::Locked;
    auto block = ui::column({.align = FlexAlign::Stretch});
    const std::size_t earlier = feed.earlierStart(nowMs());
    if (index == 0 || index == earlier) {
      block->addChild(ui::row({.paddingV = Tokens::spaceP8 * s, .paddingH = (Tokens::spaceP16 + Tokens::sizeControlM + Tokens::spaceP12) * s},
                              text(index < earlier ? "Now" : "Earlier today", Tokens::typeFootnote, ColorRole::Label2, s)));
    } else {
      block->addChild(ui::separator({}));
    }

    auto row = ui::row({.align = FlexAlign::Start, .gap = Tokens::spaceP12 * s, .paddingV = Tokens::spaceP12 * s,
                        .paddingH = Tokens::spaceP16 * s});
    switch (item.kind) {
    case FeedItem::Kind::Text: row->addChild(initialsAvatar(m_client.messages().nameFor(item.who), s)); break;
    case FeedItem::Kind::Offer: row->addChild(glyphAvatar("file", s)); break;
    case FeedItem::Kind::MissedCall: row->addChild(glyphAvatar("phone-x", s)); break;
    case FeedItem::Kind::Notification: row->addChild(glyphAvatar("bell", s)); break;
    case FeedItem::Kind::Photos: row->addChild(glyphAvatar("photo", s)); break;
    }

    auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * s, .flexGrow = 1.0F});
    const bool offer = item.kind == FeedItem::Kind::Offer;
    const std::string who = offer ? (item.files.size() == 1 ? item.files.front().first
                                                            : std::format("{} files", item.files.size()))
                                  : item.kind == FeedItem::Kind::Text ? m_client.messages().nameFor(item.who)
                                                                      : item.who;
    auto head = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    auto name = text(who, Tokens::typeHeadline, ColorRole::Label1, s);
    name->setFlexGrow(1.0F);
    head->addChild(std::move(name));
    const bool photos = item.kind == FeedItem::Kind::Photos;
    head->addChild(text(offer ? byteSize(item.total) : photos ? std::string("Today") : clock(item.atMs), Tokens::typeFootnote,
                        ColorRole::Label2, s));
    body->addChild(std::move(head));
    const std::string line = offer    ? std::string("From your phone")
                             : photos ? std::string("Drag one to the desktop, or open Photos")
                             : locked ? std::string(kLockedText)
                                      : item.body;
    body->addChild(text(line, Tokens::typeParagraph, (locked && !offer) || photos ? ColorRole::Label2 : ColorRole::Label1, s, 2));
    if (photos) {
      // Mockup .strip: 72 thumbnails at gap 8, as many as fit the body width.
      const float side = Tokens::componentLinkStripThumb * s;
      const float gap = Tokens::spaceP8 * s;
      const float bodyWidth = width - (2.0F * Tokens::spaceP16 + Tokens::sizeControlM + Tokens::spaceP12) * s;
      const auto fit = static_cast<std::size_t>(std::max(1.0F, std::floor((bodyWidth + gap) / (side + gap))));
      const float radius = Style::scaledRadius(Tokens::radiusR12, s);
      auto strip = ui::row({.gap = gap, .paddingV = Tokens::spaceP4 * s});
      for (const std::string& path : item.photos | std::views::take(fit)) {
        InputArea* area = nullptr;
        auto pick = ui::inputArea({.out = &area, .cursorShape = CursorShape::Pointer, .width = side, .height = side,
                                   .onPress = [this](const InputArea::PointerData& data) {
                                     m_press = Press{.x = data.sceneX, .y = data.sceneY, .serial = data.serial};
                                   },
                                   .onClick = [this](const InputArea::PointerData&) { m_hooks.openPhotos(); }});
        area->setOnMotion([this, area, path, s](const InputArea::PointerData& data) {
          if (!area->pressed() || !m_press) return;
          if (std::hypot(data.sceneX - m_press->x, data.sceneY - m_press->y) < Style::dragStartThreshold * s) return;
          m_hooks.dragPhoto(path, std::exchange(m_press, std::nullopt)->serial);
        });
        pick->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Fill1), .radius = radius, .width = side, .height = side}));
        Image* image = nullptr;
        pick->addChild(ui::image({.out = &image, .fit = ImageFit::Cover, .radius = radius, .width = side, .height = side}));
        const std::vector<std::uint8_t>* png = renderer != nullptr ? m_client.thumbnail(path) : nullptr;
        if (png != nullptr) (void)image->setSourceBytes(*renderer, png->data(), png->size());
        pick->addChild(ui::box({.fill = clearColorSpec(), .border = colorSpecFromRole(ColorRole::Stroke),
                                .borderWidth = Tokens::componentSplitStroke * s, .radius = radius, .width = side, .height = side}));
        strip->addChild(std::move(pick));
      }
      body->addChild(std::move(strip));
    }

    auto actions = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingV = Tokens::spaceP4 * s});
    const FeedItem copy = item;
    const auto canReply = item.kind == FeedItem::Kind::Text || std::ranges::any_of(item.actions, &FeedAction::reply);
    if (offer && item.transfer == Transfer::Offered) {
      actions->addChild(ui::button({.text = std::string("Decline"), .variant = ButtonVariant::Tonal,
                                    .onClick = [this, copy] { m_client.decline(copy); }}));
      actions->addChild(ui::button({.text = std::string("Save to Downloads"), .variant = ButtonVariant::Primary,
                                    .onClick = [this, copy] { m_client.accept(copy); }}));
    } else if (offer) {
      const float progress = item.total > 0 ? static_cast<float>(item.bytes) / static_cast<float>(item.total) : 0.0F;
      actions->addChild(ui::progressBar({.progress = item.transfer == Transfer::Done ? 1.0F : progress,
                                         .height = Tokens::spaceP4 * s, .flexGrow = 1.0F}));
    } else if (canReply && !locked) {
      const std::string key = item.key;
      auto capsule = replyCapsule(m_drafts[key], "Reply", Tokens::sizeControlM * s, s,
                                  [this, key](const std::string& value) { m_drafts[key] = value; },
                                  [this, copy, key] {
                                    const std::string value = m_drafts[key];
                                    if (value.empty()) return;
                                    m_drafts.erase(key);
                                    m_client.reply(copy, value);
                                  });
      actions->addChild(std::move(capsule));
    } else {
      for (const FeedAction& action : item.actions) {
        if (action.reply) continue;
        const bool call = item.kind == FeedItem::Kind::MissedCall;
        actions->addChild(ui::button({.text = action.label, .glyph = call ? std::optional<std::string>("phone") : std::nullopt,
                                      .glyphSize = Tokens::sizeSymbolInline * s, .variant = ButtonVariant::Tonal,
                                      .onClick = [this, copy, id = action.id] { m_client.action(copy, id); }}));
        if (call && m_hooks.textTo) {
          actions->addChild(ui::button({.text = std::string("Text"), .glyph = std::string("message"),
                                        .glyphSize = Tokens::sizeSymbolInline * s, .variant = ButtonVariant::Ghost,
                                        .onClick = [this, number = action.id] { m_hooks.textTo(number); }}));
        }
      }
    }
    if (!actions->children().empty()) body->addChild(std::move(actions));
    row->addChild(std::move(body));
    block->addChild(std::move(row));
    return block;
  }

  std::size_t ThreadAdapter::itemCount() const { return m_client.messages().threads().size(); }

  std::uint64_t ThreadAdapter::itemKey(std::size_t index) const { return m_client.messages().threads()[index].id; }

  std::uint64_t ThreadAdapter::itemRevision(std::size_t index) const {
    const Thread& t = m_client.messages().threads()[index];
    return std::hash<std::string>{}(std::format("{}|{}|{}|{}|{}|{}", t.name, t.last, t.atMs, t.unread, m_selected == t.id,
                                                static_cast<int>(m_client.presence().kind)));
  }

  float ThreadAdapter::measureItem(Renderer& /*renderer*/, std::size_t /*index*/, float /*width*/) {
    return Tokens::componentLinkThreadRow * m_scale;
  }

  std::unique_ptr<Node> ThreadAdapter::createItem() { return ui::column({.align = FlexAlign::Stretch}); }

  // Thread row (handoff.yaml): avatar | name + time; last line + unread badge; selected = the selection tint.
  void ThreadAdapter::bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float /*width*/, bool hovered) {
    const float s = m_scale;
    const Thread& t = m_client.messages().threads()[index];
    const bool selected = m_selected == t.id;
    const bool locked = m_client.presence().kind == Presence::Kind::Locked;
    auto& holder = static_cast<Flex&>(item);
    while (!holder.children().empty()) holder.removeChild(holder.children().back().get());
    const ColorSpec fill = selected  ? colorSpecFromRole(ColorRole::Accent, SelectionList::selectionTintAlpha())
                           : hovered ? colorSpecFromRole(ColorRole::Fill1)
                                     : clearColorSpec();
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingV = Tokens::spaceP8 * s,
                        .paddingH = Tokens::spaceP12 * s, .fill = fill, .radius = Style::scaledRadius(Tokens::radiusR12, s),
                        .height = Tokens::componentLinkThreadRow * s});
    row->addChild(initialsAvatar(t.name, s));
    auto body = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
    const ColorRole quiet = selected ? ColorRole::Label1 : ColorRole::Label2;
    auto head = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    auto name = text(t.name, Tokens::typeBody, ColorRole::Label1, s);
    if (t.unread > 0) name->setFontWeight(FontWeight::SemiBold);
    name->setFlexGrow(1.0F);
    head->addChild(std::move(name));
    head->addChild(text(when(t.atMs), Tokens::typeFootnote, quiet, s));
    body->addChild(std::move(head));
    auto line = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    auto last = text(locked && t.unread > 0 ? std::string(kLockedText) : t.last, Tokens::typeFootnote, quiet, s);
    last->setFlexGrow(1.0F);
    line->addChild(std::move(last));
    if (t.unread > 0) {
      const float h = Tokens::componentBadgeHeight * s;
      auto badge = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .paddingH = Tokens::componentBadgePaddingX * s,
                            .fill = colorSpecFromRole(ColorRole::Fill3), .radius = h / 2.0F, .minWidth = h, .height = h});
      auto count = text(t.unread > 99 ? std::string("99+") : std::to_string(t.unread), Tokens::typeCaption, ColorRole::Label1, s);
      count->setFontWeight(FontWeight::SemiBold);
      badge->addChild(std::move(count));
      line->addChild(std::move(badge));
    }
    body->addChild(std::move(line));
    row->addChild(std::move(body));
    holder.addChild(std::move(row));
  }

  void ThreadAdapter::onActivate(std::size_t index) { m_open(m_client.messages().threads()[index].id); }

  std::uint64_t ConversationAdapter::itemRevision(std::size_t index) const {
    const ConversationRow& row = m_rows[index];
    return (static_cast<std::uint64_t>(row.kind) << 2U) | (row.runEnd ? 1U : 0U) |
           (m_client.presence().kind == Presence::Kind::Locked ? 2U : 0U);
  }

  bool ConversationAdapter::itemInteractive(std::size_t index) const {
    return m_rows[index].kind == ConversationRow::Kind::Failed;
  }

  float ConversationAdapter::measureItem(Renderer& renderer, std::size_t index, float width) {
    return build(index, width)->measure(renderer, LayoutConstraints::available(width, 1.0e6F)).height;
  }

  std::unique_ptr<Node> ConversationAdapter::createItem() { return ui::column({.align = FlexAlign::Stretch}); }

  void ConversationAdapter::bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float width, bool /*hovered*/) {
    auto& holder = static_cast<Flex&>(item);
    while (!holder.children().empty()) holder.removeChild(holder.children().back().get());
    holder.addChild(build(index, width));
  }

  void ConversationAdapter::onActivate(std::size_t index) {
    if (m_rows[index].kind == ConversationRow::Kind::Failed) m_client.retryText(m_rows[index].serial);
  }

  // Bubbles (handoff.yaml): in fill_1 left, out fill_2 right, r 16, paragraph, at most 72 % wide; the run's time
  // below its last bubble; day lines as centred footnotes.
  std::unique_ptr<Node> ConversationAdapter::build(std::size_t index, float width) const {
    using K = ConversationRow::Kind;
    const float s = m_scale;
    const ConversationRow& row = m_rows[index];
    const float inset = Tokens::spaceP24 * s;
    if (row.kind == K::Day) {
      return ui::row({.justify = FlexJustify::Center, .paddingV = Tokens::spaceP8 * s},
                     text(row.text, Tokens::typeFootnote, ColorRole::Label2, s));
    }
    const bool in = row.kind == K::In;
    const bool hidden = in && m_client.presence().kind == Presence::Kind::Locked;
    auto block = ui::column({.align = in ? FlexAlign::Start : FlexAlign::End, .gap = Tokens::spaceP4 * s, .paddingH = inset});
    const float padH = Tokens::spaceP12 * s;
    auto bubble = ui::column({.paddingV = Tokens::spaceP8 * s, .paddingH = padH,
                              .fill = colorSpecFromRole(in ? ColorRole::Fill1 : ColorRole::Fill2),
                              .radius = Style::scaledRadius(Tokens::componentLinkBubbleRadius, s)});
    auto body = ui::label({.text = hidden ? std::string(kLockedText) : row.text, .fontSize = Tokens::typeParagraph.size * s,
                           .fontWeight = static_cast<FontWeight>(Tokens::typeParagraph.weight),
                           .color = colorSpecFromRole(hidden ? ColorRole::Label2 : ColorRole::Label1),
                           .maxWidth = std::max(0.0F, (width - 2.0F * inset) * Tokens::componentLinkBubbleMax - 2.0F * padH)});
    bubble->addChild(std::move(body));
    block->addChild(std::move(bubble));
    if (row.runEnd || row.kind == K::Failed) {
      const std::string meta = row.kind == K::Sending  ? std::string("Sending…")
                               : row.kind == K::Failed ? std::string("Not sent · Click to try again")
                                                       : clock(row.atMs);
      block->addChild(ui::row({.paddingV = Tokens::spaceP2 * s, .paddingH = padH},
                              text(meta, Tokens::typeFootnote, row.kind == K::Failed ? ColorRole::DangerText : ColorRole::Label2, s)));
    }
    return block;
  }

  std::uint64_t CallAdapter::itemKey(std::size_t index) const {
    const CallEntry& c = m_client.calls()[index];
    return std::hash<std::string>{}(std::format("{}|{}", c.number, c.atMs));
  }

  float CallAdapter::measureItem(Renderer& renderer, std::size_t index, float width) {
    return build(index)->measure(renderer, LayoutConstraints::available(width, 1.0e6F)).height;
  }

  std::unique_ptr<Node> CallAdapter::createItem() { return ui::column({.align = FlexAlign::Stretch}); }

  void CallAdapter::bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float /*width*/, bool /*hovered*/) {
    auto& holder = static_cast<Flex&>(item);
    while (!holder.children().empty()) holder.removeChild(holder.children().back().get());
    holder.addChild(build(index));
  }

  // Call row (handoff.yaml): direction symbol | name + kind · duration | time | Call; missed in danger_text.
  std::unique_ptr<Node> CallAdapter::build(std::size_t index) const {
    using K = CallEntry::Kind;
    const float s = m_scale;
    const auto& calls = m_client.calls();
    const CallEntry& c = calls[index];
    const std::int64_t now = nowMs();
    const auto today = [&](std::size_t i) { return dayLabel(calls[i].atMs, now) == "Today"; };
    const float indent = Tokens::sizeControlM * s + Tokens::spaceP12 * s;
    auto block = ui::column({.align = FlexAlign::Stretch});
    if (index == 0 || today(index - 1) != today(index)) {
      auto head = text(today(index) ? "Today" : "Earlier", Tokens::typeFootnote, ColorRole::Label2, s);
      head->setFontWeight(FontWeight::SemiBold);
      auto line = ui::row({.paddingV = Tokens::spaceP8 * s}, std::move(head));
      line->setPadding(Tokens::spaceP16 * s, 0.0F, Tokens::spaceP8 * s, indent);
      block->addChild(std::move(line));
    } else {
      auto rule = ui::column({.align = FlexAlign::Stretch});
      rule->setPadding(0.0F, 0.0F, 0.0F, indent);
      rule->addChild(ui::separator({}));
      block->addChild(std::move(rule));
    }
    const bool missed = c.kind == K::Missed;
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingV = Tokens::spaceP8 * s,
                        .minHeight = Tokens::componentLinkThreadRow * s});
    row->addChild(missed ? glyphAvatar("phone-x", s, ColorRole::DangerText)
                         : glyphAvatar(c.kind == K::Outgoing ? "arrow-up" : "arrow-down", s));
    auto body = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
    auto name = text(c.name.empty() ? m_client.messages().nameFor(c.number) : c.name, Tokens::typeBody,
                     missed ? ColorRole::DangerText : ColorRole::Label1, s);
    name->setFontWeight(FontWeight::Medium);
    body->addChild(std::move(name));
    const char* kind = missed ? "Missed" : c.kind == K::Outgoing ? "Outgoing" : "Incoming";
    body->addChild(text(missed || c.seconds == 0 ? std::string(kind) : std::format("{} · {}", kind, duration(c.seconds)),
                        Tokens::typeFootnote, ColorRole::Label2, s));
    row->addChild(std::move(body));
    row->addChild(text(today(index) ? clock(c.atMs) : when(c.atMs) + " " + clock(c.atMs), Tokens::typeFootnote,
                       ColorRole::Label2, s));
    const std::string device = m_client.presence().phone.id;
    const std::string number = c.number;
    row->addChild(ui::button({.glyph = std::string("phone"), .glyphSize = Tokens::sizeSymbol * s, .variant = ButtonVariant::Ghost,
                              .tooltip = std::string("Call"),
                              .onClick = [this, device, number] { m_client.dial(device, number); }}));
    block->addChild(std::move(row));
    return block;
  }

  std::uint64_t TransferAdapter::itemKey(std::size_t index) const {
    return std::hash<std::string>{}(m_client.activity().items()[index].id);
  }

  float TransferAdapter::measureItem(Renderer& renderer, std::size_t index, float width) {
    return build(index)->measure(renderer, LayoutConstraints::available(width, 1.0e6F)).height;
  }

  std::unique_ptr<Node> TransferAdapter::createItem() { return ui::column({.align = FlexAlign::Stretch}); }

  void TransferAdapter::bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float /*width*/, bool /*hovered*/) {
    auto& holder = static_cast<Flex&>(item);
    while (!holder.children().empty()) holder.removeChild(holder.children().back().get());
    holder.addChild(build(index));
  }

  // Transfer row (handoff.yaml): direction symbol | name, status line, ProgressBar | actions: offered Decline + Save,
  // running Cancel, done "Show in Folder"; a failed one says why in danger_text.
  std::unique_ptr<Node> TransferAdapter::build(std::size_t index) const {
    const float s = m_scale;
    const auto& items = m_client.activity().items();
    const TransferEntry& e = items[index];
    const std::int64_t now = nowMs();
    const float indent = Tokens::sizeControlM * s + Tokens::spaceP12 * s;
    auto block = ui::column({.align = FlexAlign::Stretch});
    const std::string day = dayLabel(e.atMs, now);
    if (index == 0 || dayLabel(items[index - 1].atMs, now) != day) {
      auto head = text(day, Tokens::typeFootnote, ColorRole::Label2, s);
      head->setFontWeight(FontWeight::SemiBold);
      auto line = ui::row({}, std::move(head));
      line->setPadding(Tokens::spaceP16 * s, 0.0F, Tokens::spaceP8 * s, indent);
      block->addChild(std::move(line));
    } else {
      auto rule = ui::column({.align = FlexAlign::Stretch});
      rule->setPadding(0.0F, 0.0F, 0.0F, indent);
      rule->addChild(ui::separator({}));
      block->addChild(std::move(rule));
    }
    const float key = Tokens::sizeControlM * s;
    auto symbol = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .fill = colorSpecFromRole(ColorRole::Fill2),
                           .radius = Style::scaledRadius(Tokens::radiusR8, s), .width = key, .height = key});
    symbol->addChild(ui::glyph({.glyph = std::string(e.incoming ? "arrow-down" : "arrow-up"), .glyphSize = Tokens::sizeSymbol * s,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingV = Tokens::spaceP8 * s,
                        .minHeight = Tokens::componentLinkThreadRow * s});
    row->addChild(std::move(symbol));

    const std::string name = e.files.size() == 1 ? e.files.front().first
                             : e.files.empty()   ? std::string("Files from this desktop")
                                                 : std::format("{} files", e.files.size());
    std::string status;
    switch (e.state) {
    case Transfer::Offered: status = "Offered · " + byteSize(e.total); break;
    case Transfer::Running: {
      const auto pct = e.total > 0 ? e.bytes * 100 / e.total : 0;
      status = std::format("{} %", pct);
      if (const double rate = transferRate(e, now); rate > 0.0) {
        const auto left = static_cast<std::uint64_t>(static_cast<double>(e.total - std::min(e.bytes, e.total)) / rate);
        status += std::format(" · {}/s · {}", byteSize(static_cast<std::uint64_t>(rate)),
                              left < 60 ? std::string("under a minute left") : std::format("{} min left", (left + 59) / 60));
      }
      break;
    }
    case Transfer::Done:
      if (e.incoming && !e.paths.empty()) {
        const std::string& path = e.paths.front();
        const auto slash = path.find_last_of('/');
        const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
        status = std::format("Saved to {} · {}", dir.substr(dir.find_last_of('/') + 1), clock(e.endMs));
      } else {
        status = std::format("{} · {} · {}", e.incoming ? "Saved" : "Sent", byteSize(e.total), clock(e.endMs));
      }
      break;
    case Transfer::Failed:
      status = e.status == "cancelled"   ? std::string("Cancelled")
               : e.status == "no-space"  ? std::string("Failed: not enough space")
               : e.status == "too-large" ? std::string("Failed: the file is too large")
               : e.status == "busy"      ? std::string("Failed: the phone is busy")
                                         : std::string("Failed: the transfer broke off");
      break;
    }
    auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * s, .flexGrow = 1.0F});
    auto title = text(name, Tokens::typeBody, ColorRole::Label1, s);
    title->setFontWeight(FontWeight::Medium);
    body->addChild(std::move(title));
    const bool failed = e.state == Transfer::Failed && e.status != "cancelled";
    body->addChild(text(status, Tokens::typeFootnote, failed ? ColorRole::DangerText : ColorRole::Label2, s));
    if (e.state == Transfer::Running) {
      const float progress = e.total > 0 ? static_cast<float>(e.bytes) / static_cast<float>(e.total) : 0.0F;
      body->addChild(ui::progressBar({.progress = progress, .height = Tokens::spaceP4 * s}));
    }
    row->addChild(std::move(body));

    auto actions = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    const std::string id = e.id;
    switch (e.state) {
    case Transfer::Offered:
      actions->addChild(ui::button({.text = std::string("Decline"), .variant = ButtonVariant::Tonal,
                                    .onClick = [this, id] { m_client.declineTransfer(id); }}));
      actions->addChild(ui::button({.text = std::string("Save"), .variant = ButtonVariant::Primary,
                                    .onClick = [this, id] { m_client.acceptTransfer(id); }}));
      break;
    case Transfer::Running:
      actions->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Tonal,
                                    .onClick = [this, id] { m_client.cancelTransfer(id); }}));
      break;
    case Transfer::Done:
      if (!e.paths.empty()) {
        actions->addChild(ui::button({.text = std::string("Show in Folder"), .variant = ButtonVariant::Ghost,
                                      .onClick = [this, paths = e.paths] { m_client.showInFolder(paths); }}));
      }
      break;
    case Transfer::Failed: break;
    }
    if (!actions->children().empty()) row->addChild(std::move(actions));
    block->addChild(std::move(row));
    return block;
  }

  LinkApp::LinkApp() : DeskApp({.id = kAppId, .name = "Link"}) {}

  LinkApp::~LinkApp() = default;

  AppWindow& LinkApp::createWindow() {
    AppWindow& window = addWindow({.appId = info().id, .title = "Waiting", .width = 1200, .height = 780, .minWidth = 760,
                                   .sidebarWidth = Tokens::componentSidebarWidth,
                                   .material = AppWindow::Material::Glass, .contentCard = true});
    m_window = &window;
    if (m_client == nullptr && bus() != nullptr) {
      m_client = std::make_unique<LinkClient>(*bus());
      m_feed = std::make_unique<FeedAdapter>(*m_client, m_scale,
                                             FeedAdapter::Hooks{.textTo =
                                                                    [this](const std::string& number) {
                                                                      openCompose(number);
                                                                      show(Page::Messages);
                                                                    },
                                                                .openPhotos = [this] { show(Page::Photos); },
                                                                .dragPhoto = [this](const std::string& path, std::uint32_t serial) { dragPhoto(path, serial); }});
      m_threads = std::make_unique<ThreadAdapter>(*m_client, m_scale, m_thread, [this](std::uint64_t id) { openThread(id); });
      m_conversation = std::make_unique<ConversationAdapter>(*m_client, m_scale);
      m_calls = std::make_unique<CallAdapter>(*m_client, m_scale);
      m_photos = std::make_unique<PhotoAdapter>(*m_client, m_scale);
      m_files = std::make_unique<FileAdapter>(*m_client, m_scale);
      m_client->setOnMirrorFrame([this](MirrorDecoder::Frame frame) { showMirrorFrame(std::move(frame)); });
      m_client->setOnThumbnail([this] {
        if (m_photoGrid != nullptr) m_photoGrid->notifyDataChanged();
        if (m_feedList != nullptr) m_feedList->notifyDataChanged();
      });
      m_transfers = std::make_unique<TransferAdapter>(*m_client, m_scale);
      m_client->setOnChanged([this] {
        if (m_window == nullptr) return;
        if (m_client->mirror().kind != Mirror::Kind::Showing && m_mirrorTexture.valid()) retireMirrorFrame(true);
        sync();
        syncHotspotSheet();
        updateCaption();
        m_window->requestRebuild();
      });
      // The snapshot feed is there before Link1 answers.
      sync();
      updateCaption();
    }
    window.setSidebarBuilder([this](Renderer&, float width, float) { return sidebar(width); });
    window.setContentBuilder([this](Renderer& renderer, float width, float height) { return content(renderer, width, height); });
    // The header's one Ghost action: "Clear all" on Waiting, "Clear finished" on Activity.
    const auto clear = [this](bool finished) {
      return ui::button({.text = std::string(finished ? "Clear finished" : "Clear all"), .variant = ButtonVariant::Ghost,
                         .onClick = [this, finished] { finished ? m_client->clearFinished() : m_client->clearAll(); }});
    };
    window.setHeaderAccessoryBuilder([this, clear]() -> std::unique_ptr<Node> {
      if (m_client == nullptr) return ui::node();
      if (m_page == Page::Waiting && !m_client->feed().items().empty()) {
        return ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End}, clear(false));
      }
      if (m_page == Page::Activity && m_client->activity().finishedCount() > 0) {
        return ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End}, clear(true));
      }
      return ui::node();
    });
    window.setHeaderAccessoryWidths([clear](Renderer& renderer) {
      const auto width = [&](bool finished) {
        return clear(finished)->measure(renderer, LayoutConstraints::available(1.0e6F, 1.0e6F)).width;
      };
      const float widest = std::max(width(false), width(true));
      return AppWindow::HeaderAccessoryWidths{.floor = widest, .keep = widest};
    });
    return window;
  }

  void LinkApp::action(const std::string& name, const std::vector<std::string>& parameter, const std::string& activationToken) {
    // "page" takes the page name, then optionally the id of the phone to show.
    if (name == "page" && (parameter.size() == 1 || parameter.size() == 2)) {
      const auto it = std::ranges::find_if(kPages, [&](const PageInfo& info) {
        return std::ranges::equal(std::string_view(info.name), parameter.front(),
                                  [](char a, char b) { return std::tolower(a) == std::tolower(b); });
      });
      if (it != kPages.end()) {
        activate(activationToken);
        if (parameter.size() == 2) choosePhone(parameter[1]);
        if (m_window != nullptr) show(it->page);
        return;
      }
    }
    DeskApp::action(name, parameter, activationToken);
  }

  void LinkApp::openThread(std::optional<std::uint64_t> id) {
    if (m_thread == id && !m_compose) return;
    m_compose.reset();
    m_thread = id;
    m_conversationScroll = {};
    sync();
    m_window->requestRebuild();
  }

  void LinkApp::openCompose(const std::string& to) {
    const std::string number = to.empty() ? std::string() : m_client->messages().numberFor(to);
    if (const auto thread = number.empty() ? std::nullopt : m_client->messages().threadFor(number)) {
      openThread(thread);
      return;
    }
    m_thread.reset();
    m_compose = Compose{.to = to};
    sync();
    m_window->requestRebuild();
  }

  void LinkApp::dragPhoto(const std::string& path, std::uint32_t serial) {
    DragAndDrop* dnd = loop().wayland().dragAndDrop();
    if (dnd == nullptr || m_window == nullptr || m_window->wlSurface() == nullptr) return;
    const std::filesystem::path folder = std::filesystem::path(g_get_user_cache_dir()) / "andromeda" / "link-app" / "drag";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    // Earlier drags' copies: a drop target has read them long ago.
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
      if (entry.last_write_time(error) < old) std::filesystem::remove(entry.path(), error);
    }
    // The URI list once the save answered (empty when it failed); drop targets that ask before wait in `waiting`.
    struct Drop {
      std::optional<std::string> uris;
      std::vector<std::function<void(const std::string&)>> waiting;
    };
    auto drop = std::make_shared<Drop>();
    m_client->savePhotoTo(path, folder.string(), [drop](const std::string& saved) {
      std::string uris;
      if (gchar* uri = saved.empty() ? nullptr : g_filename_to_uri(saved.c_str(), nullptr, nullptr)) {
        uris = std::string(uri) + "\r\n";
        g_free(uri);
      }
      drop->uris = uris;
      for (const auto& reply : std::exchange(drop->waiting, {})) reply(uris);
    });
    (void)dnd->startDrag(m_window->wlSurface(), serial,
                         {.mimeTypes = {"text/uri-list"},
                          .dataLater =
                              [drop](const std::string&, std::function<void(const std::string&)> reply) {
                                if (drop->uris) {
                                  reply(*drop->uris);
                                } else {
                                  drop->waiting.push_back(std::move(reply));
                                }
                              },
                          .actions = DragAndDrop::Copy | DragAndDrop::Move});
  }

  void LinkApp::chooseAndSend(const std::string& deviceId) {
    if (m_window == nullptr || bus() == nullptr) return;
    if (m_chooser == nullptr) m_chooser = std::make_unique<desk::FileChooser>(bus());
    m_window->exportHandle([this, deviceId](const std::string& parent) {
      m_chooser->openFiles(parent, "Send Files", [this, deviceId](std::vector<std::filesystem::path> paths) {
        if (!paths.empty()) m_client->sendFiles(deviceId, paths);
      });
    });
  }

  void LinkApp::choosePhone(const std::string& deviceId) {
    if (!m_client || m_client->presence().phone.id == deviceId) return;
    if (m_page == Page::Mirror) m_client->stopMirror();
    m_client->select(deviceId);
    if (m_window == nullptr) return;
    if (m_page == Page::Photos) m_client->loadPhotos();
    if (m_page == Page::Files) m_client->openFolder("/");
    if (m_page == Page::Mirror) m_client->startMirror();
    sync();
    updateCaption();
    m_window->requestRebuild();
  }

  void LinkApp::show(Page page) {
    if (m_page == page) return;
    if (m_client && m_page == Page::Mirror) m_client->stopMirror();
    m_page = page;
    m_confirmForget = false;
    if (page == Page::Photos && m_client) m_client->loadPhotos();
    if (page == Page::Files && m_client) m_client->openFolder(m_client->files().path());
    if (page == Page::Mirror && m_client) m_client->startMirror();
    sync();
    updateCaption();
    m_window->requestRebuild();
  }

  void LinkApp::syncHotspotSheet() {
    const auto& ask = m_client->hotspotAsk();
    if (!ask) {
      if (!std::exchange(m_hotspotSheet, {}).empty()) m_window->dismissSheet();
      return;
    }
    // A newer ask replaces the shown one, so the text always names the device the answer goes to.
    std::string shown = ask->deviceId + '\n' + ask->ssid;
    if (shown == m_hotspotSheet) return;
    m_hotspotSheet = std::move(shown);
    const Presence presence = m_client->presence();
    const std::string phone = presence.phone.id == ask->deviceId ? presence.phone.name : std::string("your phone");
    const std::string ssid = ask->ssid;
    // Rulebook §9.10 alert: title-3, body 13 / 20 label_2, 24 above 3 buttons stacked full width.
    m_window->presentSheet(
        [this, phone, ssid](float s) -> std::unique_ptr<Node> {
          auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .padding = Tokens::spaceP24 * s,
                                 .width = Tokens::componentDialogAlertWidth * s});
          box->addChild(text(std::format("Join {}’s hotspot?", phone), Tokens::typeTitle3, ColorRole::Label1, s, 2));
          box->addChild(text(std::format("Wi-Fi Direct is not available, so this computer joins “{}” for files and "
                                         "mirroring, and leaves it when idle.", ssid),
                             Tokens::typeParagraph, ColorRole::Label2, s, 4));
          auto buttons = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
          buttons->setPadding(Tokens::spaceP16 * s, 0.0F, 0.0F, 0.0F);
          buttons->addChild(ui::button({.text = std::string("Use Hotspot"), .variant = ButtonVariant::Primary,
                                        .onClick = [this] { m_client->answerHotspot(true); }}));
          buttons->addChild(ui::button({.text = std::string("Always Allow"), .variant = ButtonVariant::Tonal,
                                        .onClick = [this] { m_client->answerHotspot(true, true); }}));
          buttons->addChild(ui::button({.text = std::string("Not Now"), .variant = ButtonVariant::Tonal,
                                        .onClick = [this] { m_client->answerHotspot(false); }}));
          box->addChild(std::move(buttons));
          return box;
        },
        [this] { m_client->answerHotspot(false); });
  }

  void LinkApp::sync() {
    if (m_client == nullptr) return;
    if (m_page == Page::Photos) m_client->markPhotosSeen();
    const Presence::Kind kind = m_client->presence().kind;
    const bool pairing = kind == Presence::Kind::NotPaired;
    if (pairing && m_client->pair().kind == Pair::Kind::Idle) m_client->startPairing();
    m_window->setTitle(pairing ? "Pair a Phone" : kPages[static_cast<std::size_t>(m_page)].name);
    const bool live = kind == Presence::Kind::Connected || kind == Presence::Kind::Locked;
    if (m_page == Page::Files && live && m_client->files().load() == Files::Load::Idle) m_client->openFolder("/");
    const bool cards = (m_page == Page::Messages || m_page == Page::Files || m_page == Page::Mirror) && live;
    const Messages& messages = m_client->messages();
    if (m_compose && !m_compose->sent.empty()) {
      if (const auto thread = messages.threadFor(m_compose->sent)) {
        m_compose.reset();
        m_thread = thread;
        m_conversationScroll = {};
      }
    }
    if (cards && !m_compose && (!m_thread || messages.thread(*m_thread) == nullptr)) {
      m_thread = messages.threads().empty() ? std::nullopt : std::optional(messages.threads().front().id);
      m_conversationScroll = {};
    }
    m_client->viewThread(cards ? m_thread : std::nullopt);
    if (cards != m_window->contentCards()) m_window->setContentCards(cards);
  }

  void LinkApp::updateCaption() {
    const std::size_t n = m_client ? m_client->feed().items().size() : 0;
    std::string state;
    if (m_page == Page::Waiting && n > 0) state = n == 1 ? "1 item" : std::format("{} items", n);
    if (m_page == Page::Photos && m_client && !m_client->photos().items().empty()) {
      state = std::to_string(m_client->photos().items().size());
    }
    m_window->setCaption({.state = state});
  }

  std::unique_ptr<Node> LinkApp::sidebar(float /*width*/) {
    const float s = m_scale = m_window->uiScale();
    auto side = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .padding = Tokens::componentSidebarPadding * s,
                            .flexGrow = 1.0F});
    side->addChild(ui::searchCapsule({.placeholder = "Search", .height = Tokens::sizeControlL, .scale = s}));
    auto list = std::make_unique<SelectionList>(s);
    list->setGap(Tokens::spaceP4 * s);
    Node* selected = nullptr;
    const std::vector<FeedItem> none;
    const auto& items = m_client ? m_client->feed().items() : none;
    // "<n> <noun> · <newest who>" and the newest time for one kind of feed item.
    const auto summary = [&](FeedItem::Kind kind, const char* noun) -> std::pair<std::string, std::string> {
      const auto count = std::ranges::count(items, kind, &FeedItem::kind);
      if (count == 0) return {};
      const auto newest = std::ranges::find(items, kind, &FeedItem::kind);
      const std::string who = kind == FeedItem::Kind::Offer ? std::string() : " · " + newest->who;
      return {std::format("{} {}{}", count, noun, who), clock(newest->atMs)};
    };
    for (const PageInfo& info : kPages) {
      if (info.page == Page::Settings) continue;
      auto row = std::make_unique<StatusRow>(s);
      std::pair<std::string, std::string> line;
      switch (info.page) {
      case Page::Waiting:
        if (!items.empty()) line.first = items.size() == 1 ? "1 item" : std::format("{} items", items.size());
        else if (m_client && m_client->presence().kind == Presence::Kind::Away) line.first = "Phone not connected";
        break;
      case Page::Messages: {
        static const Messages kNone;
        const Messages& messages = m_client ? m_client->messages() : kNone;
        const auto newest = std::ranges::find_if(messages.threads(), [](const Thread& t) { return t.unread > 0; });
        if (newest != messages.threads().end()) {
          line = {std::format("{} unread · {}", messages.unread(), newest->name), clock(newest->atMs)};
        } else if (!messages.threads().empty()) {
          line.first = "All read";
        }
        break;
      }
      case Page::Calls: line = summary(FeedItem::Kind::MissedCall, "missed"); break;
      case Page::Activity:
        if (m_client) {
          const Activity& activity = m_client->activity();
          const std::size_t offered = activity.offeredCount();
          const auto running = std::ranges::count(activity.items(), Transfer::Running, &TransferEntry::state);
          if (offered > 0) {
            line.first = std::format("{} offered", offered);
          } else if (running > 0) {
            line.first = running == 1 ? std::string("1 transfer running") : std::format("{} transfers running", running);
          }
        }
        break;
      case Page::Files: line.first = "Phone storage"; break;
      case Page::Mirror: line.first = "Show the phone screen"; break;
      case Page::Photos: {
        if (!m_client) break;
        const auto days = m_client->photos().days(nowMs());
        if (days.empty() || days.front().second != "Today") break;
        const std::size_t today = days.size() > 1 ? days[1].first : m_client->photos().items().size();
        line.first = today == 1 ? std::string("1 new photo today") : std::format("{} new photos today", today);
        break;
      }
      case Page::Settings: break;
      }
      const bool hot = !line.second.empty() || (info.page == Page::Waiting && !items.empty());
      row->setData({.symbol = info.glyph, .name = info.name, .status = line.first, .time = line.second, .unseen = hot,
                    .selected = info.page == m_page});
      row->setOnClick([this, page = info.page] { show(page); });
      if (info.page == m_page) selected = row.get();
      list->addChild(std::move(row));
    }
    auto settings = std::make_unique<StatusRow>(s);
    settings->setData({.symbol = "settings", .name = "Settings", .selected = m_page == Page::Settings});
    settings->setOnClick([this] { show(Page::Settings); });
    if (m_page == Page::Settings) selected = settings.get();
    list->addChild(std::move(settings));
    if (selected != nullptr) list->setSelected(selected, false);
    // Pair (flow.html#page=pair): only search and the PhoneCard until a phone is paired.
    if (!m_client || m_client->presence().kind != Presence::Kind::NotPaired) side->addChild(std::move(list));
    side->addChild(ui::spacer());

    // PhoneCard (handoff.yaml): phone symbol, name 600, dot + transport + battery.
    const Presence presence = m_client ? m_client->presence() : Presence{};
    auto card = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP4 * s, .padding = Tokens::spaceP12 * s,
                            .fill = colorSpecFromRole(ColorRole::GlassCard), .radius = Style::scaledRadius(Tokens::radiusR12, s)});
    auto head = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    head->addChild(ui::glyph({.glyph = std::string("device-mobile"), .glyphSize = Tokens::sizeSymbol * s,
                              .color = colorSpecFromRole(ColorRole::Label1)}));
    std::string name = presence.phone.name;
    std::string line;
    switch (presence.kind) {
    case Presence::Kind::NoDaemon:
      name = "Link is not running";
      break;
    case Presence::Kind::NotPaired:
      name = "No phone yet";
      line = "Pair one to start";
      break;
    case Presence::Kind::Away:
      line = lastSeenText(presence.phone.lastSeen, nowMs() / 1000);
      break;
    case Presence::Kind::Connected:
    case Presence::Kind::Locked: {
      line = presence.phone.transport == "bluetooth" ? "Bluetooth" : "Wi-Fi";
      if (presence.phone.battery >= 0) line += std::format(" · {} %", presence.phone.battery);
      break;
    }
    }
    auto label = text(name, Tokens::typeHeadline, ColorRole::Label1, s);
    label->setFlexGrow(1.0F);
    head->addChild(std::move(label));
    if (m_client && m_client->phones().size() > 1) {
      Button* choose = nullptr;
      head->addChild(ui::button({.out = &choose, .glyph = std::string("selector"), .glyphSize = Tokens::sizeSymbol * s,
                                 .variant = ButtonVariant::Ghost, .tooltip = std::string("Choose phone"),
                                 .radius = Tokens::sizeControlS * s / 2.0F, .width = Tokens::sizeControlS * s,
                                 .height = Tokens::sizeControlS * s}));
      choose->setOnClick([this, choose] {
        std::vector<ContextMenuControlEntry> entries;
        const std::vector<Phone>& phones = m_client->phones();
        const std::string shown = m_client->presence().phone.id;
        for (std::size_t i = 0; i < phones.size(); ++i) {
          entries.push_back({.id = static_cast<std::int32_t>(i), .label = phones[i].name, .radio = true,
                             .toggleState = phones[i].id == shown ? 1 : 0});
        }
        float x = 0.0F;
        float y = 0.0F;
        Node::absolutePosition(choose, x, y);
        m_window->openContextMenu(std::move(entries), x, y + choose->height(), [this](std::int32_t i) {
          const std::vector<Phone>& now = m_client->phones();
          if (i >= 0 && static_cast<std::size_t>(i) < now.size()) choosePhone(now[static_cast<std::size_t>(i)].id);
        });
      });
    }
    card->addChild(std::move(head));
    if (!line.empty()) {
      const bool live = presence.kind == Presence::Kind::Connected || presence.kind == Presence::Kind::Locked;
      auto status = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
      if (presence.kind != Presence::Kind::NotPaired) {
        status->addChild(ui::box({.fill = colorSpecFromRole(live ? ColorRole::Success : ColorRole::Label3),
                                  .radius = Tokens::spaceP8 * s / 2.0F, .width = Tokens::spaceP8 * s,
                                  .height = Tokens::spaceP8 * s}));
      }
      status->addChild(text(line, Tokens::typeFootnote, ColorRole::Label2, s));
      card->addChild(std::move(status));
    }
    if (presence.kind == Presence::Kind::Connected || presence.kind == Presence::Kind::Locked ||
        presence.kind == Presence::Kind::Away) {
      // RoundKey (handoff.yaml): 32 circle fill_1, on = accent; an away phone keeps the row, dimmed.
      const float key = Tokens::sizeControlM * s;
      const bool live = presence.kind != Presence::Kind::Away;
      const auto round = [&](const char* glyph, const char* tip, bool on, std::function<void()> click) {
        return ui::button({.glyph = std::string(glyph), .glyphSize = Tokens::sizeSymbol * s, .enabled = live,
                           .variant = on && live ? ButtonVariant::Primary : ButtonVariant::Tonal, .tooltip = std::string(tip),
                           .radius = key / 2.0F, .width = key, .height = key, .onClick = std::move(click)});
      };
      const std::string id = presence.phone.id;
      const bool dnd = presence.phone.dnd == 1;
      auto keys = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .paddingV = Tokens::spaceP4 * s});
      keys->addChild(round("bell", "Ring phone", false, [this, id] { m_client->ring(id); }));
      keys->addChild(round("device-mobile", "Mirror", false, [this] { show(Page::Mirror); }));
      keys->addChild(round("clipboard", "Send clipboard", false, [this, id] {
        if (TextClipboard* clipboard = Input::textClipboard(); clipboard != nullptr) {
          clipboard->requestClipboardText([this, id](std::optional<std::string> text) {
            if (text.has_value() && !text->empty()) m_client->shareText(id, *text);
          });
        }
      }));
      keys->addChild(round("circle-minus", "Do not disturb", dnd, [this, id, dnd] { m_client->setDnd(id, !dnd); }));
      keys->addChild(round("arrow-up", "Send files", false, [this, id] { chooseAndSend(id); }));
      card->addChild(std::move(keys));
    }
    side->addChild(std::move(card));
    return side;
  }

  std::unique_ptr<Node> LinkApp::pair(Renderer& renderer, float s) {
    const Pair& pair = m_client->pair();
    if (pair.kind == Pair::Kind::Failed) {
      return emptyState("device-mobile-off", "Pairing did not start", pair.error, s,
                        ui::button({.text = std::string("Try Again"), .variant = ButtonVariant::Primary,
                                    .onClick = [this] { m_client->startPairing(); }}));
    }
    auto page = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP32 * s,
                         .flexGrow = 1.0F});
    // A scanner needs dark modules on a light ground in either scheme, so the QR view keeps the light tone.
    auto view = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .padding = Tokens::spaceP16 * s,
                            .fill = ColorSpec{.fixed = colorForRole(ColorRole::View, PaletteTone::Light)},
                            .radius = Style::scaledRadius(Tokens::radiusR16, s),
                            .border = colorSpecFromRole(ColorRole::Stroke), .borderWidth = 0.5F * s,
                            .width = Tokens::componentLinkQr * s, .height = Tokens::componentLinkQr * s});
    const float drawn = (Tokens::componentLinkQr - 2.0F * Tokens::spaceP16) * s;
    Image* image = nullptr;
    view->addChild(ui::image({.out = &image, .fit = ImageFit::Contain, .width = drawn, .height = drawn}));
    const std::unique_ptr<QRcode, decltype(&QRcode_free)> qr(
        pair.kind == Pair::Kind::Showing ? QRcode_encodeString(pair.uri.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1) : nullptr,
        &QRcode_free);
    if (qr != nullptr) {
      // Whole texels per module at twice the drawn size keep the module edges sharp when scaled.
      const int texels = std::max(1, static_cast<int>(std::ceil(2.0F * drawn / static_cast<float>(qr->width))));
      const int side = qr->width * texels;
      const Color ink = colorForRole(ColorRole::Label1, PaletteTone::Light);
      const std::array<std::uint8_t, 4> dark{static_cast<std::uint8_t>(ink.r * 255.0F), static_cast<std::uint8_t>(ink.g * 255.0F),
                                             static_cast<std::uint8_t>(ink.b * 255.0F), 0xFF};
      std::vector<std::uint8_t> rgba(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4, 0);
      for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
          if ((qr->data[((y / texels) * qr->width) + (x / texels)] & 1) == 0) continue;
          std::ranges::copy(dark, rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y) * side + x) * 4));
        }
      }
      (void)image->setSourceRaw(renderer, rgba.data(), rgba.size(), side, side, side * 4, PixmapFormat::RGBA);
    }
    page->addChild(std::move(view));

    auto steps = ui::column({.align = FlexAlign::Start, .gap = Tokens::spaceP12 * s,
                             .maxWidth = Tokens::componentLinkPairText * s});
    steps->addChild(text("Scan with Andromeda Link", Tokens::typeTitle2, ColorRole::Label1, s));
    auto how = text("Open Link on your Android phone, tap Pair, and point the camera at this code. Both devices must be on "
                    "the same network.", Tokens::typeParagraph, ColorRole::Label2, s, 4);
    how->setMaxWidth(Tokens::componentLinkPairText * s);
    steps->addChild(std::move(how));
    steps->addChild(text("Or enter this code on the phone:", Tokens::typeParagraph, ColorRole::Label1, s));
    auto code = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    for (const char digit : pair.code) {
      auto tile = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center,
                              .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Style::scaledRadius(Tokens::radiusR8, s),
                              .width = Tokens::componentLinkCodeDigitW * s, .height = Tokens::componentLinkCodeDigitH * s});
      tile->addChild(text(std::string(1, digit), Tokens::typeTitle2, ColorRole::Label1, s));
      code->addChild(std::move(tile));
    }
    steps->addChild(std::move(code));
    page->addChild(std::move(steps));
    return page;
  }

  std::unique_ptr<Node> LinkApp::content(Renderer& renderer, float width, float height) {
    m_photoGrid = nullptr;
    m_feedList = nullptr;
    m_mirrorImage = nullptr;
    m_mirrorStat = nullptr;
    m_renderer = &renderer;
    const float s = m_scale = m_window->uiScale();
    if (m_client == nullptr) return emptyState("plug-connected-x", "No session bus", "Link needs a D-Bus session.", s);
    const Presence presence = m_client->presence();
    switch (presence.kind) {
    case Presence::Kind::NoDaemon:
      return emptyState("plug-connected-x", "Link is not running", "Start the Link service to reach your phone.", s);
    case Presence::Kind::NotPaired:
      return pair(renderer, s);
    case Presence::Kind::Away: {
      if (m_page == Page::Settings) return settings(presence);
      const std::string id = presence.phone.id;
      std::string seen;
      if (presence.phone.lastSeen > 0) {
        seen = lastSeenText(presence.phone.lastSeen, nowMs() / 1000);
        if (!presence.phone.transport.empty()) seen += presence.phone.transport == "bluetooth" ? " over Bluetooth" : " on Wi-Fi";
        seen += ". ";
      }
      return emptyState("device-mobile", presence.phone.name + " is not connected",
                        seen + "Open Link on the phone, or connect over Bluetooth.", s,
                        ui::button({.text = std::string("Connect"), .variant = ButtonVariant::Primary,
                                    .onClick = [this, id] { m_client->connect(id); }}));
    }
    case Presence::Kind::Connected:
    case Presence::Kind::Locked: break;
    }
    if (m_page == Page::Messages) return messages(width, height);
    if (m_page == Page::Calls) return calls();
    if (m_page == Page::Photos) return photos(renderer);
    if (m_page == Page::Files) return files(width, height);
    if (m_page == Page::Mirror) return mirror(width, height);
    if (m_page == Page::Activity) return activity();
    if (m_page == Page::Settings) return settings(presence);
    if (m_page != Page::Waiting) {
      const PageInfo& info = kPages[static_cast<std::size_t>(m_page)];
      return emptyState(info.glyph, info.name, "This page comes in a later build step.", s);
    }
    if (m_client->feed().items().empty()) {
      return emptyState("bell", "Nothing waiting", "Texts, missed calls and files from your phone show here.", s);
    }
    auto view = ui::virtualListView({.out = &m_feedList, .state = &m_feedScroll, .contentScale = s, .adapter = m_feed.get(),
                                     .flexGrow = 1.0F});
    return view;
  }

  // A card as split cards look (design-split): window card radius, stroke edge, a 40 caption that moves the window.
  std::unique_ptr<Node> LinkApp::card(std::vector<std::unique_ptr<Node>> caption, std::unique_ptr<Node> body, float width,
                                      bool windowButtons) {
    const float s = m_scale;
    const float radius = Style::scaledRadius(Tokens::componentWindowCardRadius, s);
    const float height = Tokens::componentWindowCaption * s;
    auto box = ui::column({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(panelMaterialRole(), Style::windowGlassTint()),
                           .radius = radius, .border = colorSpecFromRole(ColorRole::Stroke),
                           .borderWidth = Tokens::componentSplitStroke * s, .minWidth = width, .maxWidth = width, .width = width});
    box->setClipRadii(Radii{radius, radius, radius, radius});
    auto cap = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .width = width, .height = height});
    cap->setPadding(0.0F, Tokens::spaceP8 * s, 0.0F, Tokens::spaceP16 * s);
    for (auto& part : caption) cap->addChild(std::move(part));
    if (windowButtons) cap->addChild(m_window->buildWindowButtons());
    // A drag on the caption moves the window, as the header does.
    struct Press {
      bool down = false;
      std::uint32_t serial = 0;
      float x = 0.0F;
      float y = 0.0F;
    };
    auto press = std::make_shared<Press>();
    auto area = ui::inputArea({.width = width, .height = height});
    area->setOnPress([press](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT) *press = {.down = data.pressed, .serial = data.serial, .x = data.localX, .y = data.localY};
    });
    area->setOnMotion([this, press, s](const InputArea::PointerData& data) {
      if (press->down && std::hypot(data.localX - press->x, data.localY - press->y) > Tokens::spaceP4 * s) {
        press->down = false;
        m_window->beginMove(press->serial);
      }
    });
    area->addChild(std::move(cap));
    box->addChild(std::move(area));
    box->addChild(std::move(body));
    return box;
  }

  // Messages (handoff.yaml): thread list card 320 | conversation card with the reply capsule.
  std::unique_ptr<Node> LinkApp::messages(float width, float height) {
    const float s = m_scale;
    const Messages& messages = m_client->messages();
    auto cards = ui::row({.align = FlexAlign::Stretch, .gap = Tokens::componentWindowCardGap * s, .width = width, .height = height});
    const float listWidth = Tokens::componentLinkThreadList * s;

    std::vector<std::unique_ptr<Node>> listCaption;
    listCaption.push_back(text("Messages", Tokens::typeHeadline, ColorRole::Label1, s));
    listCaption.back()->setFlexGrow(1.0F);
    listCaption.push_back(ui::button({.glyph = std::string("plus"), .glyphSize = Tokens::sizeSymbol * s, .variant = ButtonVariant::Ghost,
                                      .tooltip = std::string("New message"), .onClick = [this] { openCompose({}); }}));
    std::unique_ptr<Node> list;
    if (messages.load() == Messages::Load::Loading) {
      list = ui::spacer();
    } else if (messages.threads().empty()) {
      list = emptyState("message", "No texts", "Texts on your phone show here.", s);
    } else {
      auto padded = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s, .flexGrow = 1.0F});
      padded->addChild(ui::virtualListView({.state = &m_threadScroll, .contentScale = s, .adapter = m_threads.get(),
                                            .flexGrow = 1.0F}));
      list = std::move(padded);
    }
    list->setFlexGrow(1.0F);
    cards->addChild(card(std::move(listCaption), std::move(list), listWidth, false));

    const float convWidth = std::max(0.0F, width - listWidth - Tokens::componentWindowCardGap * s);
    const Thread* thread = m_thread ? messages.thread(*m_thread) : nullptr;
    std::vector<std::unique_ptr<Node>> head;
    head.push_back(text(thread ? thread->name : m_compose ? std::string("New Message") : std::string(), Tokens::typeHeadline,
                        ColorRole::Label1, s));
    head.back()->setFlexGrow(1.0F);
    std::unique_ptr<Node> body;
    if (m_compose) {
      body = compose(s);
    } else if (thread == nullptr) {
      body = emptyState("message", "No conversation", "Pick a thread to read it.", s);
    } else {
      const std::string number = thread->address;
      const std::string device = m_client->presence().phone.id;
      head.push_back(ui::button({.glyph = std::string("phone"), .glyphSize = Tokens::sizeSymbol * s, .variant = ButtonVariant::Ghost,
                                 .tooltip = std::string("Call"), .onClick = [this, device, number] { m_client->dial(device, number); }}));
      m_conversation->setRows(messages.conversation(thread->id, nowMs()));
      auto column = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
      VirtualListView* log = nullptr;
      column->addChild(ui::virtualListView({.out = &log, .state = &m_conversationScroll, .contentScale = s,
                                            .itemGap = Tokens::spaceP4 * s, .adapter = m_conversation.get(), .flexGrow = 1.0F}));
      log->scrollView().setStickToBottom(true);
      log->setAnchorBottom(true);
      const std::uint64_t id = thread->id;
      const auto send = [this, id] {
        const std::string value = m_threadDrafts[id];
        if (value.empty()) return;
        m_threadDrafts.erase(id);
        m_client->sendText(id, value);
      };
      auto reply = ui::row({.align = FlexAlign::Center, .padding = Tokens::spaceP8 * s});
      reply->addChild(replyCapsule(m_threadDrafts[id], "Text message", Tokens::sizeControlL * s, s,
                                   [this, id](const std::string& value) { m_threadDrafts[id] = value; }, send));
      column->addChild(std::move(reply));
      body = std::move(column);
    }
    body->setFlexGrow(1.0F);
    cards->addChild(card(std::move(head), std::move(body), convWidth, true));
    return cards;
  }

  // New message: a "To" field with contact matches under it, the text capsule at the bottom.
  std::unique_ptr<Node> LinkApp::compose(float s) {
    const Messages& messages = m_client->messages();
    auto column = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
    auto to = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingV = Tokens::spaceP8 * s,
                       .paddingH = Tokens::spaceP16 * s});
    to->addChild(text("To", Tokens::typeBody, ColorRole::Label2, s));
    to->addChild(ui::input({.value = m_compose->to, .placeholder = std::string("Name or number"), .fontSize = Tokens::typeBody.size * s,
                            .frameVisible = false, .flexGrow = 1.0F,
                            .onChange = [this](const std::string& value) {
                              m_compose->to = value;
                              m_compose->sent.clear();
                              m_window->requestRebuild();
                            },
                            .configure = [](Input& input) {
                              if (input.inputArea() != nullptr) input.inputArea()->setTabFocusKey("link-compose-to");
                            }}));
    column->addChild(std::move(to));
    column->addChild(ui::separator({}));
    auto list = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s, .flexGrow = 1.0F});
    constexpr std::size_t kMatches = 6;
    for (const Contact& contact : messages.matches(m_compose->to, kMatches)) {
      const std::string number = contact.numbers.front();
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingH = Tokens::spaceP12 * s,
                          .height = Tokens::sizeControlL * s});
      row->addChild(initialsAvatar(contact.name, s));
      auto name = text(contact.name, Tokens::typeBody, ColorRole::Label1, s);
      name->setFlexGrow(1.0F);
      row->addChild(std::move(name));
      row->addChild(text(number, Tokens::typeFootnote, ColorRole::Label2, s));
      auto pick = ui::inputArea({.onClick = [this, number](const InputArea::PointerData&) { openCompose(number); }});
      pick->addChild(std::move(row));
      list->addChild(std::move(pick));
    }
    if (!m_compose->sent.empty()) {
      list->addChild(ui::row({.paddingV = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s},
                             text(std::format("Sent to {}", m_compose->sent), Tokens::typeFootnote, ColorRole::Label2, s)));
    }
    column->addChild(std::move(list));
    const auto send = [this] {
      const std::string number = m_client->messages().numberFor(m_compose->to);
      if (number.empty() || m_compose->body.empty()) return;
      m_client->sendTo(number, std::exchange(m_compose->body, {}));
      m_compose->sent = number;
      m_window->requestRebuild();
    };
    auto reply = ui::row({.align = FlexAlign::Center, .padding = Tokens::spaceP8 * s});
    reply->addChild(replyCapsule(m_compose->body, "Text message", Tokens::sizeControlL * s, s,
                                 [this](const std::string& value) { m_compose->body = value; }, send));
    column->addChild(std::move(reply));
    return column;
  }

  std::uint64_t FileAdapter::itemKey(std::size_t index) const {
    return std::hash<std::string>{}(m_client.files().entries()[index].name);
  }

  float FileAdapter::measureItem(Renderer& /*renderer*/, std::size_t /*index*/, float /*width*/) {
    return Tokens::sizeControlL * m_scale;
  }

  std::unique_ptr<Node> FileAdapter::createItem() { return ui::column({.align = FlexAlign::Stretch}); }

  // File row (flow.html .tr2): symbol 32 | name | size 112 | modified 112, h 40, r 8, fill_1 on hover.
  void FileAdapter::bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float /*width*/, bool hovered) {
    const float s = m_scale;
    const Files& files = m_client.files();
    const FileEntry& entry = files.entries()[index];
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s,
                        .fill = hovered ? colorSpecFromRole(ColorRole::Fill1) : clearColorSpec(),
                        .radius = Style::scaledRadius(Tokens::radiusR8, s), .height = Tokens::sizeControlL * s});
    auto symbol = ui::row({.align = FlexAlign::Center, .width = Tokens::componentLinkFileSymbol * s});
    symbol->setPadding(0.0F, 0.0F, 0.0F, Tokens::spaceP8 * s);
    symbol->addChild(ui::glyph({.glyph = std::string(entry.folder ? "folder" : "file"), .glyphSize = Tokens::sizeSymbol * s,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
    row->addChild(std::move(symbol));
    auto name = text(entry.name, Tokens::typeBody, ColorRole::Label1, s);
    name->setFlexGrow(1.0F);
    row->addChild(std::move(name));
    std::string size = entry.folder ? std::string() : byteSize(entry.size);
    ColorRole sizeRole = ColorRole::Label2;
    if (const FileSave* save = entry.folder ? nullptr : files.save(files.childPath(entry.name))) {
      switch (save->state) {
      case FileSave::State::Running:
        size = save->total > 0 ? std::format("Saving {} %", save->got * 100 / save->total) : std::string("Saving…");
        break;
      case FileSave::State::Done: size = "In Downloads"; break;
      case FileSave::State::Failed:
        size = "Not saved";
        sizeRole = ColorRole::DangerText;
        break;
      }
    }
    for (auto [value, role] : {std::pair{size, sizeRole}, std::pair{entry.mtimeS > 0 ? when(entry.mtimeS * 1000) : std::string(),
                                                                       ColorRole::Label2}}) {
      auto cell = text(value, Tokens::typeFootnote, role, s);
      cell->setTextAlign(TextAlign::End);
      auto box = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .width = Tokens::componentLinkFileColumn * s});
      box->setPadding(0.0F, Tokens::spaceP12 * s, 0.0F, 0.0F);
      box->addChild(std::move(cell));
      row->addChild(std::move(box));
    }
    auto& holder = static_cast<Flex&>(item);
    while (!holder.children().empty()) holder.removeChild(holder.children().back().get());
    holder.addChild(std::move(row));
  }

  void FileAdapter::onActivate(std::size_t index) {
    const Files& files = m_client.files();
    if (index >= files.entries().size()) return;
    const FileEntry& entry = files.entries()[index];
    if (entry.folder) {
      m_client.openFolder(files.childPath(entry.name));
      return;
    }
    const FileSave* save = files.save(files.childPath(entry.name));
    if (save != nullptr && save->state == FileSave::State::Done) {
      m_client.showInFolder({save->saved});
    } else {
      m_client.saveFile(entry.name);
    }
  }

  // Files (flow.html#page=files): caption Back + breadcrumb; header Name | Size | Modified; the folder's rows.
  std::unique_ptr<Node> LinkApp::files(float width, float height) {
    const float s = m_scale;
    const Files& files = m_client->files();
    std::vector<std::unique_ptr<Node>> caption;
    caption.push_back(ui::button({.glyph = std::string("chevron-left"), .enabled = files.path() != "/", .variant = ButtonVariant::Ghost,
                                  .tooltip = std::string("Back"), .onClick = [this] { m_client->openFolder(m_client->files().parent()); }}));
    auto crumbs = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .clipChildren = true, .flexGrow = 1.0F});
    const std::vector<std::string> names = files.crumbs();
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (i > 0) {
        crumbs->addChild(ui::glyph({.glyph = std::string("chevron-right"), .glyphSize = Tokens::sizeSymbol * s * 0.75F,
                                    .color = colorSpecFromRole(ColorRole::Label3)}));
      }
      if (i + 1 == names.size()) {
        crumbs->addChild(text(names[i], Tokens::typeHeadline, ColorRole::Label1, s));
      } else {
        const Button::ButtonStateColors rest{.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Label2)};
        const Button::ButtonStateColors hot{.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Label1)};
        auto crumb = ui::button({.text = names[i], .fontSize = Tokens::typeHeadline.size * s, .variant = ButtonVariant::Ghost,
                                 .customPalette = Button::ButtonPalette{.normal = rest, .hover = hot, .pressed = hot, .disabled = rest},
                                 .paddingV = 0.0F, .paddingH = 0.0F,
                                 .onClick = [this, i] { m_client->openFolder(m_client->files().crumbPath(i)); }});
        crumbs->addChild(std::move(crumb));
      }
    }
    caption.push_back(std::move(crumbs));
    caption.push_back(ui::button({.text = std::string("Send files"), .glyph = std::string("arrow-up"),
                                  .variant = ButtonVariant::Tonal,
                                  .onClick = [this] { chooseAndSend(m_client->presence().phone.id); }}));

    std::unique_ptr<Node> body;
    if (files.load() == Files::Load::Failed) {
      body = emptyState("folder-off", "This folder did not open", files.error(), s,
                        ui::button({.text = std::string("Try Again"), .variant = ButtonVariant::Secondary,
                                    .onClick = [this] { m_client->openFolder(m_client->files().path()); }}));
    } else if (files.load() == Files::Load::Ready && files.entries().empty()) {
      body = emptyState("folder", "Empty folder", "This folder on the phone has nothing in it.", s);
    } else {
      auto page = ui::column({.align = FlexAlign::Stretch});
      page->setPadding(Tokens::spaceP8 * s, Tokens::spaceP24 * s, 0.0F, Tokens::spaceP24 * s);
      auto head = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .height = Tokens::sizeControlM * s});
      head->addChild(ui::row({.width = Tokens::componentLinkFileSymbol * s}));
      for (const char* label : {"Name", "Size", "Modified"}) {
        auto cell = ui::label({.text = std::string(label), .fontSize = Tokens::typeFootnote.size * s,
                               .fontWeight = static_cast<FontWeight>(500), .color = colorSpecFromRole(ColorRole::Label2)});
        if (std::string_view(label) == "Name") {
          cell->setFlexGrow(1.0F);
          head->addChild(std::move(cell));
          continue;
        }
        cell->setTextAlign(TextAlign::End);
        auto box = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .width = Tokens::componentLinkFileColumn * s});
        box->setPadding(0.0F, Tokens::spaceP12 * s, 0.0F, 0.0F);
        box->addChild(std::move(cell));
        head->addChild(std::move(box));
      }
      page->addChild(std::move(head));
      page->addChild(ui::separator({.color = colorSpecFromRole(ColorRole::Separator), .thickness = s}));
      if (!files.entries().empty()) {
        page->addChild(ui::virtualListView({.state = &m_fileScroll, .contentScale = s, .adapter = m_files.get(), .flexGrow = 1.0F}));
      }
      body = std::move(page);
    }
    body->setFlexGrow(1.0F);
    auto cards = ui::row({.align = FlexAlign::Stretch, .width = width, .height = height});
    cards->addChild(card(std::move(caption), std::move(body), width, true));
    return cards;
  }

  std::string LinkApp::mirrorStat() const {
    const Mirror& mirror = m_client->mirror();
    const std::string transport = m_client->presence().phone.transport == "bluetooth" ? "Bluetooth" : "Wi-Fi";
    return std::format("{} × {} · {} fps · {}", mirror.width, mirror.height, m_client->mirrorFps(), transport);
  }

  void LinkApp::retireMirrorFrame(bool all) {
    if (m_renderer == nullptr) return;
    TextureManager& textures = m_renderer->textureManager();
    if (m_mirrorHold != nullptr || all) {
      m_mirrorRetired.push_back({.hold = std::move(m_mirrorHold), .fence = textures.submittedWorkFence()});
      textures.unload(m_mirrorTexture);
    }
    // At most 2 frames wait; an older fence is from paints frames ago, so the wait is short.
    while (!m_mirrorRetired.empty()) {
      RetiredFrame& oldest = m_mirrorRetired.front();
      pollfd fd{.fd = oldest.fence, .events = POLLIN, .revents = 0};
      const bool waitOldest = all || m_mirrorRetired.size() > 2;
      if (oldest.fence >= 0 && ::poll(&fd, 1, waitOldest ? -1 : 0) == 0) break;
      if (oldest.fence >= 0) ::close(oldest.fence);
      m_mirrorRetired.erase(m_mirrorRetired.begin());
    }
  }

  void LinkApp::showMirrorFrame(MirrorDecoder::Frame frame) {
    if (m_renderer == nullptr) return;
    TextureManager& textures = m_renderer->textureManager();
    bool uploaded = false;
    if (frame.dmabuf.fd >= 0) {
      TextureHandle imported = textures.importDmabuf(frame.dmabuf);
      if (!imported.valid()) {
        m_client->refuseMirrorDmabuf();
        return;
      }
      retireMirrorFrame(false);
      textures.unload(m_mirrorTexture);
      m_mirrorTexture = imported;
      m_mirrorHold = std::move(frame.hold);
      uploaded = true;
    } else {
      retireMirrorFrame(false); // an imported texture must not take updateSubImage: it writes into the VA surface
      const bool same = m_mirrorTexture.valid() && m_mirrorTexture.width == frame.width && m_mirrorTexture.height == frame.height;
      uploaded =
          (same && textures.updateSubImage(m_mirrorTexture, frame.rgba.data(), 0, 0, frame.width, frame.height, TextureDataFormat::Rgba)) ||
          textures.replace(m_mirrorTexture, frame.rgba.data(), frame.width, frame.height, TextureDataFormat::Rgba);
    }
    if (!uploaded || m_mirrorImage == nullptr) return;
    m_mirrorImage->setExternalTexture(*m_renderer, m_mirrorTexture);
    m_mirrorImage->markPaintDirty();
    if (m_mirrorStat != nullptr) m_mirrorStat->setText(mirrorStat());
    m_window->surface()->requestRedraw();
  }

  // Mirror (flow.html#page=mirror): a dark stage, the phone's screen centred, its size and rate above, keys below.
  std::unique_ptr<Node> LinkApp::mirror(float width, float height) {
    const float s = m_scale;
    const Mirror& mirror = m_client->mirror();
    const std::string phone = m_client->presence().phone.name;
    std::vector<std::unique_ptr<Node>> caption;
    caption.push_back(text("Mirror", Tokens::typeHeadline, ColorRole::Label1, s));
    auto name = text(phone, Tokens::typeFootnote, ColorRole::Label2, s);
    name->setFlexGrow(1.0F);
    caption.push_back(std::move(name));
    if (mirror.kind == Mirror::Kind::Showing || mirror.kind == Mirror::Kind::Asking) {
      caption.push_back(ui::button({.text = std::string("Stop"), .variant = ButtonVariant::Ghost, .onClick = [this] { m_client->stopMirror(); }}));
    }

    const float bodyH = height - Tokens::componentWindowCaption * s;
    auto stage = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP16 * s,
                             .fill = colorSpecFromRole(ColorRole::View), .flexGrow = 1.0F});
    stage->setPaletteTone(PaletteTone::Dark);
    const auto start = [this](const char* label) {
      return ui::button({.text = std::string(label), .variant = ButtonVariant::Primary, .onClick = [this] { m_client->startMirror(); }});
    };
    switch (mirror.kind) {
    case Mirror::Kind::Idle:
      stage->addChild(emptyState("device-mobile", "Show " + phone + "'s screen", "The phone asks before it shares its screen.", s, start("Start")));
      break;
    case Mirror::Kind::Asking:
      stage->addChild(emptyState("device-mobile", "Allow on the phone", "Tap Start now on " + phone + " to share its screen.", s));
      break;
    case Mirror::Kind::Ended:
      stage->addChild(emptyState("device-mobile-off", "Screen sharing stopped", phone + " stopped sharing its screen.", s, start("Start Again")));
      break;
    case Mirror::Kind::Failed:
      stage->addChild(emptyState("device-mobile-off", "No screen to show", mirror.error, s, start("Try Again")));
      break;
    case Mirror::Kind::Showing: {
      auto stat = text(mirrorStat(), Tokens::typeFootnote, ColorRole::Label2, s);
      m_mirrorStat = stat.get();
      stage->addChild(std::move(stat));
      // The video fills the stage height less the stat line, the key bar and their gaps, in the phone's aspect.
      const float aspect = static_cast<float>(mirror.width) / static_cast<float>(mirror.height);
      float videoH = std::max(1.0F, bodyH - Tokens::componentLinkMirrorChrome * s);
      float videoW = videoH * aspect;
      const float maxW = width - 2.0F * Tokens::spaceP24 * s;
      if (videoW > maxW) {
        videoW = maxW;
        videoH = videoW / aspect;
      }
      m_client->setMirrorTarget(static_cast<int>(std::lround(videoW)), static_cast<int>(std::lround(videoH)));
      Image* image = nullptr;
      auto area = ui::inputArea({.width = videoW, .height = videoH});
      const float corner = m_client->mirrorCorner() > 0 ? videoW * static_cast<float>(m_client->mirrorCorner()) / 10000.0F
                                                        : Style::scaledRadius(Tokens::componentLinkMirrorRadius, s);
      area->addChild(ui::image({.out = &image, .fit = ImageFit::Contain, .radius = corner,
                                .width = videoW, .height = videoH}));
      m_mirrorImage = image;
      if (m_mirrorTexture.valid()) image->setExternalTexture(*m_renderer, m_mirrorTexture);
      // MirrorInput takes positions in 1/10000 of the video.
      const auto at = [videoW, videoH](float x, float y) {
        return std::pair{static_cast<std::int32_t>(std::clamp(x / videoW, 0.0F, 1.0F) * 10000.0F),
                         static_cast<std::int32_t>(std::clamp(y / videoH, 0.0F, 1.0F) * 10000.0F)};
      };
      area->setOnPress([this, at](const InputArea::PointerData& data) {
        if (data.button != BTN_LEFT || data.pressed == m_touching) return;
        m_touching = data.pressed;
        const auto [x, y] = at(data.localX, data.localY);
        m_client->mirrorTouch(data.pressed ? "touch-down" : "touch-up", x, y);
      });
      area->setOnMotion([this, at](const InputArea::PointerData& data) {
        if (!m_touching) return;
        const auto [x, y] = at(data.localX, data.localY);
        m_client->mirrorTouch("touch-move", x, y);
      });
      stage->addChild(std::move(area));

      // FloatingCapsuleBar (handoff.yaml), dark glass: Back, Home, Recents | Volume, Lock.
      auto bar = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .fill = colorSpecFromRole(ColorRole::Popover),
                          .radius = Tokens::sizeControlXl * s / 2.0F, .height = Tokens::sizeControlXl * s});
      bar->setPadding(0.0F, Tokens::spaceP8 * s, 0.0F, Tokens::spaceP8 * s);
      const auto key = [&](const char* glyph, const char* tip, std::function<void()> press,
                           std::function<void()> secondary = nullptr) {
        bar->addChild(ui::button({.glyph = std::string(glyph), .glyphSize = Tokens::sizeSymbol * s, .variant = ButtonVariant::Ghost,
                                  .tooltip = std::string(tip),
                                  .onClick = std::move(press), .onRightClick = std::move(secondary)}));
      };
      key("chevron-left", "Back", [this] { m_client->mirrorAction("back"); });
      key("home", "Home", [this] { m_client->mirrorAction("home"); });
      key("dots", "Recent apps", [this] { m_client->mirrorAction("recents"); });
      bar->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Separator), .width = s, .height = Tokens::spaceP12 * s}));
      constexpr std::int32_t kVolumeUp = 24; // Android KeyEvent codes
      constexpr std::int32_t kVolumeDown = 25;
      constexpr std::int32_t kPower = 26;
      key("volume", "Volume up (right-click: down)", [this] { m_client->mirrorKey(kVolumeUp); },
          [this] { m_client->mirrorKey(kVolumeDown); });
      key("lock", "Lock", [this] { m_client->mirrorKey(kPower); });
      key("photo", "Screenshot", [this] { m_client->mirrorAction("screenshot"); });
      stage->addChild(std::move(bar));
      break;
    }
    }
    auto cards = ui::row({.align = FlexAlign::Stretch, .width = width, .height = height});
    cards->addChild(card(std::move(caption), std::move(stage), width, true));
    return cards;
  }

  // Calls (handoff.yaml): dial row (capsule "Name or number" + Primary Call), then the log grouped Today / Earlier.
  std::unique_ptr<Node> PhotoAdapter::createTile() {
    auto tile = std::make_unique<Node>();
    tile->addChild(ui::image({.fit = ImageFit::Cover}));
    // Rulebook §9.7 thumbnail grid: a 2 px label_1 ring at a 2 px gap and a 20 px accent check badge at inset 8.
    tile->addChild(ui::box({.fill = clearColorSpec(), .border = colorSpecFromRole(ColorRole::Label1), .borderWidth = 2.0F}));
    tile->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Accent)}));
    tile->addChild(ui::glyph({.glyph = std::string("check"), .color = colorSpecFromRole(ColorRole::OnAccent)}));
    return tile;
  }

  void PhotoAdapter::bindTile(Node& tile, std::size_t index, bool /*selected*/, bool /*hovered*/) {
    const float s = m_scale;
    const auto& kids = tile.children();
    auto* image = static_cast<Image*>(kids[0].get());
    auto* ring = static_cast<Box*>(kids[1].get());
    auto* badge = static_cast<Box*>(kids[2].get());
    auto* check = static_cast<Glyph*>(kids[3].get());
    const float w = tile.width();
    const float h = tile.height();
    const bool picked = m_client.photos().selected(index);
    const float radius = Style::scaledRadius(Tokens::radiusR12, s);
    const float inset = picked ? 4.0F * s : 0.0F; // the ring and its gap stay inside the cell
    image->setPosition(inset, inset);
    image->setSize(w - 2.0F * inset, h - 2.0F * inset);
    image->setRadius(std::max(0.0F, radius - inset));
    ring->setBorder(colorSpecFromRole(picked ? ColorRole::Label1 : ColorRole::Stroke), (picked ? 2.0F : 0.5F) * s);
    ring->setPosition(0.0F, 0.0F);
    ring->setSize(w, h);
    ring->setRadius(radius);
    const float d = 20.0F * s;
    badge->setVisible(picked);
    badge->setPosition(w - Tokens::spaceP8 * s - d, h - Tokens::spaceP8 * s - d);
    badge->setSize(d, d);
    badge->setRadius(d / 2.0F);
    check->setVisible(picked);
    check->setGlyphSize(Tokens::sizeSymbol * s * 0.75F);
    check->setPosition(badge->x() + (d - check->width()) / 2.0F, badge->y() + (d - check->height()) / 2.0F);
    const std::string& path = m_client.photos().items()[index].path;
    std::string& shown = m_shown[&tile];
    if (shown == path || m_renderer == nullptr) return;
    const std::vector<std::uint8_t>* png = m_client.thumbnail(path);
    if (png == nullptr) {
      image->clear(*m_renderer);
      shown.clear();
      return;
    }
    if (image->setSourceBytes(*m_renderer, png->data(), png->size())) shown = path;
  }

  std::string PhotoAdapter::itemTooltip(std::size_t index) const {
    return m_client.photos().items()[index].name;
  }

  std::unique_ptr<Node> PhotoAdapter::createSectionHeader() {
    auto header = ui::column({.align = FlexAlign::Start, .justify = FlexJustify::End});
    header->setPadding(Tokens::spaceP16 * m_scale, 0.0F, Tokens::spaceP8 * m_scale, 0.0F);
    header->addChild(ui::label({.fontSize = Tokens::typeFootnote.size * m_scale, .fontWeight = static_cast<FontWeight>(600),
                                .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1}));
    return header;
  }

  void PhotoAdapter::bindSectionHeader(Node& header, std::size_t section, float /*width*/, float /*height*/) {
    if (section < m_days.size()) static_cast<Label*>(header.children()[0].get())->setText(m_days[section].second);
  }

  std::unique_ptr<Node> LinkApp::photos(Renderer& renderer) {
    const float s = m_scale;
    const Photos& photos = m_client->photos();
    const auto retry = [this] {
      return ui::button({.text = std::string("Try Again"), .variant = ButtonVariant::Secondary,
                         .onClick = [this] { m_client->loadPhotos(); }});
    };
    switch (photos.load()) {
    case Photos::Load::Idle:
    case Photos::Load::Loading:
      if (photos.items().empty()) return emptyState("photo", "Loading photos", "The newest photos on the phone show here.", s);
      break;
    case Photos::Load::Denied:
      return emptyState("photo-off", "Link cannot see photos", "Allow Photos access for Link in the phone's settings.", s, retry());
    case Photos::Load::Unsupported:
      return emptyState("photo-off", "This phone does not share photos", "Update Link on the phone to see its new photos here.", s);
    case Photos::Load::Failed:
      return emptyState("alert-circle", "Photos did not load", "The phone did not answer in time.", s, retry());
    case Photos::Load::Ready:
      if (photos.items().empty()) return emptyState("photo", "No photos", "New photos on the phone show here.", s);
      break;
    }
    m_photos->setRenderer(&renderer);
    auto days = photos.days(nowMs());
    std::vector<std::size_t> starts;
    starts.reserve(days.size());
    for (const auto& day : days) starts.push_back(day.first);
    m_photos->setDays(std::move(days));
    auto page = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
    page->setPadding(0.0F, Tokens::spaceP24 * s, 0.0F, Tokens::spaceP24 * s);
    const float headerH = (Tokens::spaceP16 + Tokens::typeFootnote.line + Tokens::spaceP8) * s;
    page->addChild(ui::virtualGridView({.out = &m_photoGrid, .state = &m_photoScroll, .contentScale = s,
                                        .minCellWidth = Tokens::componentLinkPhotoMin * s, .squareCells = true,
                                        .columnGap = Tokens::spaceP8 * s, .rowGap = Tokens::spaceP8 * s, .overscanRows = 2,
                                        .itemCursorShape = CursorShape::Pointer, .adapter = m_photos.get(), .flexGrow = 1.0F}));
    m_photoGrid->setSections(std::move(starts), headerH);

    // FloatingCapsuleBar (handoff.yaml): "n selected · Save · Clear", or how the last save went.
    std::string what;
    if (photos.selectedCount() > 0) {
      what = std::format("{} selected", photos.selectedCount());
    } else if (photos.save() == Photos::Save::Saving) {
      what = "Saving to Downloads…";
    } else if (photos.save() == Photos::Save::Done) {
      what = photos.saveFailed() == 0 ? std::format("Saved {} to Downloads", photos.saved())
                                      : std::format("{} not saved", photos.saveFailed());
    }
    if (!what.empty()) {
      auto bar = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .fill = colorSpecFromRole(ColorRole::Popover),
                          .radius = Tokens::sizeControlL * s / 2.0F, .height = Tokens::sizeControlL * s});
      bar->setPadding(0.0F, Tokens::spaceP8 * s, 0.0F, Tokens::spaceP16 * s);
      bar->addChild(text(what, Tokens::typeBody, ColorRole::Label1, s));
      if (photos.selectedCount() > 0) {
        const bool saving = photos.save() == Photos::Save::Saving;
        bar->addChild(ui::button({.text = std::string("Save"), .glyph = std::string("download"), .enabled = !saving,
                                  .variant = ButtonVariant::Ghost, .onClick = [this] { m_client->savePhotos(); }}));
        bar->addChild(ui::button({.text = std::string("Clear"), .glyph = std::string("x"), .variant = ButtonVariant::Ghost,
                                  .onClick = [this] { m_client->clearPhotoSelection(); }}));
      }
      auto foot = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center});
      foot->setPadding(Tokens::spaceP8 * s, 0.0F, Tokens::spaceP24 * s, 0.0F);
      foot->addChild(std::move(bar));
      page->addChild(std::move(foot));
    }
    return page;
  }

  std::unique_ptr<Node> LinkApp::calls() {
    const float s = m_scale;
    switch (m_client->callLog()) {
    case CallLog::Off:
      return emptyState("phone-off", "Calls are off", "Turn on Calls in Link on your phone to see them here.", s);
    case CallLog::Denied:
      return emptyState("phone-off", "Link cannot read calls", "Allow Phone access for Link in the phone's settings.", s);
    case CallLog::Failed:
      return emptyState("alert-circle", "Calls did not load", "The phone could not read its call log.", s);
    case CallLog::Ready: break;
    }
    auto page = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
    page->setPadding(0.0F, Tokens::spaceP24 * s, Tokens::spaceP24 * s, Tokens::spaceP24 * s);
    const std::string device = m_client->presence().phone.id;
    const auto dial = [this, device] {
      const std::string number = m_client->messages().numberFor(m_dial);
      if (number.empty()) return;
      m_dial.clear();
      m_client->dial(device, number);
      m_window->requestRebuild();
    };
    const float h = Tokens::sizeControlM * s;
    auto field = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP8 * s,
                          .fill = colorSpecFromRole(ColorRole::Fill1), .radius = h / 2.0F, .height = h, .flexGrow = 1.0F});
    field->addChild(ui::glyph({.glyph = std::string("phone"), .glyphSize = Tokens::sizeSymbol * s,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
    field->addChild(ui::input({.value = m_dial, .placeholder = std::string("Name or number"), .fontSize = Tokens::typeBody.size * s,
                               .frameVisible = false, .flexGrow = 1.0F,
                               .onChange = [this](const std::string& value) { m_dial = value; },
                               .onSubmit = [dial](const std::string&) { dial(); }}));
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingV = Tokens::spaceP8 * s});
    row->addChild(std::move(field));
    row->addChild(ui::button({.text = std::string("Call"), .variant = ButtonVariant::Primary, .onClick = dial}));
    page->addChild(std::move(row));
    if (m_client->calls().empty()) {
      page->addChild(emptyState("phone", "No calls", "Calls on your phone show here.", s));
    } else {
      page->addChild(ui::virtualListView({.state = &m_callScroll, .contentScale = s, .adapter = m_calls.get(), .flexGrow = 1.0F}));
    }
    return page;
  }

  // Activity (handoff.yaml): transfers since the app started; Link1 keeps no list.
  std::unique_ptr<Node> LinkApp::activity() {
    const float s = m_scale;
    if (m_client->activity().items().empty()) {
      return emptyState("arrows-down-up", "No transfers", "Files you send or receive show here.", s);
    }
    auto page = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
    page->setPadding(0.0F, Tokens::spaceP24 * s, Tokens::spaceP24 * s, Tokens::spaceP24 * s);
    page->addChild(ui::virtualListView({.state = &m_transferScroll, .contentScale = s, .adapter = m_transfers.get(),
                                        .flexGrow = 1.0F}));
    return page;
  }

  // Settings (handoff.yaml): max width 640; grouped cards (fill_1 r 12 p 4) of rows with a Toggle; Ghost destructive
  // "Forget <phone>…" that asks in place.
  std::unique_ptr<Node> LinkApp::settings(const Presence& presence) {
    const float s = m_scale;
    const PhoneSettings& phone = m_client->phoneSettings();
    const DesktopSettings& desk = m_client->desktopSettings();
    const std::string id = presence.phone.id;
    const auto header = [&](const std::string& title) {
      auto label = text(title, Tokens::typeFootnote, ColorRole::Label2, s);
      label->setFontWeight(FontWeight::SemiBold);
      auto line = ui::row({}, std::move(label));
      line->setPadding(Tokens::spaceP16 * s, 0.0F, Tokens::spaceP8 * s, Tokens::componentListRowInset * s);
      return line;
    };
    const auto group = [&] {
      return ui::column({.align = FlexAlign::Stretch, .padding = Tokens::componentListCardPadding * s,
                         .fill = colorSpecFromRole(ColorRole::Fill1),
                         .radius = Style::scaledRadius(Tokens::componentListCardRadius, s)});
    };
    const float inset = Tokens::componentListRowInset * s;
    const auto row = [&](Flex& card, const char* glyph, const std::string& title, const std::string& hint,
                         std::unique_ptr<Node> trail) {
      if (!card.children().empty()) {
        auto rule = ui::column({.align = FlexAlign::Stretch});
        rule->setPadding(0.0F, inset, 0.0F, inset + Tokens::sizeSymbol * s + Tokens::spaceP12 * s);
        rule->addChild(ui::separator({}));
        card.addChild(std::move(rule));
      }
      auto r = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingV = Tokens::spaceP8 * s,
                        .paddingH = inset, .minHeight = Tokens::sizeControlL * s});
      r->addChild(ui::glyph({.glyph = std::string(glyph), .glyphSize = Tokens::sizeSymbol * s,
                             .color = colorSpecFromRole(ColorRole::Label1)}));
      auto words = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
      words->addChild(text(title, Tokens::typeBody, ColorRole::Label1, s, 2));
      if (!hint.empty()) words->addChild(text(hint, Tokens::typeFootnote, ColorRole::Label2, s, 2));
      r->addChild(std::move(words));
      if (trail) r->addChild(std::move(trail));
      card.addChild(std::move(r));
    };
    const auto toggle = [&](bool on, std::function<void(bool)> change) {
      return ui::toggle({.checked = on, .scale = s, .onChange = std::move(change)});
    };

    auto page = ui::column({.align = FlexAlign::Stretch, .fillWidth = true});
    page->setPadding(0.0F, 0.0F, Tokens::spaceP24 * s, 0.0F);
    page->addChild(header(presence.phone.name));
    auto phoneCard = group();
    row(*phoneCard, "bell", "Show phone notifications", "Banners and the Waiting page",
        toggle(phone.notifications, [this, id](bool on) { m_client->setGrant(id, "notifications", on); }));
    row(*phoneCard, "clipboard", "Share clipboard", "Both ways, text and images",
        toggle(phone.clipboard, [this, id](bool on) { m_client->setGrant(id, "clipboard", on); }));
    row(*phoneCard, "arrow-down", "Save offered files without asking", "To Downloads",
        toggle(phone.autoAccept, [this, id](bool on) { m_client->setAutoAccept(id, on); }));
    row(*phoneCard, "device-mobile", "Allow screen mirroring", "",
        toggle(phone.screen, [this, id](bool on) { m_client->setGrant(id, "screen", on); }));
    row(*phoneCard, "access-point", "Join phone hotspot", "For files and mirroring when Wi-Fi Direct fails",
        toggle(phone.nearby, [this, id](bool on) { m_client->setGrant(id, "nearby", on); }));
    page->addChild(std::move(phoneCard));

    page->addChild(header("This computer"));
    auto deskCard = group();
    row(*deskCard, "lock", "Show phone notifications while locked", "Off: the lock screen shows only how many",
        toggle(desk.notificationsWhileLocked, [this](bool on) { m_client->setNotificationsWhileLocked(on); }));
    row(*deskCard, "device-laptop", "Visible to nearby phones",
        desk.nearbyName.empty() ? std::string("Quick Share") : std::format("As “{}”", desk.nearbyName),
        toggle(desk.nearbyVisible, [this](bool on) { m_client->setNearbyVisible(on); }));
    row(*deskCard, "terminal-2", "Commands the phone can run",
        desk.commands == 1 ? std::string("1 command") : std::format("{} commands", desk.commands), nullptr);
    page->addChild(std::move(deskCard));

    auto forget = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    forget->setPadding(Tokens::spaceP24 * s, 0.0F, 0.0F, 0.0F);
    if (m_confirmForget) {
      auto ask = text(std::format("Forget {}? It must pair again to connect.", presence.phone.name), Tokens::typeBody,
                      ColorRole::Label1, s, 2);
      ask->setFlexGrow(1.0F);
      forget->addChild(std::move(ask));
      forget->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Tonal, .onClick = [this] {
                                     m_confirmForget = false;
                                     m_window->requestRebuild();
                                   }}));
      forget->addChild(ui::button({.text = std::string("Forget"), .variant = ButtonVariant::Destructive, .onClick = [this, id] {
                                     m_confirmForget = false;
                                     m_client->unpair(id);
                                   }}));
    } else {
      Button* button = nullptr;
      forget->addChild(ui::button({.out = &button, .text = std::format("Forget {}…", presence.phone.name),
                                   .variant = ButtonVariant::Ghost, .onClick = [this] {
                                     m_confirmForget = true;
                                     m_window->requestRebuild();
                                   }}));
      auto palette = Button::defaultPalette(ButtonVariant::Ghost);
      for (auto* state : {&palette.normal, &palette.hover, &palette.pressed}) state->label = colorSpecFromRole(ColorRole::DangerText);
      button->setCustomPalette(palette);
    }
    page->addChild(std::move(forget));
    auto scroll = ui::scrollView({.state = &m_settingsScroll, .contentScale = s, .viewportPaddingH = Tokens::spaceP24 * s,
                                  .maxContentWidth = Tokens::componentLinkSettingsWidth * s,
                                  .contentAlign = ScrollView::ContentAlign::Start, .flexGrow = 1.0F});
    scroll->content()->addChild(std::move(page));
    return scroll;
  }

} // namespace linkapp
