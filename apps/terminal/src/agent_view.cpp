#include "agent_view.h"

#include "agent_host.h"
#include "agent_markup.h"
#include "design_tokens.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/spinner.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

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

    std::string agoText(agent::WallTime at) {
      const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now() - at).count();
      if (minutes < 1) {
        return "now";
      }
      if (minutes < 60) {
        return std::format("{} min ago", minutes);
      }
      return minutes < 24 * 60 ? std::format("{} h ago", minutes / 60) : std::format("{} d ago", minutes / (24 * 60));
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
      std::string_view word;
    };

    Verb verbFor(std::string_view kind) {
      if (kind == "read") {
        return {"file-text", "Read"};
      }
      if (kind == "search") {
        return {"search", "Search"};
      }
      if (kind == "edit" || kind == "delete" || kind == "move") {
        return {"pencil", kind == "edit" ? "Edit" : kind == "delete" ? "Delete" : "Move"};
      }
      if (kind == "execute") {
        return {"terminal", "Run"};
      }
      if (kind == "fetch") {
        return {"world", "Fetch"};
      }
      if (kind == "think") {
        return {"bulb", "Think"};
      }
      return {"sparkles", "Tool"};
    }

    // The first hunk of a whole-file edit: the changed middle after the common leading and trailing lines.
    struct Hunk {
      std::size_t firstLine = 0; // 1-based line of the first change
      std::vector<std::string_view> removed;
      std::vector<std::string_view> added;
    };

    Hunk firstHunk(const agent::DiffHunk& diff) {
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
      return {.firstLine = head + 1,
              .removed = {before.begin() + static_cast<std::ptrdiff_t>(head), before.end() - static_cast<std::ptrdiff_t>(tail)},
              .added = {after.begin() + static_cast<std::ptrdiff_t>(head), after.end() - static_cast<std::ptrdiff_t>(tail)}};
    }

    std::string countCaption(const agent::Action& action) {
      std::size_t added = 0;
      std::size_t removed = 0;
      for (const auto& diff : action.diffs) {
        const Hunk hunk = firstHunk(diff);
        added += hunk.added.size();
        removed += hunk.removed.size();
      }
      return std::format("+{} −{}", added, removed);
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

      std::unique_ptr<Label> code(std::string value, ColorRole role, int maxLines = 0) const {
        return ui::label({.text = std::move(value),
                          .fontSize = Tokens::typeFootnote.size * s,
                          .fontFamily = mono(),
                          .color = colorSpecFromRole(role),
                          .maxLines = maxLines,
                          .ellipsize = maxLines > 0 ? std::optional(TextEllipsize::End) : std::nullopt});
      }

      std::unique_ptr<Glyph> glyph(std::string_view name, ColorRole role) const {
        return ui::glyph({.glyph = std::string(name), .glyphSize = Tokens::sizeSymbol * s, .color = colorSpecFromRole(role)});
      }

      std::unique_ptr<Spinner> spinner() const {
        auto spin = ui::spinner({.color = colorSpecFromRole(ColorRole::AccentText), .spinnerSize = Tokens::sizeSymbol * s});
        spin->start();
        return spin;
      }

      // A mono block on fill_1: output tails, error wells, command wells.
      std::unique_ptr<Flex> well(std::vector<std::string_view> rows, ColorRole role, bool lastStrong) const {
        auto box = ui::column({.align = FlexAlign::Stretch,
                               .paddingV = Tokens::spaceP8 * s,
                               .paddingH = Tokens::spaceP12 * s,
                               .fill = colorSpecFromRole(ColorRole::Fill1),
                               .radius = Style::scaledRadius(Tokens::radiusR8, s)});
        for (std::size_t i = 0; i < rows.size(); ++i) {
          const bool strong = lastStrong && i + 1 == rows.size();
          box->addChild(code(std::string(rows[i]), strong ? ColorRole::Label1 : role, 1));
        }
        return box;
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

    std::string optionWord(const agent::Request& request) {
      if (!request.answer) {
        return {};
      }
      const auto it = std::ranges::find(request.options, *request.answer, &agent::Option::id);
      if (it == request.options.end()) {
        return *request.answer;
      }
      return it->kind == "allow_once" ? "allowed once" : it->kind == "allow_always" ? "always allowed" : "rejected";
    }

  } // namespace

  namespace {

    constexpr float kPlanWidth = 360.0F;
    constexpr float kPlanRowGap = 4.0F;

    // The title band: the status pill never shrinks; the plan pill reads "Plan N of M · current" when it fits, else
    // "N of M", else hides. It measures at its minimum so the header ellipsizes the title before it folds the band.
    class BandRow final : public Flex {
    public:
      Node* status = nullptr;
      Node* full = nullptr;
      Node* compact = nullptr;

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
        if (full == nullptr) {
          return;
        }
        const float room = w - status->measure(renderer, {}).width - gap();
        const bool showFull = full->measure(renderer, {}).width <= room;
        show(full, showFull);
        show(compact, !showFull && compact->measure(renderer, {}).width <= room);
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
      case agent::RowStatus::Starting: return "starting";
      case agent::RowStatus::Ready: return "ready";
      case agent::RowStatus::Working: return (thinking() ? "thinking" : "working") + clock(true);
      case agent::RowStatus::NeedsPermission: return "needs you" + clock(true);
      case agent::RowStatus::Done: return "done" + clock(false);
      case agent::RowStatus::Stopped: return "stopped" + clock(false);
      case agent::RowStatus::Failed: return "agent stopped";
    }
    return {};
  }

  bool AgentView::ticking() const {
    const agent::Session* model = session();
    return model != nullptr && (model->phase() == agent::Phase::Working || model->phase() == agent::Phase::Cancelling);
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
    const auto& plan = model->plan();
    if (!plan.empty()) {
      const auto done = std::ranges::count(plan, std::string("completed"), &agent::PlanEntry::status);
      const auto current = std::ranges::find(plan, std::string("in_progress"), &agent::PlanEntry::status);
      std::string text = std::format("Plan {} of {}", done, plan.size());
      if (current != plan.end()) {
        text += " \u00b7 " + current->content;
      }
      const auto pill = [&](std::string label) {
        auto button = ui::button({.text = std::move(label),
                                  .glyph = std::string("list-check"),
                                  .fontSize = Tokens::typeFootnote.size * s,
                                  .glyphSize = Tokens::spaceP12 * s,
                                  .variant = ButtonVariant::Tonal,
                                  .maxWidth = kPlanWidth * s,
                                  .paddingH = Tokens::spaceP8 * s,
                                  .gap = Tokens::spaceP4 * s,
                                  .radius = h / 2.0F,
                                  .height = h});
        Button* anchor = button.get();
        button->setOnClick([anchor, onPlan] { onPlan(anchor); });
        return row->addChild(std::move(button));
      };
      row->full = pill(std::move(text));
      row->compact = pill(std::format("{} of {}", done, plan.size()));
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
        glyph = "check";
        break;
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
    return 2.0F * Tokens::spaceP8 + static_cast<float>(rows) * (Tokens::typeParagraph.line + kPlanRowGap + Tokens::spaceP8);
  }

  std::unique_ptr<Node> AgentView::planList(float s) const {
    const Kit kit{s};
    auto list = ui::column({.align = FlexAlign::Stretch, .gap = kPlanRowGap * s, .padding = Tokens::spaceP8 * s});
    const agent::Session* model = session();
    if (model == nullptr) {
      return list;
    }
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
      return std::hash<std::string>{}(model->mode()) ^ model->commands().size();
    }
    if (index - 1 >= model->entries().size()) {
      return 0;
    }
    const std::size_t entry = index - 1;
    return std::visit(
        [&](const auto& e) -> std::uint64_t {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, agent::Prose>) {
            return e.text.size() * 2 + (e.streaming ? 1 : 0);
          } else if constexpr (std::is_same_v<T, agent::Note>) {
            return e.text.size();
          } else if constexpr (std::is_same_v<T, agent::Action>) {
            return (e.output.size() << 8) ^ (e.diffs.size() << 4) ^ static_cast<std::uint64_t>(e.status);
          } else if constexpr (std::is_same_v<T, agent::Turn>) {
            return std::hash<std::string>{}(agoText(e.started));
          } else if constexpr (std::is_same_v<T, agent::Request>) {
            const auto& pending = model->pending();
            const auto at = std::ranges::find(pending, entry);
            return (e.answer ? std::hash<std::string>{}(*e.answer) : 1)
                   ^ (static_cast<std::uint64_t>(at - pending.begin()) << 32) ^ pending.size();
          } else {
            return 0;
          }
        },
        model->entries()[entry]);
  }

  bool AgentView::itemInteractive(std::size_t index) const {
    const agent::Session* model = session();
    return model != nullptr && index >= 1 && index - 1 < model->entries().size()
           && std::holds_alternative<agent::Request>(model->entries()[index - 1]);
  }

  float AgentView::measureItem(Renderer& renderer, std::size_t index, float width) {
    auto node = buildItem(index, width);
    return node->measure(renderer, LayoutConstraints::available(width, 1.0e6F)).height;
  }

  std::unique_ptr<Node> AgentView::createItem() { return ui::column({.align = FlexAlign::Stretch}); }

  void AgentView::bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float width, bool /*hovered*/) {
    auto& holder = static_cast<Flex&>(item);
    while (!holder.children().empty()) {
      holder.removeChild(holder.children().back().get());
    }
    holder.addChild(buildItem(index, width));
  }

  std::unique_ptr<Node> AgentView::buildItem(std::size_t index, float width) {
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
      banner->addChild(kit.text("Claude in " + model->cwd(), Tokens::typeFootnote, ColorRole::Label1, FontWeight::SemiBold));
      if (!model->mode().empty()) {
        banner->addChild(kit.text("Mode: " + model->mode(), Tokens::typeCaption, ColorRole::Label2));
      }
      if (!model->commands().empty()) {
        auto chips = ui::row({.wrap = true, .gap = Tokens::spaceP8 * s, .maxWidth = measure});
        for (const auto& command : model->commands()) {
          auto chip = ui::row({.paddingV = Tokens::spaceP2 * s,
                               .paddingH = Tokens::spaceP8 * s,
                               .fill = colorSpecFromRole(ColorRole::Fill1),
                               .radius = Style::scaledRadius(Tokens::radiusR4, s)});
          chip->addChild(kit.code("/" + command, ColorRole::Label2, 1));
          chips->addChild(std::move(chip));
        }
        banner->addChild(std::move(chips));
      }
      item->addChild(std::move(banner));
      return item;
    }
    const auto& entries = model->entries();
    if (index - 1 >= entries.size()) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingV = Tokens::spaceP8 * s, .paddingH = indent});
      row->addChild(kit.spinner());
      row->addChild(ui::label({.text = "<i>Thinking</i>",
                               .fontSize = Tokens::typeParagraph.size * s,
                               .color = colorSpecFromRole(ColorRole::Label2),
                               .configure = [](Label& label) { label.setUseMarkup(true); }}));
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
            auto row = ui::row({.align = FlexAlign::Start});
            row->setPadding(first ? 0.0F : Tokens::spaceP16 * s, 0.0F, Tokens::spaceP8 * s, 0.0F);
            auto mark = ui::label({.text = "›",
                                   .fontSize = Tokens::typeParagraph.size * s,
                                   .fontWeight = FontWeight::SemiBold,
                                   .fontFamily = mono(),
                                   .color = colorSpecFromRole(ColorRole::AccentText),
                                   .width = indent});
            row->addChild(std::move(mark));
            row->addChild(kit.text(e.text, Tokens::typeParagraph, ColorRole::Label1, FontWeight::Medium, measure));
            row->addChild(ui::row({.flexGrow = 1.0F}));
            row->addChild(kit.text(agoText(e.started), Tokens::typeCaption, ColorRole::Label2));
            item->addChild(std::move(row));
          } else if constexpr (std::is_same_v<T, agent::Prose>) {
            auto prose = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .maxWidth = indent + measure});
            prose->setPadding(Tokens::spaceP8 * s, 0.0F, Tokens::spaceP8 * s, indent);
            for (const auto& block : agent::markdownBlocks(e.text)) {
              if (block.kind == agent::Block::Kind::Code) {
                prose->addChild(kit.well(lines(block.text), ColorRole::Label1, false));
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
            const Verb verb = verbFor(e.kind);
            const bool failed = e.status == agent::ActionStatus::Failed;
            const bool muted = e.status == agent::ActionStatus::Cancelled || e.status == agent::ActionStatus::Rejected;
            auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .height = Tokens::componentAgentActionRow * s});
            row->setPadding(0.0F, 0.0F, 0.0F, indent);
            if (e.status == agent::ActionStatus::Running) {
              row->addChild(kit.spinner());
            } else {
              row->addChild(kit.glyph(failed ? "alert-circle" : verb.glyph, failed ? ColorRole::TermRed : ColorRole::Label2));
            }
            row->addChild(ui::label({.text = std::string(verb.word),
                                     .fontSize = Tokens::typeFootnote.size * s,
                                     .color = colorSpecFromRole(failed ? ColorRole::TermRed : ColorRole::Label2),
                                     .width = Tokens::componentAgentVerbWidth * s}));
            const std::string target = !e.command.empty() ? e.command : !e.paths.empty() ? e.paths.front() : e.title;
            auto targetLabel = kit.code(target, failed ? ColorRole::TermRed : muted ? ColorRole::Label2 : ColorRole::Label1, 1);
            targetLabel->setFlexGrow(1.0F);
            row->addChild(std::move(targetLabel));
            std::string end;
            ColorRole endRole = ColorRole::Label2;
            switch (e.status) {
              case agent::ActionStatus::Pending: end = "waiting for you"; break;
              case agent::ActionStatus::Running: break;
              case agent::ActionStatus::Done:
                if (!e.diffs.empty()) {
                  end = countCaption(e);
                }
                break;
              case agent::ActionStatus::Failed:
                end = "failed";
                endRole = ColorRole::TermRed;
                break;
              case agent::ActionStatus::Rejected: end = "rejected"; break;
              case agent::ActionStatus::Cancelled: end = "cancelled"; break;
            }
            if (!end.empty()) {
              row->addChild(kit.text(end, Tokens::typeFootnote, endRole, failed ? FontWeight::SemiBold : FontWeight::Normal));
            }
            if (e.status == agent::ActionStatus::Done) {
              row->addChild(kit.glyph("check", ColorRole::Label2));
            } else if (e.status == agent::ActionStatus::Running) {
              row->addChild(ui::keycap({.text = "Esc", .scale = s}));
            }
            item->addChild(std::move(row));
            const float block = Tokens::componentAgentBlockIndent * s;
            if (e.status == agent::ActionStatus::Running && !e.output.empty()) {
              auto rows = lines(e.output);
              const auto keep = static_cast<std::size_t>(Tokens::componentAgentTailLines);
              rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(std::min(keep, rows.size())));
              auto tail = ui::column({.align = FlexAlign::Stretch});
              tail->setPadding(Tokens::spaceP4 * s, 0.0F, Tokens::spaceP4 * s, block);
              tail->addChild(kit.well(rows, ColorRole::Label2, true));
              item->addChild(std::move(tail));
            } else if (e.status == agent::ActionStatus::Done && !e.diffs.empty()) {
              auto hunk = ui::column({.align = FlexAlign::Stretch, .maxWidth = block + 720.0F * s});
              hunk->setPadding(Tokens::spaceP4 * s, 0.0F, Tokens::spaceP4 * s, block);
              hunk->addChild(kit.diff(firstHunk(e.diffs.front()), std::min(720.0F * s, width - 2.0F * inset - block)));
              item->addChild(std::move(hunk));
            } else if (failed && !e.output.empty()) {
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
            std::string_view glyph = "alert-circle";
            std::string sentence;
            switch (e.kind) {
              case agent::StopKind::Refusal:
                glyph = "ban";
                sentence = "Claude declined this request.";
                break;
              case agent::StopKind::Cancelled:
                glyph = "player-stop-filled";
                sentence = "You stopped the turn.";
                break;
              case agent::StopKind::MaxTokens: sentence = "The reply reached its token limit."; break;
              case agent::StopKind::MaxTurns: sentence = "The turn reached its request limit."; break;
              case agent::StopKind::Failed: sentence = "Claude stopped."; break;
            }
            auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingV = Tokens::spaceP8 * s});
            row->setPadding(Tokens::spaceP8 * s, 0.0F, Tokens::spaceP8 * s, indent);
            const ColorRole role = failure ? ColorRole::TermRed : ColorRole::Label2;
            row->addChild(kit.glyph(glyph, role));
            row->addChild(kit.text(sentence, Tokens::typeParagraph, role));
            item->addChild(std::move(row));
            if (failure && !e.message.empty()) {
              auto rows = lines(e.message);
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
    const std::string title = kind == "execute" ? "Run a command"
                              : kind == "edit"  ? "Edit a file"
                              : action != nullptr && !action->title.empty() ? action->title
                                                                            : std::string("Use a tool");
    const auto& pending = model.pending();
    const auto at = std::ranges::find(pending, entry);
    if (at == pending.end()) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .height = Tokens::componentAgentActionRow * s});
      row->setPadding(0.0F, 0.0F, 0.0F, Tokens::componentAgentIndent * s);
      row->addChild(kit.glyph("hand-stop", ColorRole::Label2));
      auto what = kit.text(title, Tokens::typeFootnote, ColorRole::Label2);
      what->setFlexGrow(1.0F);
      row->addChild(std::move(what));
      row->addChild(kit.text(optionWord(request), Tokens::typeFootnote, ColorRole::Label2));
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
    header->addChild(kit.text(std::format("Claude asks · {} of {}", (at - pending.begin()) + 1, pending.size()),
                              Tokens::typeCaption, ColorRole::Label2));
    card->addChild(std::move(header));
    if (action != nullptr && kind == "execute" && !action->command.empty()) {
      card->addChild(kit.well(lines(action->command), ColorRole::Label1, false));
      card->addChild(kit.text("in " + model.cwd(), Tokens::typeCaption, ColorRole::Label2));
    } else if (action != nullptr && !action->diffs.empty()) {
      card->addChild(kit.code(action->diffs.front().path + "  " + countCaption(*action), ColorRole::Label2, 1));
      card->addChild(kit.diff(firstHunk(action->diffs.front()), m_lastWidth - 2.0F * Tokens::spaceP24 * s - Tokens::spaceP16 * s - Tokens::spaceP12 * s - Tokens::spaceP16 * s));
      card->addChild(kit.text("Shows the first hunk", Tokens::typeCaption, ColorRole::Label2));
    } else if (action != nullptr && !action->paths.empty()) {
      card->addChild(kit.code(action->paths.front(), ColorRole::Label1, 1));
    }
    if (front) {
      auto options = ui::row({.wrap = true, .gap = Tokens::spaceP8 * s});
      for (std::size_t i = 0; i < request.options.size(); ++i) {
        auto button = ui::button({.text = request.options[i].name,
                                  .fontSize = Tokens::typeFootnote.size * s,
                                  .variant = i == 0 ? ButtonVariant::Primary : ButtonVariant::Outline,
                                  .gap = Tokens::spaceP8 * s,
                                  .radius = Tokens::componentButtonHeightM * s / 2.0F,
                                  .height = Tokens::componentButtonHeightM * s,
                                  .onClick = [this, i] { answer(i); }});
        if (i < 4) {
          button->addChild(ui::keycap({.text = std::to_string(i + 1), .scale = s}));
        }
        options->addChild(std::move(button));
      }
      card->addChild(std::move(options));
    } else {
      card->addChild(kit.text("Answer the request above first", Tokens::typeCaption, ColorRole::Label2));
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
    auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch, .width = width, .height = height,
                            .configure = [](Flex& flex) { flex.setFill(colorSpecFromRole(ColorRole::View)); }});
    root->addChild(ui::virtualListView({.out = &m_log, .contentScale = s, .adapter = this, .flexGrow = 1.0F}));
    m_log->scrollView().setStickToBottom(true);
    auto composer = ui::row({.align = FlexAlign::Center,
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
                                  .onChange = [this](const std::string&) { syncComposer(); },
                                  .onSubmit = [this](const std::string& text) { submit(text); }}));
    composer->addChild(ui::label({.out = &m_meta, .fontSize = Tokens::typeCaption.size * s, .color = colorSpecFromRole(ColorRole::Label2)}));
    const float round = Tokens::componentButtonHeightM * s;
    composer->addChild(ui::button({.out = &m_send,
                                   .glyph = std::string("arrow-up"),
                                   .glyphSize = Tokens::sizeSymbol * s,
                                   .variant = ButtonVariant::Primary,
                                   .tooltip = std::string("Send  Enter"),
                                   .radius = round / 2.0F,
                                   .width = round,
                                   .height = round,
                                   .onClick = [this] { submit(m_input->value()); }}));
    composer->addChild(ui::button({.out = &m_stop,
                                   .glyph = std::string("player-stop-filled"),
                                   .glyphSize = Tokens::sizeSymbol * s,
                                   .variant = ButtonVariant::Outline,
                                   .tooltip = std::string("Stop  Esc"),
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
    root->addChild(std::move(dock));
    syncComposer();
    return root;
  }

  void AgentView::setSize(float width, float height) {
    if (m_root != nullptr) {
      m_root->setSize(width, height);
    }
  }

  void AgentView::sceneDestroyed() {
    m_root = nullptr;
    m_log = nullptr;
    m_input = nullptr;
    m_meta = nullptr;
    m_send = nullptr;
    m_stop = nullptr;
  }

  InputArea* AgentView::focusArea() const noexcept { return m_input != nullptr ? m_input->inputArea() : nullptr; }

  void AgentView::changed() {
    if (m_log != nullptr) {
      m_log->notifyDataChanged();
    }
    syncComposer();
  }

  // Hint, meta and button follow the phase: stop while a turn runs, send when there is text to send.
  void AgentView::syncComposer() {
    const agent::Session* model = session();
    if (m_input == nullptr || model == nullptr) {
      return;
    }
    const agent::Phase phase = model->phase();
    const bool running = phase == agent::Phase::Working || phase == agent::Phase::Cancelling;
    m_input->setPlaceholder(!model->pending().empty()    ? "1–4 answers the request above"
                            : running                    ? "Queue a follow-up · Esc stops"
                            : phase == agent::Phase::Starting ? "Starting Claude…"
                            : phase == agent::Phase::Failed   ? "Claude stopped"
                                                              : "Ask Claude");
    std::string meta = model->mode();
    bool full = false;
    if (model->contextSize() > 0) {
      const auto percent = model->contextUsed() * 100 / model->contextSize();
      full = percent >= 90;
      meta += std::format("{}{} % context", meta.empty() ? "" : " · ", percent);
    }
    m_meta->setText(meta);
    m_meta->setColor(colorSpecFromRole(full ? ColorRole::TermRed : ColorRole::Label2));
    const bool canSend = !running && phase == agent::Phase::Idle && !m_input->value().empty();
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
    if (event.sym == kEscape && (model->phase() == agent::Phase::Working)) {
      model->cancel();
      m_host.flush(m_key);
      changed();
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
