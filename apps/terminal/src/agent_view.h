#pragma once

#include "agent_session.h"
#include "content_view.h"
#include "ui/controls/virtual_list_view.h"

#include <cstdint>
#include <functional>
#include <map>
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
    // Opens a file: URI (the edit card's Open link).
    void setOpenUri(std::function<void(const std::string& uri)> open) { m_openUri = std::move(open); }
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
    // The sidebar row's line while a turn runs: "Run · meson test", "replying", "thinking", "working"; else empty.
    [[nodiscard]] std::string activity() const;
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
    [[nodiscard]] float itemHeightOverride(std::size_t index, float measured) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;

    [[nodiscard]] bool thinking() const;
    [[nodiscard]] bool streamingAt(std::size_t entry) const;
    // An elapsed-time label in a log slot (a running action, the thinking row): tick() rewrites it in place as
    // prefix + time + suffix; a slot rebind drops its entry.
    struct Clock {
      const Node* holder = nullptr;
      Label* label = nullptr;
      agent::WallTime started;
      std::string prefix;
      std::string suffix;
    };
    // clock receives the item's elapsed-time label, if it has one.
    [[nodiscard]] std::unique_ptr<Node> buildItem(std::size_t index, float width, Clock* clock);
    // A pending request asks about this action: the card replaces its row.
    [[nodiscard]] bool asked(const agent::Action& action) const;
    // The entry is the crash that ended the session, which is still Failed: its row offers Restart.
    [[nodiscard]] bool restartable(std::size_t entry) const;
    void toggleDetail(const std::string& id); // an action row click opens or closes its diff or output
    [[nodiscard]] const agent::Request* requestFor(const agent::Action& action) const;
    // A request card's list item opens from its header row and carries the log above; an answer folds the item from
    // card height to the row that carries the answer. Height on spring.move, the new content fades in on spring.fade
    // past the content gate. The springs belong to the log, so rebinds keep them; bindItem reapplies the state.
    enum class CardMotion : std::uint8_t { Opening, FoldQueued, Folding }; // FoldQueued waits for the row's height
    struct Motion {
      CardMotion phase = CardMotion::Opening;
      float height = 0.0F;  // the item height in px
      float opacity = 0.0F; // of the card body (Opening) or the row (Folding)
      Flex* holder = nullptr; // the slot bound to the item, if any
      int springs = 0;        // live springs; the motion ends when the last one ends
      std::uint32_t move = 0;
      std::uint32_t fade = 0;
    };
    void startMotion(std::size_t index, float from, float to);
    void applyMotion(const Motion& motion) const;
    void endSpring(std::size_t index);
    void cancelMotion(std::size_t index);
    [[nodiscard]] std::unique_ptr<Node> buildRequest(const agent::Request& request, std::size_t entry);
    void answer(std::size_t option);
    void submit(const std::string& text);
    void syncComposer();
    void syncLog();
    void syncJump();
    void jumpToEnd();
    [[nodiscard]] std::vector<agent::Session::Command> slashMatches() const;
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
    std::vector<Clock> m_clocks;
    std::map<std::size_t, float> m_arrived; // open request items whose card has arrived -> the card item height
    std::map<std::size_t, Motion> m_motions; // by list item index
    std::function<void(const std::string& uri)> m_openUri;
    std::set<std::string> m_expanded; // action ids whose detail the user opened
    Input* m_input = nullptr;
    Label* m_meta = nullptr;
    Button* m_send = nullptr;
    Button* m_stop = nullptr;
  };

} // namespace term
