#include "link_app.h"

#include "app/app_window.h"
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
#include <format>
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
    return build(index, width)->measure(renderer, LayoutConstraints::available(width, 1.0e6F)).height;
  }

  std::unique_ptr<Node> FeedAdapter::createItem() { return ui::column({.align = FlexAlign::Stretch}); }

  void FeedAdapter::bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float width, bool /*hovered*/) {
    auto& holder = static_cast<Flex&>(item);
    while (!holder.children().empty()) holder.removeChild(holder.children().back().get());
    holder.addChild(build(index, width));
  }

  // FeedItem (handoff.yaml): avatar 32 | who headline + time footnote; body paragraph, 2 lines; actions row g 8.
  std::unique_ptr<Node> FeedAdapter::build(std::size_t index, float /*width*/) {
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
    head->addChild(text(offer ? byteSize(item.total) : clock(item.atMs), Tokens::typeFootnote, ColorRole::Label2, s));
    body->addChild(std::move(head));
    const std::string line = offer ? std::string("From your phone")
                             : locked ? std::string(kLockedText)
                                      : item.body;
    body->addChild(text(line, Tokens::typeParagraph, locked && !offer ? ColorRole::Label2 : ColorRole::Label1, s, 2));

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
      m_feed = std::make_unique<FeedAdapter>(*m_client, m_scale);
      m_threads = std::make_unique<ThreadAdapter>(*m_client, m_scale, m_thread, [this](std::uint64_t id) {
        if (m_thread == id) return;
        m_thread = id;
        m_conversationScroll = {};
        sync();
        m_window->requestRebuild();
      });
      m_conversation = std::make_unique<ConversationAdapter>(*m_client, m_scale);
      m_calls = std::make_unique<CallAdapter>(*m_client, m_scale);
      m_transfers = std::make_unique<TransferAdapter>(*m_client, m_scale);
      m_client->setOnChanged([this] {
        if (m_window == nullptr) return;
        sync();
        updateCaption();
        m_window->requestRebuild();
      });
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
    if (name == "page" && parameter.size() == 1 && m_window != nullptr) {
      const auto it = std::ranges::find_if(kPages, [&](const PageInfo& info) {
        return std::ranges::equal(std::string_view(info.name), parameter.front(),
                                  [](char a, char b) { return std::tolower(a) == std::tolower(b); });
      });
      if (it != kPages.end()) {
        show(it->page);
        return;
      }
    }
    DeskApp::action(name, parameter, activationToken);
  }

  void LinkApp::show(Page page) {
    if (m_page == page) return;
    m_page = page;
    m_confirmForget = false;
    sync();
    updateCaption();
    m_window->requestRebuild();
  }

  void LinkApp::sync() {
    if (m_client == nullptr) return;
    const Presence::Kind kind = m_client->presence().kind;
    const bool pairing = kind == Presence::Kind::NotPaired;
    if (pairing && m_client->pair().kind == Pair::Kind::Idle) m_client->startPairing();
    m_window->setTitle(pairing ? "Pair a Phone" : kPages[static_cast<std::size_t>(m_page)].name);
    const bool cards = m_page == Page::Messages && (kind == Presence::Kind::Connected || kind == Presence::Kind::Locked);
    const Messages& messages = m_client->messages();
    if (cards && (!m_thread || messages.thread(*m_thread) == nullptr)) {
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
      case Page::Photos:
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
    if (presence.kind == Presence::Kind::Connected || presence.kind == Presence::Kind::Locked) {
      // RoundKey (handoff.yaml): 32 circle fill_1, on = accent.
      const float key = Tokens::sizeControlM * s;
      const auto round = [&](const char* glyph, const char* tip, bool on, std::function<void()> click) {
        return ui::button({.glyph = std::string(glyph), .glyphSize = Tokens::sizeSymbolInline * s,
                           .variant = on ? ButtonVariant::Primary : ButtonVariant::Tonal, .tooltip = std::string(tip),
                           .radius = key / 2.0F, .width = key, .height = key, .onClick = std::move(click)});
      };
      const std::string id = presence.phone.id;
      const bool dnd = presence.phone.dnd == 1;
      auto keys = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .paddingV = Tokens::spaceP4 * s});
      keys->addChild(round("bell", "Ring phone", false, [this, id] { m_client->ring(id); }));
      keys->addChild(round("device-mobile", "Mirror", false, [this] { show(Page::Mirror); }));
      keys->addChild(round("circle-minus", "Do not disturb", dnd, [this, id, dnd] { m_client->setDnd(id, !dnd); }));
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
    if (m_page == Page::Activity) return activity();
    if (m_page == Page::Settings) return settings(presence);
    if (m_page != Page::Waiting) {
      const PageInfo& info = kPages[static_cast<std::size_t>(m_page)];
      return emptyState(info.glyph, info.name, "This page comes in a later build step.", s);
    }
    if (m_client->feed().items().empty()) {
      return emptyState("bell", "Nothing waiting", "Texts, missed calls and files from your phone show here.", s);
    }
    VirtualListView* list = nullptr;
    auto view = ui::virtualListView({.out = &list, .state = &m_feedScroll, .contentScale = s, .adapter = m_feed.get(),
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
    head.push_back(text(thread ? thread->name : std::string(), Tokens::typeHeadline, ColorRole::Label1, s));
    head.back()->setFlexGrow(1.0F);
    std::unique_ptr<Node> body;
    if (thread == nullptr) {
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

  // Calls (handoff.yaml): dial row (capsule "Name or number" + Primary Call), then the log grouped Today / Earlier.
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
