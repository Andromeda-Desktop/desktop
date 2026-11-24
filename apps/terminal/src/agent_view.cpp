#include "agent_view.h"

#include "i18n/i18n.h"
#include "agent_host.h"
#include "agent_markup.h"
#include "core/input/cursor_shape.h"
#include "link_finder.h"
#include "core/input/key_modifiers.h"
#include "platform/input_events.h"
#include "design_tokens.h"
#include "render/animation/animation_manager.h"
#include "render/core/render_styles.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "core/text_clipboard.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/pinned.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/spinner.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <string_view>

namespace term {

  namespace {

    // "4:12", or "1:04:12" past an hour.
    std::string clockText(std::chrono::seconds elapsed) {
      const auto n = std::max<std::int64_t>(0, elapsed.count());
      return n >= 3600 ? std::format("{}:{:02}:{:02}", n / 3600, n / 60 % 60, n % 60) : std::format("{}:{:02}", n / 60, n % 60);
    }

    // A running action: "41 s", then "1:05".
    std::string elapsedText(agent::WallTime started) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - started);
      return elapsed.count() < 60 ? i18n::tr("terminal.agent.seconds", "count", std::max<std::int64_t>(0, elapsed.count())) : clockText(elapsed);
    }

    std::string agoText(agent::WallTime at) {
      const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now() - at).count();
      if (minutes < 1) {
        return i18n::tr("terminal.agent.now");
      }
      if (minutes < 60) {
        return i18n::tr("terminal.agent.minutes-ago", "count", minutes);
      }
      return minutes < 24 * 60 ? i18n::tr("terminal.agent.hours-ago", "count", minutes / 60) : i18n::tr("terminal.agent.days-ago", "count", minutes / (24 * 60));
    }

    const agent::Turn* lastTurn(const agent::Session& model) {
      for (auto it = model.entries().rbegin(); it != model.entries().rend(); ++it) {
        if (const auto* turn = std::get_if<agent::Turn>(&*it)) {
          return turn;
        }
      }
      return nullptr;
    }

    constexpr std::uint32_t kEscape = 0xff1b;
    constexpr std::uint32_t kEnd = 0xff57;
    constexpr std::uint32_t kTab = 0xff09;
    constexpr std::uint32_t kShiftTab = 0xfe20; // ISO_Left_Tab
    constexpr std::uint32_t kReturn = 0xff0d;
    constexpr std::uint32_t kUp = 0xff52;
    constexpr std::uint32_t kDown = 0xff54;

    // The content plane: log over composer, and the jump pill floating centred just above the composer.
    class AgentRoot final : public Flex {
    public:
      Node* jump = nullptr;
      Node* jumpShadow = nullptr; // e2 key shadow under the jump pill
      Node* slash = nullptr;
      Node* dock = nullptr;
      Node* fade = nullptr; // over the log's top edge, under the title band
      float scale = 1.0F;

    protected:
      void doLayout(Renderer& renderer) override {
        Flex::doLayout(renderer);
        place(renderer);
      }
      void doArrange(Renderer& renderer, const LayoutRect& rect) override {
        Flex::doArrange(renderer, rect);
        place(renderer);
      }

    private:
      static void float_(Renderer& renderer, Node& node, float x, float y, float w, float h) {
        node.arrange(renderer, {.x = x, .y = y, .width = w, .height = h});
      }
      void place(Renderer& renderer) {
        if (dock == nullptr) {
          return;
        }
        if (fade != nullptr) {
          float_(renderer, *fade, 0.0F, 0.0F, width(), Tokens::spaceP16 * scale);
        }
        if (jump != nullptr && jump->visible()) {
          const LayoutSize size = jump->measure(renderer, {});
          // The dock's top padding is the 16 gap between the pill and the composer.
          const float x = (width() - size.width) / 2.0F;
          const float y = dock->y() - size.height;
          float_(renderer, *jump, x, y, size.width, size.height);
          float_(renderer, *jumpShadow, x, y + Tokens::elevationE2.keyY * scale, size.width, size.height);
        }
        if (slash != nullptr && slash->visible()) {
          const float inset = Tokens::spaceP16 * scale;
          const float w = std::min(Tokens::componentAgentSlashWidth * scale, width() - 2.0F * inset);
          const LayoutSize size = slash->measure(renderer, {.minWidth = w, .maxWidth = w});
          float_(renderer, *slash, inset, dock->y() + inset - Tokens::componentPopoverOffset * scale - size.height, w,
                 size.height);
        }
      }
    };

    std::string mono() { return std::string(Tokens::fontMono); }

    std::vector<std::string_view> lines(std::string_view text) {
      std::vector<std::string_view> out;
      std::size_t pos = 0;
      while (pos < text.size()) {
        const auto eol = text.find('\n', pos);
        out.push_back(text.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos));
        pos = eol == std::string_view::npos ? text.size() : eol + 1;
      }
      return out;
    }

    struct Verb {
      std::string_view glyph;
      std::string word;
    };

    // The command, else the first path (relative to the session folder when inside it), else the title.
    std::string actionTarget(const agent::Action& action, std::string_view cwd) {
      if (!action.command.empty()) {
        return action.command;
      }
      if (action.kind == "search" && !action.title.empty()) {
        // "pattern in /abs/dir": the folder reads relative to the session folder.
        std::string title = action.title;
        const std::string prefix = std::string(cwd) + "/";
        for (auto at = title.find(prefix); !cwd.empty() && at != std::string::npos; at = title.find(prefix, at)) {
          title.erase(at, prefix.size());
        }
        return title;
      }
      if (action.paths.empty()) {
        return action.title;
      }
      std::string_view path = action.paths.front();
      if (!cwd.empty() && path.starts_with(cwd) && path.size() > cwd.size() && path[cwd.size()] == '/') {
        path.remove_prefix(cwd.size() + 1);
      }
      return std::string(path);
    }

    Verb verbFor(std::string_view kind) {
      if (kind == "read") {
        return {"file-text", i18n::tr("terminal.agent.verb-read")};
      }
      if (kind == "search") {
        return {"search", i18n::tr("terminal.agent.verb-search")};
      }
      if (kind == "edit" || kind == "delete" || kind == "move") {
        return {"pencil", kind == "edit" ? i18n::tr("terminal.agent.verb-edit") : kind == "delete" ? i18n::tr("terminal.agent.verb-delete") : i18n::tr("terminal.agent.verb-move")};
      }
      if (kind == "execute") {
        return {"terminal", i18n::tr("terminal.agent.verb-run")};
      }
      if (kind == "fetch") {
        return {"world", i18n::tr("terminal.agent.verb-fetch")};
      }
      if (kind == "think") {
        return {"bulb", i18n::tr("terminal.agent.verb-think")};
      }
      return {"sparkles", i18n::tr("terminal.agent.verb-tool")};
    }

    // The first hunk of a whole-file edit: the changed middle after the common leading and trailing lines.
    struct Hunk {
      std::size_t firstLine = 0; // 1-based line of the first change
      std::vector<std::string_view> removed;
      std::vector<std::string_view> added;
    };

    // start: the 1-based file line where the diff's old text begins.
    Hunk firstHunk(const agent::DiffHunk& diff, std::size_t start = 1) {
      const auto before = diff.oldText ? lines(*diff.oldText) : std::vector<std::string_view>{};
      const auto after = lines(diff.newText);
      std::size_t head = 0;
      while (head < before.size() && head < after.size() && before[head] == after[head]) {
        ++head;
      }
      std::size_t tail = 0;
      while (tail < before.size() - head && tail < after.size() - head
             && before[before.size() - 1 - tail] == after[after.size() - 1 - tail]) {
        ++tail;
      }
      return {.firstLine = start + head,
              .removed = {before.begin() + static_cast<std::ptrdiff_t>(head), before.end() - static_cast<std::ptrdiff_t>(tail)},
              .added = {after.begin() + static_cast<std::ptrdiff_t>(head), after.end() - static_cast<std::ptrdiff_t>(tail)}};
    }

    // A finished action's run time: "0.3 s", "41 s", then "1:05".
    std::string durationText(const agent::Action& action) {
      const double seconds = std::chrono::duration<double>(action.ended - action.started).count();
      if (seconds < 10.0) {
        return i18n::tr("terminal.agent.seconds-fraction", "count", std::format("{:.1f}", std::max(0.0, seconds)));
      }
      return seconds < 60.0 ? i18n::tr("terminal.agent.seconds", "count", static_cast<int>(seconds))
                            : clockText(std::chrono::seconds(static_cast<std::int64_t>(seconds)));
    }

    // The first number after `word` in a tool's output ("Exit code 1", "Found 38 files").
    std::optional<std::string> numberAfter(std::string_view output, std::string_view word) {
      const auto at = output.find(word);
      if (at == std::string_view::npos) {
        return std::nullopt;
      }
      std::size_t i = at + word.size();
      const std::size_t from = i;
      while (i < output.size() && std::isdigit(static_cast<unsigned char>(output[i])) != 0) {
        ++i;
      }
      return i > from ? std::optional<std::string>(output.substr(from, i - from)) : std::nullopt;
    }

    // What a finished action's row ends with, before any answer prefix: counts, hits or run time.
    std::string doneCaption(const agent::Action& action) {
      if (action.kind == "execute") {
        return durationText(action);
      }
      if (action.kind == "search") {
        if (auto found = numberAfter(action.output, "Found "); found) {
          return *found + " hits";
        }
      }
      return {};
    }

    // An action whose row opens a detail on click: its diff or its output.
    bool hasDetail(const agent::Action& action) {
      return (action.status == agent::ActionStatus::Done && !action.diffs.empty())
             || ((action.status == agent::ActionStatus::Done || action.status == agent::ActionStatus::Failed) && !action.output.empty());
    }

    // Builders share one scale: every value is a token times the window's ui scale.
    struct Kit {
      float s;

      std::unique_ptr<Label> text(std::string value, const Tokens::Type& type, ColorRole role,
                                  FontWeight weight = FontWeight::Normal, std::optional<float> maxWidth = std::nullopt,
                                  bool markup = false) const {
        return ui::label({.text = std::move(value),
                          .fontSize = type.size * s,
                          .fontWeight = weight,
                          .color = colorSpecFromRole(role),
                          .maxWidth = maxWidth,
                          .configure = [markup](Label& label) { label.setUseMarkup(markup); }});
      }

      std::unique_ptr<Label> code(std::string value, ColorRole role, int maxLines = 0, bool ellipsize = true) const {
        return ui::label({.text = std::move(value),
                          .fontSize = Tokens::typeFootnote.size * s,
                          .fontFamily = mono(),
                          .color = colorSpecFromRole(role),
                          .maxLines = maxLines,
                          .ellipsize = maxLines > 0 && ellipsize ? std::optional(TextEllipsize::End) : std::nullopt});
      }

      std::unique_ptr<Glyph> glyph(std::string_view name, ColorRole role) const {
        return ui::glyph({.glyph = std::string(name), .glyphSize = Tokens::sizeSymbol * s, .color = colorSpecFromRole(role)});
      }

      std::unique_ptr<Spinner> spinner() const {
        auto spin = ui::spinner({.color = colorSpecFromRole(ColorRole::AccentText), .spinnerSize = Tokens::sizeSymbol * s});
        spin->start();
        return spin;
      }

      // Column-major so each column takes its widest cell: all cells in a row share one height, so rows line up and the
      // separators meet.
      std::unique_ptr<Node> table(const std::vector<std::vector<std::string>>& rows, const std::vector<bool>& alignEnd) const {
        auto grid = ui::row({.align = FlexAlign::Start});
        for (std::size_t c = 0; c < rows.front().size(); ++c) {
          auto column = ui::column({.align = FlexAlign::Stretch});
          for (std::size_t r = 0; r < rows.size(); ++r) {
            if (r > 0) {
              column->addChild(ui::column({.fill = colorSpecFromRole(ColorRole::Separator), .height = 1.0F}));
            }
            auto cell = ui::row({.align = FlexAlign::Center,
                                 .justify = alignEnd[c] ? FlexJustify::End : FlexJustify::Start,
                                 .paddingH = Tokens::spaceP12 * s,
                                 .height = (r == 0 ? Tokens::componentListTableHeader : Tokens::sizeRowCompact) * s});
            cell->addChild(ui::label({.text = rows[r][c],
                                      .fontSize = (r == 0 ? Tokens::typeFootnote : Tokens::typeParagraph).size * s,
                                      .fontWeight = r == 0 ? FontWeight::Medium : FontWeight::Normal,
                                      .fontFeatures = alignEnd[c] ? std::optional<std::string>("tnum") : std::nullopt,
                                      .color = colorSpecFromRole(r == 0 ? ColorRole::Label2 : ColorRole::Label1),
                                      .maxLines = 1,
                                      .configure = [](Label& label) { label.setUseMarkup(true); }}));
            column->addChild(std::move(cell));
          }
          grid->addChild(std::move(column));
        }
        return sideways(std::move(grid));
      }

      enum class Overflow : std::uint8_t { Ellipsize, Scroll, Wrap };
      // A mono block on fill_1: output tails and error wells end wide lines in an ellipsis, code blocks in replies
      // scroll sideways, command wells wrap anywhere in mono 13 500.
      std::unique_ptr<Flex> well(std::vector<std::string_view> rows, ColorRole role, bool lastStrong,
                                 Overflow overflow = Overflow::Ellipsize) const {
        const bool scroll = overflow == Overflow::Scroll;
        auto box = ui::column({.align = FlexAlign::Stretch,
                               .paddingV = Tokens::spaceP8 * s,
                               .paddingH = scroll ? 0.0F : Tokens::spaceP12 * s,
                               .fill = colorSpecFromRole(ColorRole::Fill1),
                               .radius = Style::scaledRadius(Tokens::radiusR8, s)});
        auto text = ui::column({.align = FlexAlign::Start, .paddingH = scroll ? Tokens::spaceP12 * s : 0.0F});
        if (scroll) {
          text->setPadding(0.0F, Tokens::spaceP40 * s, 0.0F, Tokens::spaceP12 * s); // clear of the copy button
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
          const bool strong = lastStrong && i + 1 == rows.size();
          if (overflow == Overflow::Wrap) {
            text->addChild(ui::label({.text = std::string(rows[i]),
                                      .fontSize = Tokens::typeMono.size * s,
                                      .fontWeight = FontWeight::Medium,
                                      .fontFamily = mono(),
                                      .color = colorSpecFromRole(strong ? ColorRole::Label1 : role)}));
            continue;
          }
          text->addChild(code(std::string(rows[i]), strong ? ColorRole::Label1 : role, 1, !scroll));
        }
        if (!scroll) {
          text->setAlign(FlexAlign::Stretch);
          box->addChild(std::move(text));
          return box;
        }
        box->addChild(sideways(std::move(text)));
        return box;
      }

      // Content wider than the reply column scrolls sideways.
      std::unique_ptr<ScrollView> sideways(std::unique_ptr<Node> content) const {
        auto scroll = ui::scrollView({.contentScale = s,
                                      .viewportPaddingH = 0.0F,
                                      .viewportPaddingV = 0.0F,
                                      .fillWidth = true,
                                      .configure = [](ScrollView& view) { view.setOrientation(ScrollOrientation::Horizontal); }});
        scroll->content()->addChild(std::move(content));
        return scroll;
      }

      // "+n −m · N hunks": additions term_green, removals term_red; a zero side is left out.
      std::unique_ptr<Flex> counts(const agent::Action& action) const {
        std::size_t added = 0;
        std::size_t removed = 0;
        for (const auto& d : action.diffs) {
          const Hunk hunk = firstHunk(d);
          added += hunk.added.size();
          removed += hunk.removed.size();
        }
        auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
        if (added > 0 || removed == 0) {
          row->addChild(text(std::format("+{}", added), Tokens::typeFootnote, ColorRole::TermGreen));
        }
        if (removed > 0) {
          row->addChild(text(std::format("\u2212{}", removed), Tokens::typeFootnote, ColorRole::TermRed));
        }
        if (action.diffs.size() > 1) {
          row->addChild(text(i18n::tr("terminal.agent.hunks", "count", action.diffs.size()), Tokens::typeFootnote, ColorRole::Label2));
        }
        return row;
      }

      // width: the box width; lines ellipsize inside it.
      std::unique_ptr<Flex> diff(const Hunk& hunk, float width) const {
        constexpr std::size_t kMaxLines = 12;
        auto box = ui::column({.align = FlexAlign::Stretch,
                               .padding = Tokens::spaceP8 * s,
                               .radius = Style::scaledRadius(Tokens::radiusR8, s),
                               .border = colorSpecFromRole(ColorRole::Stroke),
                               .borderWidth = 0.5F * s,
                               .width = width});
        box->addChild(code(std::to_string(hunk.firstLine), ColorRole::Label2, 1));
        std::size_t shown = 0;
        const auto add = [&](std::string_view sign, std::string_view line, ColorRole fill) {
          if (shown++ >= kMaxLines) {
            return;
          }
          auto row = ui::row({.fill = colorSpecFromRole(fill), .fillWidth = true});
          auto text = code(std::string(sign) + "  " + std::string(line), ColorRole::Label1, 1);
          text->setMaxWidth(std::max(0.0F, width - 2.0F * Tokens::spaceP8 * s));
          row->addChild(std::move(text));
          box->addChild(std::move(row));
        };
        for (const auto line : hunk.removed) {
          add("-", line, ColorRole::DiffDel);
        }
        for (const auto line : hunk.added) {
          add("+", line, ColorRole::DiffAdd);
        }
        return box;
      }
    };

    enum class Answer : std::uint8_t { None, AllowedOnce, AlwaysAllowed, Rejected, Cancelled };

    Answer answerOf(const agent::Request& request) {
      if (!request.answer) {
        return Answer::None;
      }
      const auto it = std::ranges::find(request.options, *request.answer, &agent::Option::id);
      if (it == request.options.end()) {
        return Answer::Cancelled; // the answer is an option id or "cancelled"
      }
      return it->kind == "allow_once" ? Answer::AllowedOnce : it->kind == "allow_always" ? Answer::AlwaysAllowed : Answer::Rejected;
    }

    std::string answerWord(Answer answer) {
      switch (answer) {
      case Answer::None: return {};
      case Answer::AllowedOnce: return i18n::tr("terminal.agent.allowed-once");
      case Answer::AlwaysAllowed: return i18n::tr("terminal.agent.always-allowed");
      case Answer::Rejected: return i18n::tr("terminal.agent.rejected");
      case Answer::Cancelled: return i18n::tr("terminal.agent.cancelled");
      }
      return {};
    }

  } // namespace

  namespace {

    constexpr float kPlanWidth = 360.0F;
    constexpr float kPlanPillWidth = 280.0F; // mockup .pill max-width
    constexpr float kPlanRowGap = 4.0F;

    // "N of M": N is the step in progress (done steps + 1), or the done count when none runs.
    std::string planCount(const std::vector<agent::PlanEntry>& plan) {
      const auto done = std::ranges::count(plan, std::string("completed"), &agent::PlanEntry::status);
      const bool running = std::ranges::contains(plan, std::string("in_progress"), &agent::PlanEntry::status);
      return i18n::tr("terminal.agent.plan-progress", i18n::tr("terminal.agent.row-done"), done + (running ? 1 : 0), "total", plan.size());
    }

    // The title band: the status pill never shrinks; the agent pill hides first, then the plan pill reads "N of M"
    // instead of "Plan N of M · current", then hides. It measures at its floor (status and "N of M"); keep() is
    // status and the full plan, which the header holds while the title ellipsizes.
    class BandRow final : public Flex {
    public:
      Node* agent = nullptr;
      Node* status = nullptr;
      Node* full = nullptr;
      Node* compact = nullptr;

      float keep(Renderer& renderer) {
        return status->measure(renderer, {}).width + (full != nullptr ? gap() + full->measure(renderer, {}).width : 0.0F);
      }

    protected:
      LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
        const LayoutSize size = Flex::doMeasure(renderer, constraints);
        float w = status->measure(renderer, {}).width;
        if (compact != nullptr) {
          w += gap() + compact->measure(renderer, {}).width;
        }
        return {.width = std::max(w, constraints.minWidth), .height = size.height};
      }
      void doLayout(Renderer& renderer) override {
        choose(renderer, width());
        Flex::doLayout(renderer);
      }
      void doArrange(Renderer& renderer, const LayoutRect& rect) override {
        choose(renderer, rect.width);
        Flex::doArrange(renderer, rect);
      }

    private:
      void choose(Renderer& renderer, float w) {
        float room = w - status->measure(renderer, {}).width;
        bool showFull = true;
        if (full != nullptr) {
          const float fullW = full->measure(renderer, {}).width + gap();
          showFull = fullW <= room;
          show(full, showFull);
          show(compact, !showFull && compact->measure(renderer, {}).width + gap() <= room);
          room -= showFull ? fullW : 0.0F;
        }
        if (agent != nullptr) {
          show(agent, showFull && agent->measure(renderer, {}).width + gap() <= room);
        }
      }
      static void show(Node* node, bool on) {
        node->setVisible(on);
        node->setParticipatesInLayout(on);
      }
    };

  } // namespace

  std::string AgentView::statusText() const {
    const agent::Session* model = session();
    if (model == nullptr) {
      return {};
    }
    const agent::Turn* turn = lastTurn(*model);
    const auto now = std::chrono::system_clock::now();
    const auto clock = [&](bool running) {
      if (turn == nullptr) {
        return std::string();
      }
      const auto end = running || !turn->ended ? now : *turn->ended;
      return " \u00b7 " + clockText(std::chrono::duration_cast<std::chrono::seconds>(end - turn->started));
    };
    switch (model->rowStatus()) {
      case agent::RowStatus::Starting: return i18n::tr("terminal.agent.row-starting");
      case agent::RowStatus::Ready: return i18n::tr("terminal.agent.row-ready");
      case agent::RowStatus::Working:
        return (thinking()                                      ? i18n::tr("terminal.agent.thinking")
                : streamingAt(model->entries().size() - 1) ? i18n::tr("terminal.agent.replying")
                                                                : i18n::tr("terminal.agent.working"))
               + clock(true);
      case agent::RowStatus::NeedsPermission: return i18n::tr("terminal.agent.row-needs-you") + clock(true);
      case agent::RowStatus::Done:
      case agent::RowStatus::Declined: return i18n::tr("terminal.agent.row-done") + clock(false);
      case agent::RowStatus::Limited:
      case agent::RowStatus::Stopped: return i18n::tr("terminal.agent.row-stopped") + clock(false);
      case agent::RowStatus::Failed: return i18n::tr("terminal.agent.row-agent-stopped");
    }
    return {};
  }

  bool AgentView::ticking() const {
    const agent::Session* model = session();
    return model != nullptr && (model->phase() == agent::Phase::Working || model->phase() == agent::Phase::Cancelling);
  }

  float AgentView::bandKeep(Node& band, Renderer& renderer) {
    auto* row = dynamic_cast<BandRow*>(&band);
    return row != nullptr && row->status != nullptr ? row->keep(renderer) : band.measure(renderer, {}).width;
  }

  std::unique_ptr<Node> AgentView::band(float s, std::function<void(const Node* anchor)> onPlan, Label** status) const {
    const Kit kit{s};
    const float h = Tokens::spaceP24 * s;
    auto row = std::make_unique<BandRow>();
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Center);
    row->setJustify(FlexJustify::End);
    row->setGap(Tokens::spaceP8 * s);
    row->setFlexGrow(1.0F);
    const agent::Session* model = session();
    if (model == nullptr) {
      return row;
    }
    if (!model->model().empty()) {
      auto pill = ui::row({.align = FlexAlign::Center,
                           .gap = Tokens::spaceP4 * s,
                           .paddingH = Tokens::spaceP8 * s,
                           .fill = colorSpecFromRole(ColorRole::Fill1),
                           .radius = h / 2.0F,
                           .height = h});
      pill->addChild(ui::glyph({.glyph = std::string("sparkles"),
                                .glyphSize = Tokens::spaceP12 * s,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
      pill->addChild(kit.text(agentName() + " \u00b7 " + model->model(), Tokens::typeFootnote, ColorRole::Label1));
      row->agent = row->addChild(std::move(pill));
    }
    const auto& plan = model->plan();
    if (!plan.empty()) {
      const auto current = std::ranges::find(plan, std::string("in_progress"), &agent::PlanEntry::status);
      std::string text = i18n::tr("terminal.agent.plan-title", "progress", planCount(plan));
      if (current != plan.end()) {
        text += " \u00b7 " + current->content;
      }
      const auto pill = [&](std::string label) {
        auto button = ui::button({.text = std::move(label),
                                  .glyph = std::string("list-check"),
                                  .fontSize = Tokens::typeFootnote.size * s,
                                  .glyphSize = Tokens::spaceP12 * s,
                                  .variant = ButtonVariant::Tonal,
                                  .maxWidth = kPlanPillWidth * s,
                                  .paddingH = Tokens::spaceP8 * s,
                                  .gap = Tokens::spaceP4 * s,
                                  .radius = h / 2.0F,
                                  .height = h});
        Button* anchor = button.get();
        button->setOnClick([anchor, onPlan] { onPlan(anchor); });
        return row->addChild(std::move(button));
      };
      row->full = pill(std::move(text));
      row->compact = pill(planCount(plan));
    }
    std::string_view glyph = "circle";
    ColorRole ink = ColorRole::Label2;
    bool spin = false;
    bool escape = false;
    switch (model->rowStatus()) {
      case agent::RowStatus::Starting:
        spin = true;
        break;
      case agent::RowStatus::Ready: break;
      case agent::RowStatus::Working:
        spin = true;
        escape = true;
        break;
      case agent::RowStatus::NeedsPermission:
        glyph = "hand-stop";
        ink = ColorRole::AccentText;
        escape = true;
        break;
      case agent::RowStatus::Done:
      case agent::RowStatus::Declined:
        glyph = "check";
        break;
      case agent::RowStatus::Limited:
      case agent::RowStatus::Stopped:
        glyph = "player-stop-filled";
        break;
      case agent::RowStatus::Failed:
        glyph = "alert-circle";
        ink = ColorRole::TermRed;
        break;
    }
    auto pill = ui::row({.align = FlexAlign::Center,
                         .gap = Tokens::spaceP4 * s,
                         .paddingH = Tokens::spaceP8 * s,
                         .fill = colorSpecFromRole(ColorRole::Fill1),
                         .radius = h / 2.0F,
                         .height = h});
    if (spin) {
      pill->addChild(ui::spinner({.color = colorSpecFromRole(ColorRole::AccentText), .spinnerSize = Tokens::spaceP12 * s}));
      static_cast<Spinner*>(pill->children().back().get())->start();
    } else {
      pill->addChild(ui::glyph({.glyph = std::string(glyph), .glyphSize = Tokens::spaceP12 * s, .color = colorSpecFromRole(ink)}));
    }
    auto label = kit.text(statusText(), Tokens::typeFootnote, ink, FontWeight::Medium);
    if (status != nullptr) {
      *status = label.get();
    }
    pill->addChild(std::move(label));
    if (escape) {
      pill->addChild(ui::keycap({.text = "Esc", .scale = s}));
    }
    row->status = row->addChild(std::move(pill));
    return row;
  }

  float AgentView::planHeight() const {
    const agent::Session* model = session();
    const std::size_t rows = model != nullptr ? model->plan().size() : 0;
    const float header = Tokens::typeFootnote.line + Tokens::spaceP4 + Tokens::spaceP8 + kPlanRowGap;
    return 2.0F * Tokens::spaceP8 + header + static_cast<float>(rows) * (Tokens::typeParagraph.line + kPlanRowGap + Tokens::spaceP8);
  }

  std::unique_ptr<Node> AgentView::planList(float s) const {
    const Kit kit{s};
    auto list = ui::column({.align = FlexAlign::Stretch, .gap = kPlanRowGap * s, .padding = Tokens::spaceP8 * s});
    const agent::Session* model = session();
    if (model == nullptr) {
      return list;
    }
    // Mockup .planpop h2: i18n::tr("terminal.agent.plan") 600 13/18, the count 12/18 label_2 at the end, padding 6 8 8 (top on the 4 grid).
    auto header = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween});
    header->setPadding(Tokens::spaceP4 * s, Tokens::spaceP8 * s, Tokens::spaceP8 * s, Tokens::spaceP8 * s);
    header->addChild(kit.text(i18n::tr("terminal.agent.plan"), Tokens::typeFootnote, ColorRole::Label1, FontWeight::SemiBold));
    header->addChild(kit.text(planCount(model->plan()), Tokens::typeCaption, ColorRole::Label2));
    list->addChild(std::move(header));
    for (const auto& step : model->plan()) {
      const bool done = step.status == "completed";
      const bool current = step.status == "in_progress";
      auto row = ui::row({.align = FlexAlign::Center,
                          .gap = Tokens::spaceP8 * s,
                          .paddingV = Tokens::spaceP4 * s,
                          .paddingH = Tokens::spaceP8 * s,
                          .fill = current ? std::optional(colorSpecFromRole(ColorRole::Fill1)) : std::nullopt,
                          .radius = Style::scaledRadius(Tokens::radiusR8, s)});
      if (current) {
        row->addChild(kit.spinner());
      } else {
        row->addChild(kit.glyph(done ? "check" : "circle", done ? ColorRole::TermGreen : ColorRole::Label2));
      }
      auto text = kit.text(step.content, Tokens::typeParagraph, done ? ColorRole::Label2 : ColorRole::Label1,
                           current ? FontWeight::SemiBold : FontWeight::Normal, (kPlanWidth - 5.0F * Tokens::spaceP8 - Tokens::sizeSymbol) * s);
      row->addChild(std::move(text));
      list->addChild(std::move(row));
    }
    return list;
  }

  AgentView::AgentView(agent::Host& host, std::uint64_t key) : m_host(host), m_key(key) {}

  AgentView::~AgentView() { m_host.close(m_key); }

  const agent::Session* AgentView::session() const { return m_host.session(m_key); }

  // The reply at this entry is still arriving: it is the last entry of a running turn.
  bool AgentView::streamingAt(std::size_t entry) const {
    const agent::Session* model = session();
    return model != nullptr && model->phase() == agent::Phase::Working && entry + 1 == model->entries().size()
           && std::holds_alternative<agent::Prose>(model->entries()[entry]) && std::get<agent::Prose>(model->entries()[entry]).streaming;
  }

  std::string AgentView::activity() const {
    const agent::Session* model = session();
    if (model == nullptr || model->phase() != agent::Phase::Working) {
      return {};
    }
    if (thinking()) {
      return i18n::tr("terminal.agent.thinking");
    }
    const auto& entries = model->entries();
    for (auto it = entries.rbegin(); it != entries.rend() && !std::holds_alternative<agent::Turn>(*it); ++it) {
      if (const auto* action = std::get_if<agent::Action>(&*it); action != nullptr && action->status == agent::ActionStatus::Running) {
        return std::string(verbFor(action->kind).word) + " \u00b7 " + actionTarget(*action, model->cwd());
      }
    }
    return streamingAt(entries.size() - 1) ? i18n::tr("terminal.agent.replying") : i18n::tr("terminal.agent.working");
  }

  // Working with the user's prompt as the last entry: the agent has not answered yet.
  bool AgentView::thinking() const {
    const agent::Session* model = session();
    return model != nullptr && model->phase() == agent::Phase::Working && !model->entries().empty()
           && std::holds_alternative<agent::Turn>(model->entries().back());
  }

  // Item 0 is the session banner; then one item per entry; then the thinking row while it shows.
  std::size_t AgentView::itemCount() const {
    const agent::Session* model = session();
    return model == nullptr ? 0 : 1 + model->entries().size() + (thinking() ? 1 : 0);
  }

  std::uint64_t AgentView::itemRevision(std::size_t index) const {
    const agent::Session* model = session();
    if (model == nullptr) {
      return 0;
    }
    if (index == 0) {
      return std::hash<std::string>{}(model->mode() + '\n' + model->model()) ^ model->commands().size()
             ^ (model->modes().size() << 16);
    }
    if (index - 1 >= model->entries().size()) {
      return 0;
    }
    const std::size_t entry = index - 1;
    return std::visit(
        [&](const auto& e) -> std::uint64_t {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, agent::Prose>) {
            return e.text.size() * 4 + (e.streaming ? 1 : 0) + (streamingAt(entry) ? 2 : 0);
          } else if constexpr (std::is_same_v<T, agent::Note>) {
            return e.text.size();
          } else if constexpr (std::is_same_v<T, agent::Action>) {
            return (e.output.size() << 8) ^ (e.diffs.size() << 4) ^ static_cast<std::uint64_t>(e.status) ^ (asked(e) ? 1U << 7 : 0U)
                   ^ (m_expanded.contains(e.id) ? 1U << 6 : 0U);
          } else if constexpr (std::is_same_v<T, agent::Turn>) {
            return std::hash<std::string>{}(agoText(e.started));
          } else if constexpr (std::is_same_v<T, agent::Request>) {
            const auto& pending = model->pending();
            const auto at = std::ranges::find(pending, entry);
            return (e.answer ? std::hash<std::string>{}(*e.answer) : 1)
                   ^ (static_cast<std::uint64_t>(at - pending.begin()) << 32) ^ pending.size();
          } else if constexpr (std::is_same_v<T, agent::Stop>) {
            return restartable(entry) ? 1 : 0;
          } else {
            return 0;
          }
        },
        model->entries()[entry]);
  }

  void AgentView::toggleDetail(const std::string& id) {
    if (!m_expanded.erase(id)) {
      m_expanded.insert(id);
    }
    changed();
  }

  bool AgentView::restartable(std::size_t entry) const {
    const agent::Session* model = session();
    return model != nullptr && model->phase() == agent::Phase::Failed && entry + 1 == model->entries().size()
           && std::holds_alternative<agent::Stop>(model->entries()[entry]);
  }

  bool AgentView::itemInteractive(std::size_t index) const {
    const agent::Session* model = session();
    if (model == nullptr || index < 1 || index - 1 >= model->entries().size()) {
      return false;
    }
    const auto& entry = model->entries()[index - 1];
    const auto* prose = std::get_if<agent::Prose>(&entry);
    // A reply with a table or code block takes input: they scroll sideways.
    const auto* action = std::get_if<agent::Action>(&entry);
    return std::holds_alternative<agent::Request>(entry) || restartable(index - 1) || (action != nullptr && hasDetail(*action))
           || (prose != nullptr && std::ranges::any_of(agent::markdownBlocks(prose->text), [](const agent::Block& block) {
                return block.kind == agent::Block::Kind::Table || block.kind == agent::Block::Kind::Code;
              }));
  }

  float AgentView::measureItem(Renderer& renderer, std::size_t index, float width) {
    auto node = buildItem(index, width, nullptr);
    return node->measure(renderer, LayoutConstraints::available(width, 1.0e6F)).height;
  }

  std::unique_ptr<Node> AgentView::createItem() { return ui::column({.align = FlexAlign::Stretch}); }

  const agent::Request* AgentView::requestFor(const agent::Action& action) const {
    for (const auto& entry : session()->entries()) {
      if (const auto* request = std::get_if<agent::Request>(&entry); request != nullptr && request->toolCallId == action.id) {
        return request;
      }
    }
    return nullptr;
  }

  bool AgentView::asked(const agent::Action& action) const {
    const agent::Session& model = *session();
    return std::ranges::any_of(model.pending(), [&](std::size_t entry) {
      const auto* request = std::get_if<agent::Request>(&model.entries()[entry]);
      return request != nullptr && request->toolCallId == action.id;
    });
  }

  float AgentView::itemHeightOverride(std::size_t index, float measured) {
    const agent::Session* model = session();
    if (model != nullptr && index >= 1 && std::ranges::find(model->pending(), index - 1) != model->pending().end()) {
      const bool fresh = !m_arrived.contains(index);
      m_arrived[index] = measured;
      if (fresh && m_log != nullptr && m_log->animationManager() != nullptr) {
        const float closed = (Tokens::spaceP12 + Tokens::componentAgentActionRow + 2.0F * Tokens::spaceP8) * m_scale;
        m_motions[index] = {.phase = CardMotion::Opening, .height = closed};
        startMotion(index, closed, measured);
      }
    }
    if (const auto it = m_motions.find(index); it != m_motions.end() && it->second.phase == CardMotion::FoldQueued) {
      it->second.phase = CardMotion::Folding;
      startMotion(index, it->second.height, measured);
    }
    const auto it = m_motions.find(index);
    return it != m_motions.end() ? it->second.height : measured;
  }

  void AgentView::startMotion(std::size_t index, float from, float to) {
    AnimationManager* animations = m_log->animationManager();
    m_motions[index].springs = 1;
    const auto move = animations->animateGated(
        from, to, Style::spring(Tokens::springMove),
        [this, index](float h) {
          if (const auto it = m_motions.find(index); it != m_motions.end()) {
            it->second.height = h;
            applyMotion(it->second);
            m_log->markLayoutDirty();
          }
        },
        Tokens::motionContentGate,
        [this, index, animations] {
          if (const auto it = m_motions.find(index); it == m_motions.end()) {
            return;
          } else {
            ++it->second.springs;
          }
          const auto fade = animations->animateSpring(
              0.0F, 1.0F, Style::spring(Tokens::springFade),
              [this, index](float v) {
                if (const auto it = m_motions.find(index); it != m_motions.end()) {
                  it->second.opacity = v;
                  applyMotion(it->second);
                }
              },
              [this, index] { endSpring(index); }, m_log);
          if (const auto it = m_motions.find(index); it != m_motions.end()) {
            it->second.fade = fade;
          }
        },
        [this, index] { endSpring(index); }, m_log, 0.0F, Unit::Px, true);
    if (const auto it = m_motions.find(index); it != m_motions.end()) {
      it->second.move = move;
    }
  }

  // Opening: the slot holds the card's outer column; Folding: the row that carries the answer.
  void AgentView::applyMotion(const Motion& motion) const {
    if (motion.holder == nullptr || motion.holder->children().empty()) {
      return;
    }
    Node* top = motion.holder->children().front().get();
    if (motion.phase != CardMotion::Opening) {
      top->setOpacity(motion.opacity);
      return;
    }
    if (top->children().empty()) {
      return;
    }
    auto* card = static_cast<Flex*>(top->children().front().get());
    card->setClipChildren(true);
    card->setMaxHeight(std::max(1.0F, motion.height - 2.0F * Tokens::spaceP8 * m_scale));
    for (std::size_t i = 1; i < card->children().size(); ++i) {
      card->children()[i]->setOpacity(motion.opacity);
    }
    card->markLayoutDirty();
  }

  void AgentView::endSpring(std::size_t index) {
    const auto it = m_motions.find(index);
    if (it == m_motions.end() || --it->second.springs > 0) {
      return;
    }
    Motion done = it->second;
    m_motions.erase(it);
    done.opacity = 1.0F;
    applyMotion(done);
    if (done.phase == CardMotion::Opening && done.holder != nullptr && !done.holder->children().empty()
        && !done.holder->children().front()->children().empty()) {
      auto* card = static_cast<Flex*>(done.holder->children().front()->children().front().get());
      card->setMaxHeight(0.0F);
      card->setClipChildren(false);
      card->markLayoutDirty();
    }
    m_log->markLayoutDirty();
  }

  void AgentView::cancelMotion(std::size_t index) {
    const auto it = m_motions.find(index);
    if (it == m_motions.end()) {
      return;
    }
    if (AnimationManager* animations = m_log->animationManager(); animations != nullptr) {
      animations->cancel(it->second.move);
      animations->cancel(it->second.fade);
    }
    m_motions.erase(it);
  }

  void AgentView::bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float width, bool /*hovered*/) {
    auto& holder = static_cast<Flex&>(item);
    std::erase_if(m_clocks, [&](const Clock& clock) { return clock.holder == &item; });
    for (auto& [at, motion] : m_motions) {
      if (motion.holder == &holder) {
        motion.holder = nullptr;
      }
    }
    while (!holder.children().empty()) {
      holder.removeChild(holder.children().back().get());
    }
    Clock clock;
    holder.addChild(buildItem(index, width, &clock));
    if (const auto it = m_motions.find(index); it != m_motions.end()) {
      it->second.holder = &holder;
      applyMotion(it->second);
    }
    if (clock.label != nullptr) {
      clock.holder = &item;
      m_clocks.push_back(std::move(clock));
    }
  }

  void AgentView::tick() {
    for (const Clock& clock : m_clocks) {
      clock.label->setText(clock.prefix + elapsedText(clock.started) + clock.suffix);
    }
  }

  std::unique_ptr<Node> AgentView::buildItem(std::size_t index, float width, Clock* clock) {
    m_lastWidth = width;
    const Kit kit{m_scale};
    const float s = m_scale;
    const float inset = Tokens::spaceP24 * s;
    const float indent = Tokens::componentAgentIndent * s;
    const float measure = Tokens::componentAgentMeasure * s;
    auto item = ui::column({.align = FlexAlign::Stretch, .paddingH = inset, .width = width});
    const agent::Session* model = session();
    if (model == nullptr) {
      return item;
    }
    if (index == 0) {
      auto banner = ui::column({.align = FlexAlign::Start, .gap = Tokens::spaceP8 * s, .paddingV = Tokens::spaceP16 * s});
      const std::string agent = model->model().empty() ? agentName() : agentName() + " \u00b7 " + model->model();
      banner->addChild(kit.text(agent + " in " + model->cwd(), Tokens::typeFootnote, ColorRole::Label1, FontWeight::SemiBold));
      if (const agent::Session::Mode* mode = model->currentMode(); mode != nullptr) {
        banner->addChild(kit.text(i18n::tr("terminal.agent.mode-described", "name", mode->name, "description", mode->description), Tokens::typeCaption,
                                  ColorRole::Label2));
      } else if (!model->mode().empty()) {
        banner->addChild(kit.text(i18n::tr("terminal.agent.mode-raw", "name", model->mode()), Tokens::typeCaption, ColorRole::Label2));
      }
      // An exact width: a Content-width row measures one line, then wraps taller at arrange.
      auto chips = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Tokens::spaceP8 * s,
                            .width = std::min(measure, width - 2.0F * inset)});
      // A hint, not a list: the mockup's three when the agent has them, else its first three; "/" lists them all.
      std::vector<std::string_view> shown;
      for (const std::string_view name : {"compact", "review", "init"}) {
        if (std::ranges::contains(model->commands(), name, &agent::Session::Command::name)) {
          shown.push_back(name);
        }
      }
      if (shown.empty()) {
        for (std::size_t i = 0; i < std::min<std::size_t>(3, model->commands().size()); ++i) {
          shown.push_back(model->commands()[i].name);
        }
      }
      for (const std::string_view name : shown) {
        auto chip = ui::row({.paddingV = Tokens::spaceP2 * s,
                             .paddingH = Tokens::spaceP8 * s,
                             .fill = colorSpecFromRole(ColorRole::Fill1),
                             .radius = Style::scaledRadius(Tokens::radiusR4, s)});
        chip->addChild(kit.code("/" + std::string(name), ColorRole::Label2, 1));
        chips->addChild(std::move(chip));
      }
      if (model->modes().size() > 1) {
        chips->addChild(kit.text(i18n::tr("terminal.agent.mode-hint"), Tokens::typeCaption, ColorRole::Label2));
      }
      if (!chips->children().empty()) {
        banner->addChild(std::move(chips));
      }
      item->addChild(std::move(banner));
      return item;
    }
    const auto& entries = model->entries();
    if (index - 1 >= entries.size()) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingV = Tokens::spaceP8 * s, .paddingH = indent});
      row->addChild(kit.spinner());
      const agent::Turn* turn = lastTurn(*model);
      const Clock thinking{.started = turn != nullptr ? turn->started : std::chrono::system_clock::now(),
                           .prefix = "<i>Thinking \u00b7 ",
                           .suffix = "</i>"};
      auto label = ui::label({.text = thinking.prefix + elapsedText(thinking.started) + thinking.suffix,
                              .fontSize = Tokens::typeParagraph.size * s,
                              .color = colorSpecFromRole(ColorRole::Label2),
                              .configure = [](Label& label) { label.setUseMarkup(true); }});
      if (clock != nullptr) {
        *clock = thinking;
        clock->label = label.get();
      }
      row->addChild(std::move(label));
      item->addChild(std::move(row));
      return item;
    }
    const std::size_t entry = index - 1;
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, agent::Turn>) {
            const bool first = std::ranges::find_if(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(entry),
                                                    [](const agent::Entry& x) { return std::holds_alternative<agent::Turn>(x); })
                               == entries.begin() + static_cast<std::ptrdiff_t>(entry);
            auto row = ui::row({.align = FlexAlign::Start, .width = width - 2.0F * inset});
            row->setPadding(first ? 0.0F : Tokens::spaceP16 * s, 0.0F, Tokens::spaceP8 * s, 0.0F);
            auto mark = ui::label({.text = "›",
                                   .fontSize = Tokens::typeParagraph.size * s,
                                   .fontWeight = FontWeight::SemiBold,
                                   .fontFamily = mono(),
                                   .color = colorSpecFromRole(ColorRole::AccentText),
                                   .width = indent});
            row->addChild(std::move(mark));
            // The column takes the room the time leaves; the prompt wraps at the measure or that room.
            auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
            text->addChild(kit.text(e.text, Tokens::typeParagraph, ColorRole::Label1, FontWeight::Medium, measure));
            row->addChild(std::move(text));
            row->addChild(kit.text(agoText(e.started), Tokens::typeCaption, ColorRole::Label2));
            item->addChild(std::move(row));
          } else if constexpr (std::is_same_v<T, agent::Prose>) {
            auto prose = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .maxWidth = indent + measure});
            prose->setPadding(Tokens::spaceP8 * s, 0.0F, Tokens::spaceP8 * s, indent);
            auto blocks = agent::markdownBlocks(e.text);
            if (streamingAt(entry)) {
              // A thin accent caret ends the last run while the reply streams (no blink).
              const Color accent = colorForRole(ColorRole::AccentText);
              const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
              const std::string caret = std::format("<span foreground=\"#{:02x}{:02x}{:02x}\">\u258f</span>", byte(accent.r),
                                                    byte(accent.g), byte(accent.b));
              if (blocks.empty() || blocks.back().kind != agent::Block::Kind::Paragraph) {
                blocks.push_back({agent::Block::Kind::Paragraph, caret});
              } else {
                blocks.back().text += caret;
              }
            }
            for (const auto& block : blocks) {
              if (block.kind == agent::Block::Kind::Code) {
                auto pinned = std::make_unique<Pinned>(Tokens::spaceP4 * s, Pinned::Corner::TopRight);
                pinned->addChild(kit.well(lines(block.text), ColorRole::Label1, false, Kit::Overflow::Scroll));
                const float size = Tokens::componentAgentCopyButton * s;
                pinned->addChild(ui::button({.glyph = std::string("copy"), .glyphSize = Tokens::spaceP12 * s,
                                             .variant = ButtonVariant::Ghost, .tooltip = std::string(i18n::tr("terminal.agent.copy")),
                                             .radius = size / 2.0F, .width = size, .height = size,
                                             .onClick = [code = block.text] {
                                               if (TextClipboard* clipboard = Input::textClipboard(); clipboard != nullptr) {
                                                 clipboard->setClipboardText(code);
                                               }
                                             }}));
                prose->addChild(std::move(pinned));
              } else if (block.kind == agent::Block::Kind::Table) {
                prose->addChild(kit.table(block.rows, block.alignEnd));
              } else {
                prose->addChild(kit.text(block.text, Tokens::typeParagraph, ColorRole::Label1, FontWeight::Normal, measure, true));
              }
            }
            item->addChild(std::move(prose));
          } else if constexpr (std::is_same_v<T, agent::Note>) {
            auto note = ui::column({.align = FlexAlign::Stretch});
            note->setPadding(Tokens::spaceP8 * s, 0.0F, Tokens::spaceP8 * s, indent);
            note->addChild(kit.text("<i>" + agent::escapeMarkup(e.text) + "</i>", Tokens::typeParagraph, ColorRole::Label2,
                                    FontWeight::Normal, measure, true));
            item->addChild(std::move(note));
          } else if constexpr (std::is_same_v<T, agent::Action>) {
            if (asked(e)) {
              return; // its request card stands in its place
            }
            const bool created = e.kind == "edit" && !e.diffs.empty()
                                 && std::ranges::none_of(e.diffs, [](const agent::DiffHunk& d) { return d.oldText.has_value(); });
            const Verb verb = created ? Verb{"file-plus", i18n::tr("terminal.agent.verb-create")} : verbFor(e.kind);
            const bool failed = e.status == agent::ActionStatus::Failed;
            const bool muted = e.status == agent::ActionStatus::Cancelled || e.status == agent::ActionStatus::Rejected;
            auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .width = width - 2.0F * inset, .height = Tokens::componentAgentActionRow * s});
            row->setPadding(0.0F, 0.0F, 0.0F, indent);
            if (e.status == agent::ActionStatus::Running) {
              row->addChild(kit.spinner());
            } else {
              row->addChild(kit.glyph(failed ? "alert-circle" : verb.glyph, failed ? ColorRole::TermRed : ColorRole::Label2));
            }
            row->addChild(ui::label({.text = std::string(verb.word),
                                     .fontSize = Tokens::typeFootnote.size * s,
                                     .color = colorSpecFromRole(failed ? ColorRole::TermRed : ColorRole::Label2),
                                     .minWidth = Tokens::componentAgentVerbWidth * s,
                                     .maxWidth = Tokens::componentAgentVerbWidth * s}));
            const std::string target = actionTarget(e, model->cwd());
            auto targetLabel = kit.code(target, failed ? ColorRole::TermRed : muted ? ColorRole::Label2 : ColorRole::Label1, 1);
            targetLabel->setFlexGrow(1.0F);
            row->addChild(std::move(targetLabel));
            // An answered request folds into its row: "allowed once · 41 s".
            std::string allowed;
            if (const agent::Request* asked = requestFor(e); asked != nullptr && asked->answer) {
              if (const Answer answer = answerOf(*asked); answer == Answer::AllowedOnce || answer == Answer::AlwaysAllowed) {
                allowed = answerWord(answer) + " \u00b7 ";
              }
            }
            std::string end;
            ColorRole endRole = ColorRole::Label2;
            switch (e.status) {
              case agent::ActionStatus::Pending: end = i18n::tr("terminal.agent.waiting"); break;
              case agent::ActionStatus::Running: end = allowed + elapsedText(e.started); break;
              case agent::ActionStatus::Done:
                end = allowed + doneCaption(e);
                if (end.ends_with(" \u00b7 ") && e.diffs.empty()) {
                  end.resize(end.size() - std::string_view(" \u00b7 ").size());
                }
                break;
              case agent::ActionStatus::Failed:
                if (auto code = numberAfter(e.output, "Exit code "); code && e.kind == "execute") {
                  end = "exit " + *code + " \u00b7 " + durationText(e);
                } else {
                  end = "failed \u00b7 " + durationText(e);
                }
                endRole = ColorRole::TermRed;
                break;
              case agent::ActionStatus::Rejected: end = i18n::tr("terminal.agent.rejected"); break;
              case agent::ActionStatus::Cancelled:
                end = i18n::tr("terminal.agent.cancelled") + " \u00b7 " + clockText(std::chrono::duration_cast<std::chrono::seconds>(e.ended - e.started));
                break;
            }
            if (!end.empty()) {
              auto endLabel = kit.text(end, Tokens::typeFootnote, endRole, failed ? FontWeight::SemiBold : FontWeight::Normal);
              if (e.status == agent::ActionStatus::Running && clock != nullptr) {
                *clock = {.label = endLabel.get(), .started = e.started, .prefix = allowed};
              }
              row->addChild(std::move(endLabel));
            }
            if (e.status == agent::ActionStatus::Done && !e.diffs.empty()) {
              row->addChild(kit.counts(e));
            }
            if (e.status == agent::ActionStatus::Done) {
              row->addChild(kit.glyph("check", ColorRole::Label2));
            } else if (e.status == agent::ActionStatus::Running) {
              row->addChild(ui::keycap({.text = "Esc", .scale = s}));
            }
            const bool open = m_expanded.contains(e.id);
            if (hasDetail(e)) {
              item->addChild(ui::inputArea({.cursorShape = ::CursorShape::Pointer,
                                            .width = width - 2.0F * inset,
                                            .height = Tokens::componentAgentActionRow * s,
                                            .onClick = [this, id = e.id](const InputArea::PointerData&) { toggleDetail(id); }},
                                           std::move(row)));
            } else {
              item->addChild(std::move(row));
            }
            const float block = Tokens::componentAgentBlockIndent * s;
            if (e.status == agent::ActionStatus::Running && !e.output.empty()) {
              auto rows = lines(e.output);
              const auto keep = static_cast<std::size_t>(Tokens::componentAgentTailLines);
              rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(std::min(keep, rows.size())));
              auto tail = ui::column({.align = FlexAlign::Stretch});
              tail->setPadding(Tokens::spaceP4 * s, 0.0F, Tokens::spaceP4 * s, block);
              tail->addChild(kit.well(rows, ColorRole::Label2, true));
              item->addChild(std::move(tail));
            } else if (open && e.status == agent::ActionStatus::Done && !e.diffs.empty()) {
              auto hunk = ui::column({.align = FlexAlign::Stretch, .maxWidth = block + 720.0F * s});
              hunk->setPadding(Tokens::spaceP4 * s, 0.0F, Tokens::spaceP4 * s, block);
              hunk->addChild(kit.diff(firstHunk(e.diffs.front(), e.line.value_or(1)), std::min(720.0F * s, width - 2.0F * inset - block)));
              item->addChild(std::move(hunk));
            } else if (open && !e.output.empty()) {
              auto rows = lines(e.output);
              const auto keep = static_cast<std::size_t>(Tokens::componentAgentTailLines);
              rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(std::min(keep, rows.size())));
              auto well = ui::column({.align = FlexAlign::Stretch});
              well->setPadding(Tokens::spaceP4 * s, 0.0F, Tokens::spaceP4 * s, block);
              well->addChild(kit.well(rows, ColorRole::Label2, false));
              item->addChild(std::move(well));
            }
          } else if constexpr (std::is_same_v<T, agent::Request>) {
            item->addChild(buildRequest(e, entry));
          } else if constexpr (std::is_same_v<T, agent::Stop>) {
            const bool failure = e.kind == agent::StopKind::Failed;
            const auto split = e.message.find('\n');
            const std::string cause = e.message.substr(0, split);
            const std::string log = split == std::string::npos ? std::string() : e.message.substr(split + 1);
            std::string_view glyph = "alert-circle";
            std::string sentence;
            switch (e.kind) {
              case agent::StopKind::Refusal:
                glyph = "ban";
                sentence = i18n::tr("terminal.agent.declined", "name", agentName());
                break;
              case agent::StopKind::Cancelled:
                glyph = "player-stop-filled";
                sentence = i18n::tr("terminal.agent.stopped-by-you");
                for (std::size_t i = entry; i-- > 0;) {
                  if (const auto* turn = std::get_if<agent::Turn>(&entries[i])) {
                    if (turn->ended) {
                      sentence = i18n::tr("terminal.agent.stopped-by-you-at", "time", clockText(std::chrono::duration_cast<std::chrono::seconds>(*turn->ended - turn->started)));
                    }
                    break;
                  }
                }
                break;
              case agent::StopKind::MaxTokens: sentence = i18n::tr("terminal.agent.token-limit"); break;
              case agent::StopKind::MaxTurns: sentence = i18n::tr("terminal.agent.request-limit"); break;
              case agent::StopKind::Failed: sentence = cause.empty() ? i18n::tr("terminal.agent.stopped", "name", agentName()) : i18n::tr("terminal.agent.stopped-with-cause", "name", agentName(), "cause", cause); break;
            }
            auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingV = Tokens::spaceP8 * s});
            row->setPadding(Tokens::spaceP8 * s, 0.0F, Tokens::spaceP8 * s, indent);
            const ColorRole role = failure ? ColorRole::TermRed : ColorRole::Label1;
            row->addChild(kit.glyph(glyph, role));
            auto said = kit.text(sentence, Tokens::typeParagraph, role, FontWeight::SemiBold, measure);
            row->addChild(std::move(said));
            if (restartable(entry)) {
              const auto small = [&](std::string text, std::function<void()> click) {
                return ui::button({.text = std::move(text),
                                   .fontSize = Tokens::typeFootnote.size * s,
                                   .variant = ButtonVariant::Outline,
                                   .radius = Tokens::componentButtonHeightS * s / 2.0F,
                                   .height = Tokens::componentButtonHeightS * s,
                                   .onClick = std::move(click)});
              };
              row->addChild(small(i18n::tr("terminal.agent.restart"), [this] {
                if (agent::Session* model = m_host.session(m_key); model != nullptr) {
                  model->restarting();
                  m_host.flush(m_key);
                  changed();
                }
              }));
              if (!log.empty()) {
                row->addChild(small(i18n::tr("terminal.agent.copy-log"), [log] {
                  if (TextClipboard* clipboard = Input::textClipboard(); clipboard != nullptr) {
                    clipboard->setClipboardText(log);
                  }
                }));
              }
            }
            item->addChild(std::move(row));
            if (failure && !log.empty()) {
              auto rows = lines(log);
              const auto keep = static_cast<std::size_t>(Tokens::componentAgentTailLines);
              rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(std::min(keep, rows.size())));
              auto well = ui::column({.align = FlexAlign::Stretch});
              well->setPadding(0.0F, 0.0F, Tokens::spaceP8 * s, Tokens::componentAgentBlockIndent * s);
              well->addChild(kit.well(rows, ColorRole::Label2, false));
              item->addChild(std::move(well));
            }
          }
        },
        entries[entry]);
    return item;
  }

  // An open request is a raised card with one button per option; an answered one is a line with the answer.
  std::unique_ptr<Node> AgentView::buildRequest(const agent::Request& request, std::size_t entry) {
    const Kit kit{m_scale};
    const float s = m_scale;
    const agent::Session& model = *session();
    const auto& entries = model.entries();
    const agent::Action* action = nullptr;
    for (const auto& e : entries) {
      if (const auto* a = std::get_if<agent::Action>(&e); a != nullptr && a->id == request.toolCallId) {
        action = a;
      }
    }
    const std::string kind = action != nullptr ? action->kind : std::string();
    const std::string title = kind == "execute" ? i18n::tr("terminal.agent.run-command")
                              : kind == "edit"  ? i18n::tr("terminal.agent.edit-file")
                              : action != nullptr && !action->title.empty() ? action->title
                                                                            : std::string(i18n::tr("terminal.agent.use-tool"));
    const auto& pending = model.pending();
    const auto at = std::ranges::find(pending, entry);
    if (at == pending.end() && action != nullptr) {
      return ui::column({}); // its action row carries the answer
    }
    if (at == pending.end()) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .height = Tokens::componentAgentActionRow * s});
      row->setPadding(0.0F, 0.0F, 0.0F, Tokens::componentAgentIndent * s);
      row->addChild(kit.glyph("hand-stop", ColorRole::Label2));
      auto what = kit.text(title, Tokens::typeFootnote, ColorRole::Label2);
      what->setFlexGrow(1.0F);
      row->addChild(std::move(what));
      row->addChild(kit.text(answerWord(answerOf(request)), Tokens::typeFootnote, ColorRole::Label2));
      return row;
    }
    const bool front = at == pending.begin();
    auto card = ui::column({.align = FlexAlign::Stretch,
                            .gap = Tokens::spaceP8 * s,
                            .fill = colorSpecFromRole(ColorRole::Card),
                            .radius = Style::scaledRadius(Tokens::radiusR12, s),
                            .border = colorSpecFromRole(ColorRole::Stroke),
                            .borderWidth = 0.5F * s});
    card->setPadding(Tokens::spaceP12 * s, Tokens::spaceP12 * s, Tokens::spaceP12 * s, Tokens::spaceP16 * s);
    auto header = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    header->addChild(kit.glyph("hand-stop", ColorRole::AccentText));
    auto heading = kit.text(title, Tokens::typeFootnote, ColorRole::Label1, FontWeight::SemiBold);
    heading->setFlexGrow(1.0F);
    header->addChild(std::move(heading));
    header->addChild(kit.text(i18n::tr("terminal.agent.asks", "name", agentName(), "index", (at - pending.begin()) + 1, "total", pending.size()),
                              Tokens::typeCaption, ColorRole::Label2));
    card->addChild(std::move(header));
    if (action != nullptr && kind == "execute" && !action->command.empty()) {
      card->addChild(kit.well(lines(action->command), ColorRole::Label1, false, Kit::Overflow::Wrap));
      card->addChild(kit.text("in " + model.cwd(), Tokens::typeCaption, ColorRole::Label2));
    } else if (action != nullptr && !action->diffs.empty()) {
      // Path bar (fill_1, top corners) over the first hunk (bottom corners): one block.
      const float r = Style::scaledRadius(Tokens::radiusR8, s);
      const float boxWidth = m_lastWidth - 2.0F * Tokens::spaceP24 * s - Tokens::spaceP16 * s - Tokens::spaceP12 * s - Tokens::spaceP16 * s;
      auto bar = ui::row({.align = FlexAlign::Center,
                          .gap = Tokens::spaceP8 * s,
                          .paddingV = Tokens::spaceP4 * s,
                          .paddingH = Tokens::spaceP8 * s,
                          .fill = colorSpecFromRole(ColorRole::Fill1),
                          .width = boxWidth});
      bar->setRadii(Radii{r, r, 0.0F, 0.0F});
      const std::string path = action->diffs.front().path;
      auto name = kit.code(actionTarget(*action, model.cwd()), ColorRole::Label1, 1);
      name->setFontWeight(FontWeight::SemiBold);
      name->setFlexGrow(1.0F);
      bar->addChild(std::move(name));
      bar->addChild(kit.counts(*action));
      auto open = kit.text(i18n::tr("terminal.agent.open"), Tokens::typeFootnote, ColorRole::AccentText);
      open->setCursorShape(::CursorShape::Pointer);
      open->setOnClick([this, path](const InputArea::PointerData&) {
        if (m_openUri) {
          m_openUri(fileUri(path));
        }
      });
      bar->addChild(std::move(open));
      auto block = ui::column({.align = FlexAlign::Stretch});
      block->addChild(std::move(bar));
      auto hunk = kit.diff(firstHunk(action->diffs.front(), action->line.value_or(1)), boxWidth);
      hunk->setRadii(Radii{0.0F, 0.0F, r, r});
      block->addChild(std::move(hunk));
      card->addChild(std::move(block));
      card->addChild(kit.text(action->diffs.size() > 1 ? i18n::tr("terminal.agent.first-of-hunks", "count", action->diffs.size())
                                                        : i18n::tr("terminal.agent.first-hunk"),
                              Tokens::typeCaption, ColorRole::Label2));
    } else if (action != nullptr && !action->paths.empty()) {
      card->addChild(kit.code(action->paths.front(), ColorRole::Label1, 1));
    }
    if (front) {
      auto options = ui::row({.wrap = true, .gap = Tokens::spaceP8 * s});
      for (std::size_t i = 0; i < request.options.size(); ++i) {
        const agent::Option& option = request.options[i];
        // "Always allow <rule> here": the rule is mono and ellipsizes at its end, the words around it stay.
        const std::size_t at = option.rule.empty() ? std::string::npos : option.name.find(option.rule);
        auto button = ui::button({.text = at == std::string::npos ? option.name : StringUtils::trim(option.name.substr(0, at)),
                                  .fontSize = Tokens::typeFootnote.size * s,
                                  .variant = i == 0 ? ButtonVariant::Primary : ButtonVariant::Outline,
                                  .gap = (at == std::string::npos ? Tokens::spaceP8 : Tokens::spaceP4) * s,
                                  .radius = Tokens::componentButtonHeightM * s / 2.0F,
                                  .height = Tokens::componentButtonHeightM * s,
                                  .onClick = [this, i] { answer(i); }});
        if (at != std::string::npos) {
          button->addChild(ui::label({.text = option.rule,
                                      .fontSize = Tokens::typeFootnote.size * s,
                                      .fontFamily = mono(),
                                      .color = colorSpecFromRole(ColorRole::Label1),
                                      .maxWidth = Tokens::componentAgentOptionRule * s,
                                      .maxLines = 1,
                                      .ellipsize = TextEllipsize::End}));
          button->addChild(kit.text(StringUtils::trim(option.name.substr(at + option.rule.size())), Tokens::typeFootnote, ColorRole::Label1));
        }
        if (i < 4) {
          button->addChild(ui::keycap({.text = std::to_string(i + 1), .scale = s}));
        }
        options->addChild(std::move(button));
      }
      card->addChild(std::move(options));
    } else {
      card->addChild(kit.text(i18n::tr("terminal.agent.answer-first"), Tokens::typeCaption, ColorRole::Label2));
    }
    auto outer = ui::column({.align = FlexAlign::Stretch});
    outer->setPadding(Tokens::spaceP8 * s, 0.0F, Tokens::spaceP8 * s, Tokens::spaceP16 * s);
    outer->addChild(std::move(card));
    return outer;
  }

  std::unique_ptr<Node> AgentView::build(Renderer& /*renderer*/, float uiScale, float /*deviceScale*/, float width,
                                         float height) {
    m_scale = uiScale;
    const float s = uiScale;
    auto root = std::make_unique<AgentRoot>();
    root->setDirection(FlexDirection::Vertical);
    root->setAlign(FlexAlign::Stretch);
    root->setSize(width, height);
    root->setFill(colorSpecFromRole(ColorRole::View));
    m_root = root.get();
    root->addChild(ui::virtualListView({.out = &m_log, .contentScale = s, .adapter = this, .flexGrow = 1.0F}));
    m_log->scrollView().setStickToBottom(true);
    m_log->setAnchorBottom(true);
    m_log->setOnScrolled([this] {
      if (atBottom()) {
        syncJump();
      }
    });
    auto composer = ui::row({.out = &m_composer,
                             .align = FlexAlign::Center,
                             .gap = Tokens::spaceP8 * s,
                             .fill = colorSpecFromRole(ColorRole::Fill1),
                             .radius = Style::scaledRadius(Tokens::sizeControlXl / 2.0F, s),
                             .height = Tokens::sizeControlXl * s});
    composer->setPadding(0.0F, Tokens::spaceP8 * s, 0.0F, Tokens::spaceP16 * s);
    composer->addChild(ui::label({.text = "›",
                                  .fontSize = Tokens::typeParagraph.size * s,
                                  .fontFamily = mono(),
                                  .color = colorSpecFromRole(ColorRole::Label2)}));
    composer->addChild(ui::input({.out = &m_input,
                                  .fontSize = Tokens::typeParagraph.size * s,
                                  .frameVisible = false,
                                  .flexGrow = 1.0F,
                                  .onChange =
                                      [this](const std::string&) {
                                        m_slashSelected = 0;
                                        syncComposer();
                                        syncSlash();
                                      },
                                  .onSubmit = [this](const std::string& text) { submit(text); }}));
    // Focus: a 1.5 px accent inner ring on the composer.
    m_input->setOnFocusGain([this, s] {
      if (m_composer != nullptr) {
        m_composer->setBorder(focusRingColorSpec(), 1.5F * s);
      }
    });
    m_input->setOnFocusLoss([this] {
      if (m_composer != nullptr) {
        m_composer->setBorder(clearColorSpec(), 0.0F);
      }
    });
    composer->addChild(ui::label({.out = &m_meta, .fontSize = Tokens::typeCaption.size * s, .color = colorSpecFromRole(ColorRole::Label2)}));
    const float round = Tokens::componentButtonHeightM * s;
    composer->addChild(ui::button({.out = &m_send,
                                   .glyph = std::string("arrow-up"),
                                   .glyphSize = Tokens::sizeSymbol * s,
                                   .variant = ButtonVariant::Primary,
                                   .tooltip = std::string(i18n::tr("terminal.agent.send")),
                                   .radius = round / 2.0F,
                                   .width = round,
                                   .height = round,
                                   .onClick = [this] { submit(m_input->value()); }}));
    composer->addChild(ui::button({.out = &m_stop,
                                   .glyph = std::string("player-stop-filled"),
                                   .glyphSize = Tokens::sizeSymbol * s,
                                   .variant = ButtonVariant::Outline,
                                   .tooltip = std::string(i18n::tr("terminal.agent.stop")),
                                   .radius = round / 2.0F,
                                   .width = round,
                                   .height = round,
                                   .onClick = [this] {
                                     if (session() != nullptr) {
                                       m_host.session(m_key)->cancel();
                                       m_host.flush(m_key);
                                       changed();
                                     }
                                   }}));
    auto dock = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP16 * s});
    dock->addChild(std::move(composer));
    root->dock = root->addChild(std::move(dock));
    // The log fades out under the title band: the band has no edge of its own, it is the same plane.
    auto fade = std::make_unique<RectNode>();
    fade->setParticipatesInLayout(false);
    const Color plane = resolveColorSpec(colorSpecFromRole(ColorRole::View), root.get());
    const Color clear{plane.r, plane.g, plane.b, 0.0F};
    fade->setStyle(RoundedRectStyle{.fill = plane,
                                    .border = clearColor(),
                                    .fillMode = FillMode::LinearGradient,
                                    .gradientDirection = GradientDirection::Vertical,
                                    .gradientStops = {GradientStop{0.0F, plane}, GradientStop{0.0F, plane},
                                                      GradientStop{1.0F, clear}, GradientStop{1.0F, clear}},
                                    .radius = 0.0F,
                                    .softness = 0.0F,
                                    .borderWidth = 0.0F});
    root->fade = root->addChild(std::move(fade));
    auto shadow = ui::box({.out = &m_jumpShadow, .visible = false, .participatesInLayout = false});
    shadow->setShadow(Tokens::elevationE2.keyBlur * s, Tokens::elevationE2.keyY * s);
    root->jumpShadow = root->addChild(std::move(shadow));
    auto jump = ui::row({.out = &m_jump});
    jump->setParticipatesInLayout(false);
    jump->setVisible(false);
    root->jump = root->addChild(std::move(jump));
    auto slash = ui::column({.out = &m_slash, .align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s});
    slash->setParticipatesInLayout(false);
    slash->setVisible(false);
    root->slash = root->addChild(std::move(slash));
    root->scale = s;
    m_seen = itemCount();
    syncComposer();
    syncLog();
    return root;
  }

  void AgentView::setSize(float width, float height) {
    if (m_root != nullptr) {
      m_root->setSize(width, height);
    }
  }

  void AgentView::sceneDestroyed() {
    m_root = nullptr;
    m_composer = nullptr;
    m_jump = nullptr;
    m_jumpShadow = nullptr;
    m_slash = nullptr;
    if (m_log != nullptr && m_log->animationManager() != nullptr) {
      m_log->animationManager()->cancelForOwner(m_log);
    }
    m_motions.clear();
    m_log = nullptr;
    m_clocks.clear();
    m_input = nullptr;
    m_meta = nullptr;
    m_send = nullptr;
    m_stop = nullptr;
  }

  InputArea* AgentView::focusArea() const noexcept { return m_input != nullptr ? m_input->inputArea() : nullptr; }

  void AgentView::changed() {
    syncComposer();
    syncLog();
  }

  bool AgentView::atBottom() const {
    const ScrollView& scroll = m_log->scrollView();
    return scroll.scrollOffset() >= scroll.maxScrollOffset() - 1.0F;
  }

  // Before the list lays out the new items: at the bottom it follows them, scrolled up the jump pill counts them.
  void AgentView::syncLog() {
    const agent::Session* model = session();
    if (m_log == nullptr || model == nullptr) {
      return;
    }
    std::vector<std::size_t> turns;
    for (std::size_t i = 0; i < model->entries().size(); ++i) {
      if (std::holds_alternative<agent::Turn>(model->entries()[i])) {
        turns.push_back(i + 1);
      }
    }
    m_log->setMarkedItems(std::move(turns));
    // An answered card folds into the row that carries its answer: its action row, else its own item.
    for (auto it = m_arrived.begin(); it != m_arrived.end();) {
      const std::size_t entry = it->first - 1;
      if (std::ranges::find(model->pending(), entry) != model->pending().end()) {
        ++it;
        continue;
      }
      std::size_t target = it->first;
      if (const auto* request = entry < model->entries().size() ? std::get_if<agent::Request>(&model->entries()[entry]) : nullptr) {
        for (std::size_t i = 0; i < model->entries().size(); ++i) {
          if (const auto* action = std::get_if<agent::Action>(&model->entries()[i]); action != nullptr && action->id == request->toolCallId) {
            target = i + 1;
          }
        }
      }
      const auto open = m_motions.find(it->first);
      const float from = open != m_motions.end() ? open->second.height : it->second;
      cancelMotion(it->first);
      if (m_log->animationManager() != nullptr) {
        m_motions[target] = {.phase = CardMotion::FoldQueued, .height = from};
      }
      it = m_arrived.erase(it);
    }
    m_log->notifyDataChanged();
    syncJump();
  }

  // The jump pill shows while the log is scrolled up and entries or a request arrived since the user left the bottom.
  void AgentView::syncJump() {
    const agent::Session* model = session();
    if (m_jump == nullptr || model == nullptr) {
      return;
    }
    const std::size_t count = itemCount();
    const bool away = !atBottom();
    if (!away || count < m_seen) {
      m_seen = count;
    }
    const bool asks = away && !model->pending().empty();
    const std::size_t fresh = away ? count - m_seen : 0;
    const bool show = asks || fresh > 0;
    if (show) {
      const float s = m_scale;
      const Kit kit{s};
      std::string text = i18n::tr("terminal.agent.new-below", "count", fresh);
      std::string command;
      if (asks) {
        const auto& request = std::get<agent::Request>(model->entries()[model->pending().front()]);
        std::string target;
        for (const auto& e : model->entries()) {
          if (const auto* action = std::get_if<agent::Action>(&e); action != nullptr && action->id == request.toolCallId) {
            target = !action->command.empty() ? action->command : action->title;
          }
        }
        text = target.empty() ? i18n::tr("terminal.agent.asks-you", "name", agentName()) : i18n::tr("terminal.agent.asks-to-run", "name", agentName());
        command = target;
      }
      while (!m_jump->children().empty()) {
        m_jump->removeChild(m_jump->children().back().get());
      }
      const float h = Tokens::sizeControlM * s;
      m_jump->setFill(colorSpecFromRole(ColorRole::Card));
      m_jump->setRadius(h / 2.0F);
      m_jumpShadow->setRadius(h / 2.0F);
      m_jumpShadow->setFill(colorSpecFromRole(ColorRole::Shadow, isResolvedLightTheme() ? Tokens::elevationE2.keyLight
                                                                                         : Tokens::elevationE2.keyDark));
      m_jump->setBorder(colorSpecFromRole(ColorRole::Separator), 0.5F * s);
      auto pill = ui::button({.text = text,
                              .glyph = std::string(asks ? "hand-stop" : "arrow-down"),
                              .fontSize = Tokens::typeFootnote.size * s,
                              .glyphSize = Tokens::spaceP12 * s,
                              .variant = ButtonVariant::Ghost,
                              .maxWidth = Tokens::componentAgentMeasure * s,
                              .paddingH = Tokens::spaceP12 * s,
                              .gap = Tokens::spaceP8 * s,
                              .radius = h / 2.0F,
                              .height = h,
                              .onClick = [this] { jumpToEnd(); }});
      if (!command.empty()) {
        pill->addChild(ui::label({.text = command,
                                  .fontSize = Tokens::typeFootnote.size * s,
                                  .fontFamily = mono(),
                                  .color = colorSpecFromRole(ColorRole::Label1),
                                  .maxWidth = Tokens::componentAgentMeasure * s / 2.0F,
                                  .maxLines = 1,
                                  .ellipsize = TextEllipsize::End}));
      }
      pill->addChild(ui::keycap({.text = "End", .scale = s}));
      m_jump->addChild(std::move(pill));
    }
    if (m_jump->visible() != show) {
      m_jump->setVisible(show);
      m_jumpShadow->setVisible(show);
      m_root->markLayoutDirty();
      syncComposer();
    }
  }

  // The commands that complete the composer text: it is "/" plus a prefix, without a space, and not dismissed by Esc.
  std::vector<agent::Session::Command> AgentView::slashMatches() const {
    const agent::Session* model = session();
    std::vector<agent::Session::Command> out;
    if (m_input == nullptr || model == nullptr) {
      return out;
    }
    const std::string& text = m_input->value();
    if (!text.starts_with('/') || text.find(' ') != std::string::npos || text == m_slashDismissed) {
      return out;
    }
    for (const auto& command : model->commands()) {
      if (command.name.starts_with(std::string_view(text).substr(1))) {
        out.push_back(command);
      }
    }
    return out;
  }

  void AgentView::syncSlash() {
    if (m_slash == nullptr) {
      return;
    }
    const auto matches = slashMatches();
    while (!m_slash->children().empty()) {
      m_slash->removeChild(m_slash->children().back().get());
    }
    const float s = m_scale;
    m_slash->setFill(colorSpecFromRole(ColorRole::Popover));
    m_slash->setRadius(Style::scaledRadius(Tokens::componentPopoverRadius, s));
    m_slash->setBorder(colorSpecFromRole(ColorRole::Separator), 0.5F * s);
    const std::size_t typed = m_input->value().size();
    m_slashSelected = std::min(m_slashSelected, matches.empty() ? 0 : matches.size() - 1);
    // Rows read label_1; the selected row takes the sidebar's selection tint.
    const ColorSpec clear = clearColorSpec();
    const ColorSpec ink = colorSpecFromRole(ColorRole::Label1);
    const ColorSpec tint = colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F);
    for (std::size_t i = 0; i < matches.size(); ++i) {
      const std::string name = "/" + matches[i].name;
      const ColorSpec bg = i == m_slashSelected ? tint : clear;
      Button::ButtonPalette palette{.normal = {.bg = bg, .border = clear, .label = ink},
                                    .hover = {.bg = i == m_slashSelected ? tint : colorSpecFromRole(ColorRole::Fill1), .border = clear, .label = ink},
                                    .pressed = {.bg = colorSpecFromRole(ColorRole::Fill2), .border = clear, .label = ink},
                                    .disabled = {.bg = clear, .border = clear, .label = ink}};
      auto row = ui::button({.text = name,
                             .fontSize = Tokens::typeFootnote.size * s,
                             .contentAlign = ButtonContentAlign::Start,
                             .variant = ButtonVariant::Ghost,
                             .customPalette = palette,
                             .paddingH = Tokens::spaceP8 * s,
                             .radius = Style::scaledRadius(Tokens::radiusR8, s),
                             .height = Tokens::componentAgentActionRow * s,
                             .onClick = [this, name] { submit(name); }});
      // The typed prefix reads 700 and underlined.
      row->label()->setFontFamily(mono());
      row->label()->setUseMarkup(true);
      row->label()->setText("<b><u>" + agent::escapeMarkup(name.substr(0, typed)) + "</u></b>" + agent::escapeMarkup(name.substr(typed)));
      row->label()->setMinWidth(Tokens::componentAgentSlashName * s);
      row->label()->setMaxWidth(Tokens::componentAgentSlashName * s);
      row->label()->setEllipsize(TextEllipsize::End);
      row->setGap(Tokens::spaceP8 * s);
      row->addChild(ui::label({.text = matches[i].description,
                               .fontSize = Tokens::typeCaption.size * s,
                               .color = colorSpecFromRole(i == m_slashSelected ? ColorRole::Label1 : ColorRole::Label2),
                               .maxLines = 1,
                               .ellipsize = TextEllipsize::End,
                               .flexGrow = 1.0F}));
      m_slash->addChild(std::move(row));
    }
    const bool show = !matches.empty();
    if (m_slash->visible() != show) {
      m_slash->setVisible(show);
    }
    m_root->markLayoutDirty();
  }

  void AgentView::jumpToEnd() {
    if (m_log != nullptr) {
      m_log->scrollView().requestScrollToBottom();
      m_seen = itemCount();
      syncJump();
    }
  }

  // Hint, meta and button follow the phase: stop while a turn runs, send when there is text to send.
  void AgentView::syncComposer() {
    const agent::Session* model = session();
    if (m_input == nullptr || model == nullptr) {
      return;
    }
    const agent::Phase phase = model->phase();
    const bool running = phase == agent::Phase::Working || phase == agent::Phase::Cancelling;
    // Scrolled up, the jump pill names the request and the card sits below the view.
    const bool below = m_jump != nullptr && m_jump->visible();
    m_input->setPlaceholder(!model->pending().empty()    ? (below ? i18n::tr("terminal.agent.answer-below") : i18n::tr("terminal.agent.answer-above"))
                            : running                    ? i18n::tr("terminal.agent.queue")
                            : phase == agent::Phase::Starting ? i18n::tr("terminal.agent.starting-placeholder", "name", agentName())
                            : phase == agent::Phase::Failed   ? i18n::tr("terminal.agent.restart-placeholder", "name", agentName())
                            : model->rowStatus() == agent::RowStatus::Ready ? i18n::tr("terminal.agent.ask-placeholder", "name", agentName())
                                                                            : std::string(i18n::tr("terminal.agent.reply-placeholder")));
    const agent::Session::Mode* mode = model->currentMode();
    std::string meta = mode != nullptr ? mode->name : model->mode();
    bool full = false;
    if (model->contextSize() > 0) {
      const auto percent = model->contextUsed() * 100 / model->contextSize();
      full = percent >= 90;
      meta += i18n::tr("terminal.agent.context", "separator", meta.empty() ? "" : " · ", "percent", percent);
    }
    if (model->compacting()) {
      meta += (meta.empty() ? std::string() : std::string(" · ")) + i18n::tr("terminal.agent.compacting");
    }
    m_meta->setText(meta);
    m_meta->setColor(colorSpecFromRole(full ? ColorRole::TermRed : ColorRole::Label2));
    const bool canSend = !running && (phase == agent::Phase::Idle || phase == agent::Phase::Failed) && !m_input->value().empty();
    m_send->setVisible(canSend);
    m_send->setParticipatesInLayout(canSend);
    m_stop->setVisible(running);
    m_stop->setParticipatesInLayout(running);
  }

  void AgentView::submit(const std::string& text) {
    agent::Session* model = m_host.session(m_key);
    if (model == nullptr || text.empty() || !model->send(text)) {
      return;
    }
    m_input->setValue("");
    m_host.flush(m_key);
    changed();
  }

  void AgentView::answer(std::size_t option) {
    agent::Session* model = m_host.session(m_key);
    if (model != nullptr && model->answer(option)) {
      m_host.flush(m_key);
      changed();
    }
  }

  bool AgentView::key(const KeyboardEvent& event) {
    agent::Session* model = m_host.session(m_key);
    if (!event.pressed || model == nullptr) {
      return false;
    }
    if (event.sym == kShiftTab || (event.sym == kTab && (event.modifiers & KeyMod::Shift) != 0)) {
      model->cycleMode();
      m_host.flush(m_key);
      return true;
    }
    if (const auto matches = slashMatches(); m_slash != nullptr && m_slash->visible() && !matches.empty()) {
      if (event.sym == kUp || event.sym == kDown) {
        const std::size_t n = matches.size();
        m_slashSelected = (m_slashSelected + (event.sym == kDown ? 1 : n - 1)) % n;
        syncSlash();
        return true;
      }
      if (event.sym == kTab) {
        m_input->setValue("/" + matches[m_slashSelected].name + " ");
        return true;
      }
      if (event.sym == kReturn) {
        submit("/" + matches[m_slashSelected].name);
        return true;
      }
      if (event.sym == kEscape) {
        m_slashDismissed = m_input->value();
        syncSlash();
        return true;
      }
    }
    if (event.sym == kEscape && (model->phase() == agent::Phase::Working)) {
      model->cancel();
      m_host.flush(m_key);
      changed();
      return true;
    }
    if (event.sym == kEnd && m_jump != nullptr && m_jump->visible()) {
      jumpToEnd();
      return true;
    }
    if (event.modifiers == 0 && event.unshifted >= '1' && event.unshifted <= '4' && !model->pending().empty()
        && m_input != nullptr && m_input->value().empty()) {
      answer(static_cast<std::size_t>(event.unshifted - '1'));
      return true;
    }
    return false;
  }

} // namespace term
