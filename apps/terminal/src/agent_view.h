#pragma once

#include "agent_session.h"
#include "content_view.h"
#include "ui/controls/virtual_list_view.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>

class Button;
class Box;
class Flex;
class Input;
class Label;

namespace term::agent {
  class Host;
}

namespace term {

  // The content plane of an agent session: the log of turns, replies, tool calls and requests above a composer.
  class AgentView final : public ContentView, private VirtualListAdapter {
  public:
    AgentView(agent::Host& host, std::uint64_t key);
    ~AgentView() override;
    AgentView(const AgentView&) = delete;
    AgentView& operator=(const AgentView&) = delete;

    // The session model changed: rebinds the log and the composer.
    void changed();
    [[nodiscard]] std::uint64_t key() const noexcept { return m_key; }
    // Null after the agent session closed.
    [[nodiscard]] const agent::Session* session() const;
    [[nodiscard]] AgentView* agent() noexcept override { return this; }
    // Title band pills: the plan (once the agent sent one; onPlan opens it under the pill) and the status.
    // status receives the status pill's label: statusText() refreshes it in place while ticking().
    [[nodiscard]] std::unique_ptr<Node> band(float scale, std::function<void(const Node* anchor)> onPlan,
                                             Label** status) const;
    // "working · 4:12", "done · 6:58", "ready", ...
    [[nodiscard]] std::string statusText() const;
    // A turn runs: the status clock advances each second.
    [[nodiscard]] bool ticking() const;
    // Advances the elapsed time of the running actions in view, in place: a rebind restarts their spinners.
    void tick();
    // The plan popover: content and height in logical px before scale.
    [[nodiscard]] std::unique_ptr<Node> planList(float scale) const;
    [[nodiscard]] float planHeight() const;

    std::unique_ptr<Node> build(Renderer& renderer, float uiScale, float deviceScale, float width,
                                float height) override;
    void setSize(float width, float height) override;
    void frame() override {}
    void sceneDestroyed() override;
    void setDeviceScale(float /*deviceScale*/) override {}
    // 1-4 answer the oldest open request while the composer is empty; Esc stops the turn.
    bool key(const KeyboardEvent& event) override;
    void setWindowActive(bool /*active*/) override {}
    [[nodiscard]] InputArea* focusArea() const noexcept override;

  private:
    [[nodiscard]] std::size_t itemCount() const override;
    [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override;
    [[nodiscard]] bool itemInteractive(std::size_t index) const override;
    [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;

    [[nodiscard]] bool thinking() const;
    [[nodiscard]] bool streamingAt(std::size_t entry) const;
    // elapsed receives a running action's elapsed-time label.
    [[nodiscard]] std::unique_ptr<Node> buildItem(std::size_t index, float width, Label** elapsed);
    // A pending request asks about this action: the card replaces its row.
    [[nodiscard]] bool asked(const agent::Action& action) const;
    void arrive(Renderer& renderer, Flex& holder, float width);
    [[nodiscard]] std::unique_ptr<Node> buildRequest(const agent::Request& request, std::size_t entry);
    void answer(std::size_t option);
    void submit(const std::string& text);
    void syncComposer();
    void syncLog();
    void syncJump();
    void jumpToEnd();
    [[nodiscard]] std::vector<std::string> slashMatches() const;
    void syncSlash();
    [[nodiscard]] bool atBottom() const;

    agent::Host& m_host;
    std::uint64_t m_key;
    float m_scale = 1.0F;
    float m_lastWidth = 0.0F; // the width of the item being built
    Flex* m_root = nullptr;
    Flex* m_composer = nullptr;
    Flex* m_jump = nullptr;  // holds the jump pill while it shows
    Box* m_jumpShadow = nullptr;
    std::size_t m_seen = 0;  // items the user has seen at the bottom of the log
    Flex* m_slash = nullptr; // the slash command menu over the composer
    std::size_t m_slashSelected = 0;
    std::string m_slashDismissed; // the composer text Esc closed the menu at
    VirtualListView* m_log = nullptr;
    // Elapsed-time labels of the running actions bound into log slots; a slot rebind drops its entry.
    struct ActionClock {
      const Node* holder;
      Label* label;
      agent::WallTime started;
    };
    std::vector<ActionClock> m_clocks;
    std::set<std::size_t> m_arrived; // request entries whose card has played its arrival
    Input* m_input = nullptr;
    Label* m_meta = nullptr;
    Button* m_send = nullptr;
    Button* m_stop = nullptr;
  };

} // namespace term
