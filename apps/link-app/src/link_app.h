#pragma once

#include "app/desk_app.h"
#include "app/file_chooser.h"
#include "render/core/texture_manager.h"
#include "link_client.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/controls/virtual_list_view.h"

#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace linkapp {

  enum class Page : std::uint8_t { Waiting, Messages, Calls, Photos, Files, Activity, Mirror, Settings };

  // The Waiting feed as a virtual list: one row per item, built the same way to measure and to show.
  class FeedAdapter final : public VirtualListAdapter {
  public:
    struct Hooks {
      std::function<void(const std::string&)> textTo; // a missed call's "Text": opens a text to the number
      std::function<void()> openPhotos;               // a strip thumbnail clicked
      std::function<void(const std::string&, std::uint32_t)> dragPhoto; // a strip thumbnail dragged: phone path, press serial
    };
    FeedAdapter(LinkClient& client, const float& scale, Hooks hooks) : m_client(client), m_scale(scale), m_hooks(std::move(hooks)) {}
    [[nodiscard]] std::size_t itemCount() const override;
    [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override;
    [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return true; }
    [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;

  private:
    // renderer is null while measuring: the strip then loads no thumbnails.
    [[nodiscard]] std::unique_ptr<Node> build(Renderer* renderer, std::size_t index, float width);

    LinkClient& m_client;
    const float& m_scale;
    std::map<std::string, std::string> m_drafts; // reply text per item key, kept across row recycling
    Hooks m_hooks;
    struct Press {
      float x = 0.0F; // scene position
      float y = 0.0F;
      std::uint32_t serial = 0;
    };
    std::optional<Press> m_press; // a press on a strip thumbnail, until it drags
  };

  // Messages: the thread list, newest first; a click opens the thread.
  class ThreadAdapter final : public VirtualListAdapter {
  public:
    ThreadAdapter(LinkClient& client, const float& scale, const std::optional<std::uint64_t>& selected,
                  std::function<void(std::uint64_t)> open)
        : m_client(client), m_scale(scale), m_selected(selected), m_open(std::move(open)) {}
    [[nodiscard]] std::size_t itemCount() const override;
    [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override;
    [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override;
    [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return true; }
    [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;
    void onActivate(std::size_t index) override;

  private:
    LinkClient& m_client;
    const float& m_scale;
    const std::optional<std::uint64_t>& m_selected;
    std::function<void(std::uint64_t)> m_open;
  };

  // Messages: one conversation as day lines and bubbles, oldest first, anchored at the bottom.
  class ConversationAdapter final : public VirtualListAdapter {
  public:
    ConversationAdapter(LinkClient& client, const float& scale) : m_client(client), m_scale(scale) {}
    void setRows(std::vector<ConversationRow> rows) { m_rows = std::move(rows); }
    [[nodiscard]] std::size_t itemCount() const override { return m_rows.size(); }
    [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override { return m_rows[index].key; }
    [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override;
    [[nodiscard]] bool itemInteractive(std::size_t index) const override;
    [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;
    void onActivate(std::size_t index) override;

  private:
    // renderer null while measuring: an image bubble has a fixed size.
    [[nodiscard]] std::unique_ptr<Node> build(std::size_t index, float width, Renderer* renderer) const;

    LinkClient& m_client;
    const float& m_scale;
    std::vector<ConversationRow> m_rows;
  };

  // Calls: the phone's call log, newest first, grouped Today / Earlier.
  class CallAdapter final : public VirtualListAdapter {
  public:
    CallAdapter(LinkClient& client, const float& scale) : m_client(client), m_scale(scale) {}
    [[nodiscard]] std::size_t itemCount() const override { return m_client.calls().size(); }
    [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override;
    [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return true; }
    [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;

  private:
    [[nodiscard]] std::unique_ptr<Node> build(std::size_t index) const;

    LinkClient& m_client;
    const float& m_scale;
  };

  // Photos: the phone's photos and videos page by page under day headers; a click toggles the selection, a tile near
  // the end asks for the next page.
  class PhotoAdapter final : public VirtualGridAdapter {
  public:
    PhotoAdapter(LinkClient& client, const float& scale) : m_client(client), m_scale(scale) {}
    // The renderer of the window; thumbnails decode on bind.
    void setRenderer(Renderer* renderer) { m_renderer = renderer; }
    [[nodiscard]] std::size_t itemCount() const override { return m_client.photos().items().size(); }
    [[nodiscard]] std::unique_ptr<Node> createTile() override;
    void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override;
    void onActivate(std::size_t index) override { m_client.togglePhoto(index); }
    [[nodiscard]] std::string itemTooltip(std::size_t index) const override;
    [[nodiscard]] std::unique_ptr<Node> createSectionHeader() override;
    void bindSectionHeader(Node& header, std::size_t section, float width, float height) override;
    void setDays(std::vector<std::pair<std::size_t, std::string>> days) { m_days = std::move(days); }

  private:
    LinkClient& m_client;
    const float& m_scale;
    Renderer* m_renderer = nullptr;
    std::vector<std::pair<std::size_t, std::string>> m_days;
    std::map<const Node*, std::string> m_shown; // tile -> path of the thumbnail it shows
  };

  // Files: one phone folder, folders first; a click opens a folder, saves a file, or shows a saved one.
  class FileAdapter final : public VirtualListAdapter {
  public:
    FileAdapter(LinkClient& client, const float& scale) : m_client(client), m_scale(scale) {}
    [[nodiscard]] std::size_t itemCount() const override { return m_client.files().entries().size(); }
    [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override;
    [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return true; }
    [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;
    void onActivate(std::size_t index) override;

  private:
    LinkClient& m_client;
    const float& m_scale;
  };

  // Activity: transfers since the app started, newest first, with actions per state.
  class TransferAdapter final : public VirtualListAdapter {
  public:
    TransferAdapter(LinkClient& client, const float& scale) : m_client(client), m_scale(scale) {}
    [[nodiscard]] std::size_t itemCount() const override { return m_client.activity().items().size(); }
    [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override;
    [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return true; }
    [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;

  private:
    [[nodiscard]] std::unique_ptr<Node> build(std::size_t index) const;

    LinkClient& m_client;
    const float& m_scale;
  };

  class LinkApp final : public DeskApp {
  public:
    static constexpr const char* kAppId = "dev.andromeda.Link";
    LinkApp();
    ~LinkApp() override;

  protected:
    AppWindow& createWindow() override;
    // "page" with a page name ("messages") shows that page.
    void action(const std::string& name, const std::vector<std::string>& parameter, const std::string& activationToken) override;

  private:
    void show(Page page);
    void updateCaption();
    [[nodiscard]] std::unique_ptr<Node> sidebar(float width);
    [[nodiscard]] std::unique_ptr<Node> content(Renderer& renderer, float width, float height);
    // Pair a Phone: the QR and code of this app's pairing window.
    [[nodiscard]] std::unique_ptr<Node> pair(Renderer& renderer, float s);
    [[nodiscard]] std::unique_ptr<Node> messages(float width, float height);
    [[nodiscard]] std::unique_ptr<Node> compose(float s);
    [[nodiscard]] std::unique_ptr<Node> calls();
    [[nodiscard]] std::unique_ptr<Node> activity();
    [[nodiscard]] std::unique_ptr<Node> photos(Renderer& renderer, float width, float height);
    [[nodiscard]] std::unique_ptr<Node> files(float width, float height);
    [[nodiscard]] std::unique_ptr<Node> mirror(float width, float height);
    // Shows a decoded frame: a dmabuf frame is imported without a copy, an RGBA frame is uploaded into the one
    // mirror texture (same size: in place); then repaints the video.
    void showMirrorFrame(MirrorDecoder::Frame frame);
    // Frees the shown mirror frame (all = also the texture) once the GPU no longer samples it.
    void retireMirrorFrame(bool all);
    [[nodiscard]] std::string mirrorStat() const;
    [[nodiscard]] std::unique_ptr<Node> settings(const Presence& presence);
    [[nodiscard]] std::unique_ptr<Node> commands(const Presence& presence, float width, float height);
    void commandsChanged(bool changed);
    void commandGrip(std::size_t row, const InputArea::PointerData& data, bool motion);
    [[nodiscard]] std::unique_ptr<Node> card(std::vector<std::unique_ptr<Node>> caption, std::unique_ptr<Node> body,
                                             float width, bool windowButtons);
    // Cards mode and the viewed thread follow the page and presence.
    void sync();
    // Shows the hotspot sheet while the client holds an ask, and closes it when the ask ends.
    void syncHotspotSheet();
    void chooseAndSend(const std::string& deviceId);
    // Drags a phone photo out: saved into the cache, offered as a file URI once there.
    void dragPhoto(const std::string& path, std::uint32_t serial);
    // Shows this phone; the open page reloads its data from it.
    void choosePhone(const std::string& deviceId);
    // The pair view covers the page while this app pairs another phone (Pair Another Phone…).
    [[nodiscard]] bool pairShown() const;
    // Esc: cancels that pairing, else closes the Forget question.
    [[nodiscard]] bool dismissable() const;
    void dismiss();
    // The conversation card shows a thread or a new message, never both: these two are the only ways to change it.
    void openThread(std::optional<std::uint64_t> id);
    // A new message to `to` (name or number); an existing thread with that number opens instead.
    void openCompose(const std::string& to);

    std::unique_ptr<LinkClient> m_client;
    std::unique_ptr<FeedAdapter> m_feed;
    ScrollViewState m_feedScroll;
    std::unique_ptr<ThreadAdapter> m_threads;
    std::unique_ptr<ConversationAdapter> m_conversation;
    ScrollViewState m_threadScroll;
    ScrollViewState m_conversationScroll;
    std::optional<std::uint64_t> m_thread; // the selected thread
    struct Compose {
      std::string to;   // name or number as typed
      std::string body;
      std::string sent; // the number last texted, until the phone lists its thread
    };
    std::optional<Compose> m_compose; // set only while m_thread is empty
    std::map<std::uint64_t, std::string> m_threadDrafts;
    std::unique_ptr<CallAdapter> m_calls;
    ScrollViewState m_callScroll;
    std::unique_ptr<FileAdapter> m_files;
    ScrollViewState m_fileScroll;
    std::unique_ptr<PhotoAdapter> m_photos;
    ScrollViewState m_photoScroll;
    VirtualListView* m_feedList = nullptr;  // the Waiting list of the last build, null off Waiting
    VirtualListView* m_conversationList = nullptr; // the open conversation of the last build, null off Messages
    VirtualGridView* m_photoGrid = nullptr; // the grid of the last build, null off the Photos page
    std::unique_ptr<TransferAdapter> m_transfers;
    ScrollViewState m_transferScroll;
    ScrollViewState m_settingsScroll;
    bool m_confirmForget = false; // Settings: the Forget row asks in place
    enum class SettingsView : std::uint8_t { Main, Commands };
    SettingsView m_settingsView = SettingsView::Main;
    CommandsEditor m_commands;
    ScrollViewState m_commandsScroll;
    // Commands row drag: Pressed on a grip, Dragging past spaceP4; the release moves the row to `target`.
    // rows holds this build's row nodes; a rebuild waits while the button is down, so they live through a drag.
    struct CommandDrag {
      enum class Kind : std::uint8_t { Idle, Pressed, Dragging };
      Kind kind = Kind::Idle;
      std::size_t from = 0;
      Command moving; // the dragged row's command at the press
      std::size_t target = 0;
      float startY = 0.0F;
      std::vector<Flex*> rows;
      std::vector<float> tops;           // scene top of each row at the press
      std::vector<float> shift;          // each other row's translation target
      std::vector<std::uint32_t> springs; // AnimationManager ids per row
      ColorSpec restFill;                 // the dragged row's fill before the drag
    };
    CommandDrag m_commandDrag;
    std::string m_hotspotSheet;   // device id + ssid of the ask the sheet shows; empty while none shows
    std::string m_dial; // the "Name or number" field
    AppWindow* m_window = nullptr;
    std::unique_ptr<desk::FileChooser> m_chooser;
    Page m_page = Page::Waiting;
    float m_scale = 1.0F;
    Renderer* m_renderer = nullptr;   // the window's, from the last content build
    TextureHandle m_mirrorTexture;    // owned here; the video Image shows it without owning it
    std::shared_ptr<void> m_mirrorHold; // non-null when m_mirrorTexture wraps a decoder dmabuf
    struct RetiredFrame {
      std::shared_ptr<void> hold;
      int fence = -1; // signals once no submitted paint samples the frame; -1: already done
    };
    std::vector<RetiredFrame> m_mirrorRetired;
    Image* m_mirrorImage = nullptr;   // of the last build, null off the Mirror page
    Label* m_mirrorStat = nullptr;
    bool m_touching = false;          // a primary press on the video is down
  };

} // namespace linkapp
