// andromeda-share-picker: the ScreenCast/Screenshot source sheet (.agent/mockups/system-dialogs.html §2).
// Reads one JSON request line on stdin (WaylandContext::buildChooserJson) and prints one answer line:
// {"selections":[...], "show_pointer":bool, "remember":bool}; no selections = cancel.
#include "app/app_loop.h"
#include "app/app_window.h"
#include "app/desk_app.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "design_tokens.h"
#include "render/core/texture_manager.h"
#include "share_picker/thumbnails.h"
#include "ui/app_icon.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <gio/gdesktopappinfo.h>
#include <nlohmann/json.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <cstdio>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

  constexpr std::uint32_t kSheetWidth = 656;
  // Two rows of tiles; more rows scroll.
  constexpr std::uint32_t kSheetHeight = 448;
  constexpr float kThumbHeight = 96.0F;
  // The selection ring band around a thumbnail: Style::focusRingGap, then Style::focusRingWidth.
  constexpr float kRingBand = 4.0F;

  struct Source {
    bool monitor = false;
    std::string id; // output name or toplevel identifier
    std::string appId;
    std::string title;
    bool blocked = false;
  };

  struct Request {
    bool multiple = false;
    bool monitors = false;
    bool windows = false;
    std::vector<Source> sources;
    std::string appId;
    std::string parentWindow;
    bool cursorOption = false;
    bool showPointer = false;
    bool canRemember = false;
  };

  enum class Tab { Window, App, Screen };
  enum class Answer { Pending, Answered };

  // Empty when the line is not a valid request: the picker then answers cancel without a window.
  std::optional<Request> parseRequest(const std::string& line) try {
    Request request;
    const auto json = nlohmann::json::parse(line, nullptr, false);
    if (!json.is_object()) {
      return std::nullopt;
    }
    request.multiple = json.value("multiple", false);
    for (const auto& type : json.value("types", nlohmann::json::array())) {
      request.monitors = request.monitors || type == "monitor";
      request.windows = request.windows || type == "window";
    }
    for (const auto& output : json.value("outputs", nlohmann::json::array())) {
      const std::string description = output.value("description", "");
      request.sources.push_back({.monitor = true,
                                 .id = output.value("name", ""),
                                 .title = description.empty() ? output.value("title", "") : description});
    }
    for (const auto& window : json.value("windows", nlohmann::json::array())) {
      request.sources.push_back({.id = window.value("identifier", ""),
                                 .appId = window.value("app_id", ""),
                                 .title = window.value("title", ""),
                                 .blocked = window.value("capture_blocked", false)});
    }
    request.appId = json.value("app_id", "");
    request.parentWindow = json.value("parent_window", "");
    const std::string cursor = json.value("cursor_mode", "");
    request.cursorOption = !cursor.empty();
    request.showPointer = !cursor.empty() && cursor != "hidden";
    request.canRemember = json.value("can_remember", false);
      return request;
    } catch (const nlohmann::json::exception&) {
      return std::nullopt;
    }

  // The asking app's name from its desktop entry; empty when the portal could not name the caller.
  struct AppInfo {
    std::string name;
    std::string icon;
  };

  std::string appName(const std::string& appId) {
    if (appId.empty()) {
      return {};
    }
    GDesktopAppInfo* info = g_desktop_app_info_new((appId + ".desktop").c_str());
    if (info == nullptr) {
      return {};
    }
    std::string name = g_app_info_get_name(G_APP_INFO(info));
    g_object_unref(info);
    return name;
  }

  class Picker {
  public:
    Picker(Request request, AppLoop& loop) : m_request(std::move(request)), m_loop(loop) {
      m_name = appName(m_request.appId);
      m_showPointer = m_request.showPointer;
      m_tab = m_request.windows ? Tab::Window : Tab::Screen;
      m_textures.resize(m_request.sources.size());
      std::vector<xdpu::ThumbnailSource> sources;
      for (const Source& source : m_request.sources) {
        sources.push_back({.monitor = source.monitor, .identifier = source.id});
      }
      m_thumbnails = std::make_unique<xdpu::Thumbnails>(
          std::move(sources), [this] { DeferredCall::callLater([this] { onThumbnails(); }); });
    }

    [[nodiscard]] std::string title() const {
      return m_name.empty() ? std::string("Choose what to share") : "Choose what " + m_name + " can see";
    }

    std::unique_ptr<Node> build(Renderer& renderer, float scale) {
      m_images.assign(m_request.sources.size(), nullptr);
      m_scroll = nullptr;
      auto sheet = ui::column(
          {.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * scale, .padding = Tokens::spaceP12 * scale, .flexGrow = 1.0F});
      sheet->addChild(header(scale));
      sheet->addChild(grid(renderer, scale));
      sheet->addChild(options(scale));
      sheet->addChild(buttons(scale));
      return sheet;
    }

    bool key(const KeyboardEvent& event) {
      if (!event.pressed) {
        return false;
      }
      if (event.sym == XKB_KEY_Escape) {
        finish(false);
        return true;
      }
      // A focused widget (Tab moved focus to Cancel or a tile) handles Enter itself.
      const bool plain = (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == 0;
      if ((event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter) && plain && canShare()
          && m_window->input().focusedArea() == nullptr) {
        finish(true);
        return true;
      }
      return false;
    }

    void attach(AppWindow& window) { m_window = &window; }

    // Starts live capture for the tiles inside the scroll viewport and stops it for the rest.
    void updateShown() {
      if (m_scroll == nullptr) {
        return;
      }
      float left = 0.0F;
      float top = 0.0F;
      Node::absolutePosition(m_scroll, left, top);
      const float bottom = top + m_scroll->height();
      std::vector<std::size_t> shown;
      for (std::size_t index = 0; index < m_images.size(); ++index) {
        if (m_images[index] == nullptr) {
          continue;
        }
        float x = 0.0F;
        float y = 0.0F;
        Node::absolutePosition(m_images[index], x, y);
        if (y + m_images[index]->height() > top && y < bottom) {
          shown.push_back(index);
        }
      }
      if (shown != m_shown) {
        m_shown = shown;
        m_thumbnails->show(std::move(shown));
      }
    }

    // Window close and Escape cancel; Share prints the selection. Only the first answer counts.
    void finish(bool share) {
      if (m_answer == Answer::Answered) {
        return;
      }
      m_answer = Answer::Answered;
      nlohmann::json selections = nlohmann::json::array();
      if (share) {
        for (const Source& source : m_request.sources) {
          if (!selectedSource(source)) {
            continue;
          }
          if (source.monitor) {
            selections.push_back({{"kind", "monitor"}, {"output", source.id}});
          } else {
            selections.push_back({{"kind", "window"}, {"identifier", source.id}});
          }
        }
      }
      nlohmann::json answer{{"selections", selections}};
      if (m_request.cursorOption) {
        answer["show_pointer"] = m_showPointer;
      }
      if (m_request.canRemember) {
        answer["remember"] = m_remember;
      }
      std::cout << answer.dump() << std::endl;
      m_loop.quit(0);
    }

  private:
    void onThumbnails() {
      for (xdpu::Thumbnail& thumbnail : m_thumbnails->take()) {
        const std::size_t index = thumbnail.index;
        m_window->updateInPlace([this, index, frame = std::move(thumbnail)](Renderer& renderer) {
          upload(renderer, frame, index);
        });
      }
    }

    // One texture per source outlives rebuilds: a rebuild binds it again instead of uploading the frame.
    void upload(Renderer& renderer, const xdpu::Thumbnail& frame, std::size_t index) {
      TextureHandle& texture = m_textures[index];
      if (texture.valid()) {
        renderer.textureManager().unload(texture);
      }
      texture = renderer.textureManager().loadFromRaw(frame.rgb.data(), frame.rgb.size(), frame.width, frame.height,
                                                      frame.width * 3, PixmapFormat::RGB);
      if (Image* image = m_images[index]; image != nullptr) {
        image->setExternalTexture(renderer, texture);
      }
    }

    // A selection key: the output name, the window identifier, or on the App tab the app id.
    [[nodiscard]] std::string keyOf(const Source& source) const {
      return m_tab == Tab::App && !source.monitor ? source.appId : source.id;
    }

    const AppInfo& appInfo(const std::string& appId) const {
      auto it = m_apps.find(appId);
      if (it == m_apps.end()) {
        it = m_apps.emplace(appId, AppInfo{.name = appName(appId), .icon = appIconFile(appId)}).first;
      }
      return it->second;
    }

    [[nodiscard]] bool canShare() const { return !m_selected.empty() && m_selectedTab == m_tab; }
    
    [[nodiscard]] bool selectedSource(const Source& source) const {
      if (source.blocked) {
        return false;
      }
      if (source.monitor) {
        return m_selectedTab == Tab::Screen && m_selected.contains(source.id);
      }
      return m_selectedTab == Tab::App ? m_selected.contains(source.appId)
                                       : m_selectedTab == Tab::Window && m_selected.contains(source.id);
    }

    [[nodiscard]] std::vector<const Source*> visible() const {
      std::vector<const Source*> list;
      // App tab: one tile per app, standing for its first capturable window (a lock tile only when none is).
      std::map<std::string, std::size_t> apps;
      for (const Source& source : m_request.sources) {
        if (source.monitor != (m_tab == Tab::Screen)) {
          continue;
        }
        if (m_tab == Tab::App) {
          if (source.appId.empty()) {
            continue;
          }
          if (const auto [it, added] = apps.try_emplace(source.appId, list.size()); !added) {
            if (list[it->second]->blocked && !source.blocked) {
              list[it->second] = &source;
            }
            continue;
          }
        }
        list.push_back(&source);
      }
      return list;
    }

    void toggle(const Source& source) {
      if (source.blocked) {
        return;
      }
      if (m_selectedTab != m_tab || !m_request.multiple) {
        m_selected.clear();
      }
      m_selectedTab = m_tab;
      const std::string key = keyOf(source);
      if (m_request.multiple && m_selected.contains(key)) {
        m_selected.erase(key);
      } else {
        m_selected.insert(key);
      }
      m_window->requestRebuild();
    }

    std::unique_ptr<Node> header(float scale) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * scale});
      if (const std::string icon = appIconFile(m_request.appId); !icon.empty()) {
        row->addChild(std::make_unique<AppIconNode>(icon, Tokens::spaceP40 * scale));
      }
      auto text = ui::column({.align = FlexAlign::Start, .gap = 0.0F, .flexGrow = 1.0F});
      text->addChild(ui::label({.text = title(),
                                .fontSize = Tokens::typeTitle2.size * scale,
                                .fontWeight = FontWeight::SemiBold,
                                .color = colorSpecFromRole(ColorRole::Label1),
                                .maxLines = 1,
                                .ellipsize = TextEllipsize::End}));
      text->addChild(ui::label({.text = std::string("It sees only what you pick, until you stop."),
                                .fontSize = Tokens::typeBody.size * scale,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
      row->addChild(std::move(text));
      std::vector<ui::SegmentedOption> tabs;
      std::vector<Tab> order;
      if (m_request.windows) {
        tabs.push_back({.label = "Window"});
        order.push_back(Tab::Window);
        if (m_request.multiple) {
          tabs.push_back({.label = "App"});
          order.push_back(Tab::App);
        }
      }
      if (m_request.monitors) {
        tabs.push_back({.label = "Screen"});
        order.push_back(Tab::Screen);
      }
      if (tabs.size() > 1) {
        const auto current = static_cast<std::size_t>(std::distance(order.begin(), std::ranges::find(order, m_tab)));
        row->addChild(ui::segmented({.options = std::move(tabs),
                                     .selectedIndex = current,
                                     .scale = scale,
                                     .onChange = [this, order](std::size_t index) {
                                       m_tab = order.at(index);
                                       m_scrollState = {};
                                       m_window->requestRebuild();
                                     }}));
      }
      return row;
    }

    // A tile: the thumbnail inside a ring band (accent ring when selected), then the app icon and name.
    std::unique_ptr<Node> tile(Renderer& renderer, const Source& source, float width, float scale) {
      const bool on = !source.blocked && m_selectedTab == m_tab && m_selected.contains(keyOf(source));
      auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * scale, .width = width});
      auto frame = ui::column({.align = FlexAlign::Stretch,
                               .padding = kRingBand * scale,
                               .radius = (Tokens::radiusR4 + kRingBand) * scale,
                               .border = on ? std::optional(colorSpecFromRole(ColorRole::Accent)) : std::nullopt,
                               .borderWidth = Style::focusRingWidth * scale});
      auto thumb = ui::column({.align = FlexAlign::Center,
                               .justify = FlexJustify::Center,
                               .gap = Tokens::spaceP4 * scale,
                               .fill = colorSpecFromRole(ColorRole::Fill2),
                               .radius = Tokens::radiusR4 * scale,
                               .border = colorSpecFromRole(ColorRole::Stroke),
                               .borderWidth = 0.5F * scale,
                               .height = kThumbHeight * scale});
      if (source.blocked) {
        thumb->addChild(ui::glyph({.glyph = std::string("lock"),
                                   .glyphSize = Tokens::spaceP16 * scale,
                                   .color = colorSpecFromRole(ColorRole::Label2)}));
        thumb->addChild(ui::label({.text = std::string("Blocks capture"),
                                   .fontSize = Tokens::typeCaption.size * scale,
                                   .fontWeight = FontWeight::Medium,
                                   .color = colorSpecFromRole(ColorRole::Label2)}));
      } else {
        const auto index = static_cast<std::size_t>(&source - m_request.sources.data());
        thumb->addChild(ui::image({.out = &m_images[index],
                                   .fit = ImageFit::Contain,
                                   .radius = Tokens::radiusR4 * scale,
                                   .width = width - 2.0F * kRingBand * scale,
                                   .height = kThumbHeight * scale,
                                   .configure = [this, &renderer, index](Image& image) {
                                     if (m_textures[index].valid()) {
                                       image.setExternalTexture(renderer, m_textures[index]);
                                     }
                                   }}));
      }
      frame->addChild(std::move(thumb));
      body->addChild(std::move(frame));
      std::string name = source.title;
      if (m_tab == Tab::App) {
        const std::string app = appInfo(source.appId).name;
        name = app.empty() ? source.appId : app;
      }
      auto caption = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale, .paddingH = Tokens::spaceP4 * scale});
      if (const std::string& icon = appInfo(source.appId).icon; !source.monitor && !icon.empty()) {
        caption->addChild(std::make_unique<AppIconNode>(icon, Tokens::spaceP16 * scale));
      }
      auto text = ui::label({.text = name,
                             .fontSize = Tokens::typeFootnote.size * scale,
                             .fontWeight = FontWeight::Medium,
                             .color = colorSpecFromRole(source.blocked ? ColorRole::Label2 : ColorRole::Label1),
                             .maxLines = 1,
                             .textAlign = TextAlign::Start,
                             .ellipsize = TextEllipsize::End,
                             .configure = [scale](Label& label) { label.setLineHeight(Tokens::typeFootnote.line * scale); }});
      text->setFlexGrow(1.0F);
      caption->addChild(std::move(text));
      body->addChild(std::move(caption));
      const Source* target = &source;
      return ui::inputArea({.focusable = true,
                            .width = width,
                            .height = (kThumbHeight + 2.0F * kRingBand + Tokens::spaceP8 + Tokens::typeFootnote.line) * scale,
                            .onClick = [this, target](const InputArea::PointerData&) { toggle(*target); }},
                           std::move(body));
    }

    std::unique_ptr<Node> grid(Renderer& renderer, float scale) {
      constexpr int kColumns = 3;
      const float gap = Tokens::spaceP12 * scale;
      const float width = (static_cast<float>(kSheetWidth) * scale - 2.0F * Tokens::spaceP12 * scale - 2.0F * gap) / kColumns;
      auto scroll = ui::scrollView({.out = &m_scroll,
                                    .state = &m_scrollState,
                                    .contentScale = scale,
                                    .scrollbarAutoHide = true,
                                    .flexGrow = 1.0F,
                                    .onScrollChanged = [this](float) { updateShown(); }});
      Flex* rows = scroll->content();
      rows->setGap(gap);
      rows->setAlign(FlexAlign::Start);
      const auto list = visible();
      if (list.empty()) {
        rows->addChild(ui::label({.text = std::string(m_tab == Tab::Screen ? "No screens to share" : "No windows to share"),
                                  .fontSize = Tokens::typeBody.size * scale,
                                  .color = colorSpecFromRole(ColorRole::Label2)}));
      }
      std::unique_ptr<Flex> line;
      for (std::size_t i = 0; i < list.size(); ++i) {
        if (i % kColumns == 0) {
          if (line) {
            rows->addChild(std::move(line));
          }
          line = ui::row({.gap = gap});
        }
        line->addChild(tile(renderer, *list[i], width, scale));
      }
      if (line) {
        rows->addChild(std::move(line));
      }
      return scroll;
    }

    std::unique_ptr<Node> switchRow(const std::string& label, const std::string& detail, bool checked,
                                    std::function<void(bool)> change, float scale) {
      auto row = ui::row({.align = FlexAlign::Center,
                          .gap = Tokens::spaceP12 * scale,
                          .paddingV = Tokens::spaceP8 * scale,
                          .paddingH = Tokens::spaceP12 * scale,
                          .fill = colorSpecFromRole(ColorRole::GlassCard),
                          .radius = Tokens::radiusR8 * scale,
                          .border = colorSpecFromRole(ColorRole::GlassCardEdge),
                          .borderWidth = std::max(1.0F, 0.5F * scale),
                          .minHeight = Tokens::spaceP48 * scale,
                          .flexGrow = 1.0F});
      auto text = ui::column({.align = FlexAlign::Start, .gap = 0.0F, .flexGrow = 1.0F});
      text->addChild(ui::label({.text = label, .fontSize = Tokens::typeBody.size * scale, .fontWeight = FontWeight::Medium,
                                .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                                .ellipsize = TextEllipsize::End}));
      if (!detail.empty()) {
        text->addChild(ui::label({.text = detail, .fontSize = Tokens::typeFootnote.size * scale,
                                  .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2),
                                  .maxLines = 1, .ellipsize = TextEllipsize::End}));
      }
      row->addChild(std::move(text));
      row->addChild(ui::toggle({.checked = checked, .scale = scale, .onChange = std::move(change)}));
      return row;
    }

    std::unique_ptr<Node> options(float scale) {
      auto row = ui::row({.gap = Tokens::spaceP24 * scale});
      if (m_request.cursorOption) {
        row->addChild(switchRow("Show Pointer", {}, m_showPointer, [this](bool on) { m_showPointer = on; }, scale));
      }
      if (m_request.canRemember) {
        row->addChild(switchRow(m_name.empty() ? std::string("Remember for this app") : "Remember for " + m_name,
                                "Next time it starts sharing at once", m_remember,
                                [this](bool on) { m_remember = on; }, scale));
      }
      return row;
    }

    std::unique_ptr<Node> buttons(float scale) {
      auto row = ui::row({.gap = Tokens::spaceP8 * scale});
      row->addChild(ui::spacer());
      // System dialog footers: both buttons are capsules (rulebook 9.5).
      const float height = Style::controlHeight * scale;
      row->addChild(ui::button({.text = std::string("Cancel"),
                                .fontSize = Style::fontSizeBody * scale,
                                .paddingH = Tokens::spaceP16 * scale,
                                .radius = height / 2.0F,
                                .height = height,
                                .onClick = [this] { finish(false); }}));
      row->addChild(ui::button({.text = std::string("Share"),
                                .fontSize = Style::fontSizeBody * scale,
                                .enabled = canShare(),
                                .variant = ButtonVariant::Primary,
                                .paddingH = Tokens::spaceP16 * scale,
                                .radius = height / 2.0F,
                                .height = height,
                                .onClick = [this] { finish(true); }}));
      return row;
    }

    Request m_request;
    AppLoop& m_loop;
    std::string m_name;
    Tab m_tab = Tab::Window;
    Tab m_selectedTab = Tab::Window;
    std::set<std::string> m_selected;
    // Desktop-entry and icon lookups hit the disk: once per app id, not per rebuild.
    mutable std::map<std::string, AppInfo> m_apps;
    bool m_showPointer = false;
    bool m_remember = false;
    Answer m_answer = Answer::Pending;
    AppWindow* m_window = nullptr;
    ScrollView* m_scroll = nullptr;
    ScrollViewState m_scrollState;
    std::vector<Image*> m_images;            // per source; rebuilt with the tree
    std::vector<TextureHandle> m_textures;   // per source; the last live frame, owned here
    std::vector<std::size_t> m_shown;
    std::unique_ptr<xdpu::Thumbnails> m_thumbnails; // last: its worker joins before the members it posts to go
  };

  // One process per request: no single instance, so a second picker never forwards to the first.
  class SharePickerApp final : public DeskApp {
  public:
    explicit SharePickerApp(Request request)
        : DeskApp({.id = "dev.andromeda.SharePicker", .name = "Share", .singleInstance = false}),
          m_parentWindow(request.parentWindow), m_picker(std::move(request), loop()) {}
    ~SharePickerApp() override { destroyWindows(); }
    SharePickerApp(const SharePickerApp&) = delete;
    SharePickerApp& operator=(const SharePickerApp&) = delete;

    Picker& picker() { return m_picker; }

  protected:
    AppWindow& createWindow() override {
      AppWindow& window = addWindow({.title = m_picker.title(),
                                     .stateKey = "picker",
                                     .width = kSheetWidth,
                                     .height = kSheetHeight,
                                     .minWidth = kSheetWidth,
                                     .minHeight = kSheetHeight,
                                     .fixedSize = true,
                                     .parentWindow = m_parentWindow,
                                     .header = false});
      m_picker.attach(window);
      window.setContentBuilder(
          [this, &window](Renderer& renderer, float, float) { return m_picker.build(renderer, window.uiScale()); });
      window.setLayoutCallback([this] { m_picker.updateShown(); });
      window.setKeyHandler([this](const KeyboardEvent& event) { return m_picker.key(event); });
      return window;
    }

    // The window X cancels.
    void windowClosed(AppWindow& /*window*/) override { m_picker.finish(false); }

  private:
    std::string m_parentWindow;
    Picker m_picker;
  };

} // namespace

int main(int argc, char* argv[]) {
  std::string line;
  std::getline(std::cin, line);
  std::optional<Request> request = parseRequest(line);
  if (!request) {
    std::fprintf(stderr, "share-picker: invalid request\n");
    std::printf("{\"selections\":[]}\n");
    return 1;
  }
  SharePickerApp app(std::move(*request));
  const int code = app.run(argc, argv);
  app.picker().finish(false);
  return code;
}
