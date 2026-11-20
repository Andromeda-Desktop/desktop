#pragma once

#include "app/desk_app.h"
#include "link_client.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_list_view.h"

#include <map>
#include <memory>
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

  class LinkApp final : public DeskApp {
  public:
    static constexpr const char* kAppId = "dev.andromeda.Link";
    LinkApp();
    ~LinkApp() override;

  protected:
    AppWindow& createWindow() override;

  private:
    void show(Page page);
    void updateCaption();
    [[nodiscard]] std::unique_ptr<Node> sidebar(float width);
    [[nodiscard]] std::unique_ptr<Node> content(float width, float height);

    std::unique_ptr<LinkClient> m_client;
    std::unique_ptr<FeedAdapter> m_feed;
    ScrollViewState m_feedScroll;
    AppWindow* m_window = nullptr;
    Page m_page = Page::Waiting;
    float m_scale = 1.0F;
  };

} // namespace linkapp
