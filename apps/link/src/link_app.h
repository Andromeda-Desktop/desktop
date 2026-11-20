#pragma once

#include "app/desk_app.h"
#include "link_client.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_list_view.h"

#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace linkapp {

  enum class Page : std::uint8_t { Waiting, Messages, Calls, Photos, Files, Activity, Mirror, Settings };

  // The Waiting feed as a virtual list: one row per item, built the same way to measure and to show.
  class FeedAdapter final : public VirtualListAdapter {
  public:
    FeedAdapter(LinkClient& client, const float& scale) : m_client(client), m_scale(scale) {}
    [[nodiscard]] std::size_t itemCount() const override;
    [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override;
    [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return true; }
    [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;

  private:
    [[nodiscard]] std::unique_ptr<Node> build(std::size_t index, float width);

    LinkClient& m_client;
    const float& m_scale;
    std::map<std::string, std::string> m_drafts; // reply text per item key, kept across row recycling
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
    [[nodiscard]] std::unique_ptr<Node> build(std::size_t index, float width) const;

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
    [[nodiscard]] std::unique_ptr<Node> calls();
    [[nodiscard]] std::unique_ptr<Node> activity();
    [[nodiscard]] std::unique_ptr<Node> settings(const Presence& presence);
    [[nodiscard]] std::unique_ptr<Node> card(std::vector<std::unique_ptr<Node>> caption, std::unique_ptr<Node> body,
                                             float width, bool windowButtons);
    // Cards mode and the viewed thread follow the page and presence.
    void sync();

    std::unique_ptr<LinkClient> m_client;
    std::unique_ptr<FeedAdapter> m_feed;
    ScrollViewState m_feedScroll;
    std::unique_ptr<ThreadAdapter> m_threads;
    std::unique_ptr<ConversationAdapter> m_conversation;
    ScrollViewState m_threadScroll;
    ScrollViewState m_conversationScroll;
    std::optional<std::uint64_t> m_thread; // the selected thread
    std::map<std::uint64_t, std::string> m_threadDrafts;
    std::unique_ptr<CallAdapter> m_calls;
    ScrollViewState m_callScroll;
    std::unique_ptr<TransferAdapter> m_transfers;
    ScrollViewState m_transferScroll;
    ScrollViewState m_settingsScroll;
    bool m_confirmForget = false; // Settings: the Forget row asks in place
    std::string m_dial; // the "Name or number" field
    AppWindow* m_window = nullptr;
    Page m_page = Page::Waiting;
    float m_scale = 1.0F;
  };

} // namespace linkapp
