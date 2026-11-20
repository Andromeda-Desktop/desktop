#include "link_app.h"

#include "app/app_window.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/selection_list.h"
#include "ui/controls/status_row.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <chrono>
#include <ctime>
#include <format>

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

    std::string size(std::uint64_t bytes) {
      if (bytes >= 1'000'000'000) return std::format("{:.1f} GB", static_cast<double>(bytes) / 1e9);
      if (bytes >= 1'000'000) return std::format("{:.1f} MB", static_cast<double>(bytes) / 1e6);
      return std::format("{} KB", (bytes + 999) / 1000);
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
    const float avatar = Tokens::sizeControlM * s;
    auto lead = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .fill = colorSpecFromRole(ColorRole::Fill2),
                         .radius = avatar / 2.0F, .width = avatar, .height = avatar});
    switch (item.kind) {
    case FeedItem::Kind::Text:
      lead->addChild(text(initials(item.who), Tokens::typeCaption, ColorRole::Label2, s));
      break;
    case FeedItem::Kind::Offer:
      lead->addChild(ui::glyph({.glyph = std::string("file"), .glyphSize = Tokens::sizeSymbol * s,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
      break;
    case FeedItem::Kind::MissedCall:
      lead->addChild(ui::glyph({.glyph = std::string("phone-x"), .glyphSize = Tokens::sizeSymbol * s,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
      break;
    case FeedItem::Kind::Notification:
      lead->addChild(ui::glyph({.glyph = std::string("bell"), .glyphSize = Tokens::sizeSymbol * s,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
      break;
    }
    row->addChild(std::move(lead));

    auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * s, .flexGrow = 1.0F});
    const bool offer = item.kind == FeedItem::Kind::Offer;
    const std::string who = offer ? (item.files.size() == 1 ? item.files.front().first
                                                            : std::format("{} files", item.files.size()))
                                  : item.who;
    auto head = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    auto name = text(who, Tokens::typeHeadline, ColorRole::Label1, s);
    name->setFlexGrow(1.0F);
    head->addChild(std::move(name));
    head->addChild(text(offer ? size(item.total) : clock(item.atMs), Tokens::typeFootnote, ColorRole::Label2, s));
    body->addChild(std::move(head));
    const std::string line = offer ? std::string("From your phone")
                             : locked ? std::string("Unlock your phone to see the text")
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
      Input* field = nullptr;
      auto capsule = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s,
                              .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::sizeControlM * s / 2.0F,
                              .height = Tokens::sizeControlM * s, .flexGrow = 1.0F});
      capsule->addChild(ui::input({.out = &field, .value = m_drafts[key], .placeholder = std::string("Reply"),
                                   .fontSize = Tokens::typeBody.size * s, .frameVisible = false, .flexGrow = 1.0F,
                                   .onChange = [this, key](const std::string& value) { m_drafts[key] = value; },
                                   .onSubmit =
                                       [this, copy, key](const std::string& value) {
                                         if (value.empty()) return;
                                         m_drafts.erase(key);
                                         m_client.reply(copy, value);
                                       }}));
      capsule->addChild(ui::button({.glyph = std::string("send"), .glyphSize = Tokens::sizeSymbolInline * s,
                                    .variant = ButtonVariant::Primary, .tooltip = std::string("Send"),
                                    .radius = Tokens::sizeControlS * s / 2.0F, .width = Tokens::sizeControlS * s,
                                    .height = Tokens::sizeControlS * s,
                                    .onClick =
                                        [this, copy, key] {
                                          const std::string value = m_drafts[key];
                                          if (value.empty()) return;
                                          m_drafts.erase(key);
                                          m_client.reply(copy, value);
                                        }}));
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
      m_client->setOnChanged([this] {
        if (m_window == nullptr) return;
        updateCaption();
        m_window->requestRebuild();
      });
    }
    window.setSidebarBuilder([this](Renderer&, float width, float) { return sidebar(width); });
    window.setContentBuilder([this](Renderer&, float width, float height) { return content(width, height); });
    const auto clearAll = [this] {
      return ui::button({.text = std::string("Clear all"), .variant = ButtonVariant::Ghost,
                         .onClick = [this] { m_client->clearAll(); }});
    };
    window.setHeaderAccessoryBuilder([this, clearAll]() -> std::unique_ptr<Node> {
      if (m_page != Page::Waiting || m_client == nullptr || m_client->feed().items().empty()) return ui::node();
      return ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End}, clearAll());
    });
    window.setHeaderAccessoryWidths([clearAll](Renderer& renderer) {
      const float width = clearAll()->measure(renderer, LayoutConstraints::available(1.0e6F, 1.0e6F)).width;
      return AppWindow::HeaderAccessoryWidths{.floor = width, .keep = width};
    });
    return window;
  }

  void LinkApp::show(Page page) {
    if (m_page == page) return;
    m_page = page;
    m_window->setTitle(kPages[static_cast<std::size_t>(page)].name);
    updateCaption();
    m_window->requestRebuild();
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
        break;
      case Page::Messages: line = summary(FeedItem::Kind::Text, "new"); break;
      case Page::Calls: line = summary(FeedItem::Kind::MissedCall, "missed"); break;
      case Page::Activity:
        line = summary(FeedItem::Kind::Offer, "offered");
        line.second.clear();
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
    side->addChild(std::move(list));
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
      break;
    case Presence::Kind::Pairing:
      name = "Pairing…";
      break;
    case Presence::Kind::Away:
      line = "Not connected";
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
      status->addChild(ui::box({.fill = colorSpecFromRole(live ? ColorRole::Success : ColorRole::Label3),
                                .radius = Tokens::spaceP8 * s / 2.0F, .width = Tokens::spaceP8 * s,
                                .height = Tokens::spaceP8 * s}));
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

  std::unique_ptr<Node> LinkApp::content(float /*width*/, float /*height*/) {
    const float s = m_scale = m_window->uiScale();
    if (m_client == nullptr) return emptyState("plug-connected-x", "No session bus", "Link needs a D-Bus session.", s);
    const Presence presence = m_client->presence();
    switch (presence.kind) {
    case Presence::Kind::NoDaemon:
      return emptyState("plug-connected-x", "Link is not running", "Start the Link service to reach your phone.", s);
    case Presence::Kind::NotPaired:
    case Presence::Kind::Pairing:
      return emptyState("device-mobile", "No phone yet", "Open Link on your phone and pair it with this desktop.", s);
    case Presence::Kind::Away: {
      const std::string id = presence.phone.id;
      return emptyState("device-mobile-off", presence.phone.name + " is not connected",
                        "Link connects when the phone is on the same Wi-Fi or in Bluetooth range.", s,
                        ui::button({.text = std::string("Connect"), .variant = ButtonVariant::Primary,
                                    .onClick = [this, id] { m_client->connect(id); }}));
    }
    case Presence::Kind::Connected:
    case Presence::Kind::Locked: break;
    }
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

} // namespace linkapp
