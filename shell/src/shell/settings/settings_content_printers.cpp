#include "shell/settings/settings_content_printers.h"

#include "i18n/i18n.h"
#include "printing/print_settings.h"
#include "shell/settings/printers_model.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <format>

namespace settings {
  namespace {

    using namespace page_rows;

    constexpr float kIcon = 32.0F;
    TOKENS_LIVE(kStatusDotSize, Tokens::componentListStatusDot);
    constexpr std::size_t kJobsOnPage = 3;

    std::string detailId(const PrinterInfo& p) { return "printer:" + p.label + "\t" + p.name; }

    ColorRole dotRole(const PrinterInfo& p) {
      if (p.state == PrinterState::NeedsAttention) {
        return ColorRole::Warning;
      }
      return p.state == PrinterState::Offline ? ColorRole::Label3 : ColorRole::Success;
    }

    std::string statusText(const PrinterInfo& p) {
      if (p.paused) {
        const bool ownReason = !p.reason.empty() && p.reason != "Paused";
        return ownReason ? i18n::tr("settings.printers.paused-because", "reason", p.reason)
                         : i18n::tr("settings.printers.paused");
      }
      switch (p.state) {
      case PrinterState::Ready:
        return i18n::tr("settings.printers.ready");
      case PrinterState::Printing:
        return i18n::tr("settings.printers.printing");
      case PrinterState::NeedsAttention:
        return p.reason.empty() ? i18n::tr("settings.printers.attention") : p.reason;
      case PrinterState::Offline:
        return i18n::tr("settings.printers.offline");
      }
      return {};
    }

    std::string jobText(const JobInfo& j) {
      switch (j.state) {
      case JobState::Printing:
        return j.pagesDone > 0 ? i18n::tr("settings.printers.job-page", "page", std::to_string(j.pagesDone + 1))
                               : i18n::tr("settings.printers.printing");
      case JobState::Stopped:
        return i18n::tr("settings.printers.job-held");
      default:
        return i18n::tr("settings.printers.job-waiting");
      }
    }

    std::unique_ptr<Node> roundSymbol(std::string_view glyph, float size, float scale) {
      return ui::row(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .fill = colorSpecFromRole(ColorRole::Fill2),
           .radius = size * 0.5F * scale,
           .width = size * scale,
           .height = size * scale},
          symbol(glyph, Tokens::sizeSymbol * scale, ColorRole::Label2)
      );
    }

    // Name over a status line with a state dot; an action error replaces the status in the danger colour.
    std::unique_ptr<Flex> nameAndStatus(std::string name, const PrinterInfo* p, const std::string* error,
                                        const std::string& status, bool bold, float scale) {
      auto copy = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
      copy->addChild(ui::label({.text = std::move(name), .fontSize = Style::fontSizeBody * scale,
                                .fontWeight = bold ? FontWeight::SemiBold : FontWeight::Normal,
                                .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}));
      auto line = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
      if (p != nullptr) {
        line->addChild(ui::box({
            .fill = colorSpecFromRole(dotRole(*p)),
            .radius = kStatusDotSize * 0.5F * scale,
            .width = kStatusDotSize * scale,
            .height = kStatusDotSize * scale,
        }));
      }
      const ColorRole role = error != nullptr ? ColorRole::DangerText
          : p != nullptr && p->state == PrinterState::NeedsAttention ? ColorRole::Warning
                                                                      : ColorRole::Label2;
      line->addChild(ui::label({.text = error != nullptr ? *error : status,
                                .fontSize = Tokens::typeFootnote.size * scale, .color = colorSpecFromRole(role),
                                .maxLines = 1, .flexGrow = 1.0F}));
      copy->addChild(std::move(line));
      return copy;
    }

    std::unique_ptr<Node> tag(std::string text, float scale) {
      return ui::row(
          {.align = FlexAlign::Center,
           .paddingH = Style::spaceXs * 1.5F * scale,
           .fill = colorSpecFromRole(ColorRole::Fill1),
           .radius = Style::spaceXs * 1.5F * scale,
           .height = Style::spaceLg * scale},
          ui::label({.text = std::move(text), .fontSize = Tokens::typeFootnote.size * scale,
                     .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2)})
      );
    }

    void addPrinterRow(Flex& card, const PrinterInfo& p, const SettingsPrintersContext& ctx) {
      const float scale = ctx.scale;
      PrintersModel* model = ctx.model;
      auto r = row(scale);
      r->setGap(Style::spaceMd * scale);
      r->setMinHeight(Tokens::spaceP48 * scale);
      r->addChild(roundSymbol("printer", kIcon, scale));
      r->addChild(nameAndStatus(p.label, &p, model->error(p.name), statusText(p), false, scale));
      if (p.isDefault) {
        r->addChild(tag(i18n::tr("settings.printers.default"), scale));
      }
      if (p.paused) {
        r->addChild(plainButton(i18n::tr("settings.printers.resume"), ButtonVariant::Primary, scale,
                                [model, name = p.name] { model->act(name, [name] { return resumePrinter(name); }); }));
      }
      r->addChild(plainButton(i18n::tr("settings.printers.details"), ButtonVariant::Ghost, scale,
                              [open = ctx.openDetail, id = detailId(p)] { open(id); }));
      card.addChild(std::move(r));
    }

    void addJobRow(Flex& card, const JobInfo& j, bool indent, const SettingsPrintersContext& ctx) {
      const float scale = ctx.scale;
      auto r = row(scale);
      r->setGap(Style::spaceMd * scale);
      r->setMinHeight(Tokens::sizeRowTwo * scale);
      if (indent) {
        r->addChild(ui::row({.width = kIcon * scale}));
      }
      auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
      text->addChild(ui::label({.text = j.title, .fontSize = Style::fontSizeBody * scale,
                                .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}));
      text->addChild(ui::label({.text = jobText(j), .fontSize = Tokens::typeFootnote.size * scale,
                                .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1}));
      r->addChild(std::move(text));
      r->addChild(plainButton(i18n::tr("settings.printers.cancel"), ButtonVariant::Default, scale,
                              [model = ctx.model, printer = j.printer, id = j.id] { model->cancelJob(printer, id); }));
      card.addChild(std::move(r));
    }

    std::vector<JobInfo> jobsOf(const PrintersModel& model, const std::string& printer) {
      std::vector<JobInfo> out;
      std::ranges::copy_if(model.jobs(), std::back_inserter(out),
                           [&](const JobInfo& j) { return j.printer == printer; });
      return out;
    }

    void addDefaults(Flex& content, const PrinterInfo& p, const SettingsPrintersContext& ctx) {
      const float scale = ctx.scale;
      PrintersModel* model = ctx.model;
      Flex* card = addSection(content, i18n::tr("settings.printers.defaults"), nullptr, scale);
      if (!p.media.empty()) {
        std::vector<std::string> labels;
        std::ranges::transform(p.media, std::back_inserter(labels), mediaLabel);
        const auto it = std::ranges::find(p.media, p.defaultMedia);
        auto r = labelRow({}, i18n::tr("settings.printers.paper"), scale);
        r->addChild(ui::select({
            .options = std::move(labels),
            .selectedIndex = it == p.media.end() ? std::optional<std::size_t>{} : std::size_t(it - p.media.begin()),
            .fontSize = Style::fontSizeBody * scale,
            .glyphSize = Tokens::sizeSymbolInline * scale,
            .onSelectionChanged = [model, name = p.name, media = p.media](std::size_t i, std::string_view) {
              model->act(name, [name, value = media[i]] { return setPrinterDefault(name, "media", value); });
            },
        }));
        card->addChild(std::move(r));
      }
      if (p.duplex) {
        static constexpr std::array<const char*, 3> kSides = {"one-sided", "two-sided-long-edge",
                                                              "two-sided-short-edge"};
        const auto it = std::ranges::find(kSides, p.defaultSides);
        auto r = labelRow({}, i18n::tr("settings.printers.two-sided"), scale);
        r->addChild(ui::segmented({
            .options = std::vector<ui::SegmentedOption>{{.label = i18n::tr("settings.printers.sides-off")},
                                                    {.label = i18n::tr("settings.printers.sides-long")},
                                                    {.label = i18n::tr("settings.printers.sides-short")}},
            .selectedIndex = it == kSides.end() ? 0 : std::size_t(it - kSides.begin()),
            .scale = scale,
            .onChange = [model, name = p.name](std::size_t i) {
              model->act(name, [name, i] { return setPrinterDefault(name, "sides", kSides[i]); });
            },
        }));
        card->addChild(std::move(r));
      }
      if (!p.colour) {
        addValueRow(*card, i18n::tr("settings.printers.colour"), i18n::tr("settings.printers.mono-only"), scale);
      }
    }

    void addDetail(Flex& content, const SettingsPrintersContext& ctx) {
      const float scale = ctx.scale;
      PrintersModel* model = ctx.model;
      const auto tab = ctx.detail.find('\t');
      const std::string name = tab == std::string::npos ? std::string{} : ctx.detail.substr(tab + 1);
      const auto& printers = model->printers();
      const auto it = std::ranges::find(printers, name, &PrinterInfo::name);
      if (it == printers.end()) {
        addEmptyState(content, "printer-off", i18n::tr("settings.printers.gone"), scale);
        return;
      }
      const PrinterInfo& p = *it;

      Flex* hero = addCard(content, scale);
      auto top = row(scale);
      top->setGap(Style::spaceMd * scale);
      top->setMinHeight(Tokens::spaceP48 * scale);
      top->addChild(roundSymbol("printer", kIcon, scale));
      std::string where = p.uri;
      if (!p.location.empty()) {
        where += " · " + p.location;
      }
      auto copy = nameAndStatus(p.label, &p, model->error(p.name), statusText(p), true, scale);
      copy->addChild(ui::label({.text = std::move(where), .fontSize = Tokens::typeFootnote.size * scale,
                                .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1}));
      top->addChild(std::move(copy));
      if (p.paused) {
        top->addChild(plainButton(i18n::tr("settings.printers.resume"), ButtonVariant::Primary, scale,
                                  [model, n = p.name] { model->act(n, [n] { return resumePrinter(n); }); }));
      }
      hero->addChild(std::move(top));

      Flex* card = addCard(content, scale);
      addSwitchRow(*card, {}, i18n::tr("settings.printers.default-printer"), p.isDefault, scale,
                   [model, n = p.name](bool on) {
                     model->act(n, [n, on] { return setDefaultPrinter(on ? n : std::string{}); });
                   });

      const std::vector<JobInfo> jobs = jobsOf(*model, p.name);
      Flex* queue = addSection(content, i18n::tr("settings.printers.queue"),
                ui::label({.text = i18n::tr(jobs.size() == 1 ? "settings.printers.one-job" : "settings.printers.jobs",
                                            "count", std::to_string(jobs.size())),
                           .fontSize = Tokens::typeFootnote.size * scale, .color = colorSpecFromRole(ColorRole::Label2)}),
                scale);
      if (jobs.empty()) {
        addValueRow(*queue, i18n::tr("settings.printers.no-jobs"), {}, scale);
      }
      for (const auto& j : jobs) {
        addJobRow(*queue, j, false, ctx);
      }
      if (jobs.size() > 1) {
        addActionRow(*queue, i18n::tr("settings.printers.cancel-all"), false, scale,
                     [model, n = p.name] { model->act(n, [n] { return cancelAllJobs(n); }); });
      }
      if (!p.paused) {
        addActionRow(*queue, i18n::tr("settings.printers.pause"), false, scale,
                     [model, n = p.name] { model->act(n, [n] { return pausePrinter(n); }); });
      }

      addDefaults(content, p, ctx);

      Flex* actions = addCard(content, scale);
      addActionRow(*actions, i18n::tr("settings.printers.test-page"), false, scale,
                   [model, n = p.name] { model->act(n, [n] { return printTestPage(n); }); });
      auto remove = row(scale);
      remove->addChild(plainButton(i18n::tr("settings.printers.remove"), ButtonVariant::Destructive, scale,
                                   [model, n = p.name, back = ctx.back] {
                                     model->act(n, [n] { return removePrinter(n); }, back);
                                   }));
      actions->addChild(std::move(remove));
    }

    void addNearby(Flex& content, const SettingsPrintersContext& ctx) {
      const float scale = ctx.scale;
      PrintersModel* model = ctx.model;
      const bool looking = model->discovery() == PrintersModel::Discovery::Running;
      std::unique_ptr<Node> trailing;
      if (looking) {
        trailing = ui::spinner({.spinnerSize = Tokens::sizeSymbolInline * scale, .spinning = true});
      } else {
        trailing = iconButton("refresh", i18n::tr("settings.printers.scan"), scale, [model] { model->discover(); });
      }
      Flex* card = addSection(content, i18n::tr("settings.printers.nearby"), std::move(trailing), scale);
      const std::vector<FoundPrinter> nearby = model->nearby();
      if (nearby.empty()) {
        auto r = row(scale);
        r->addChild(ui::label({.text = i18n::tr(looking ? "settings.printers.looking" : "settings.printers.none-found"),
                               .fontSize = Style::fontSizeBody * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
        card->addChild(std::move(r));
      }
      const bool busy = model->adding() && !model->adding()->error;
      for (const auto& f : nearby) {
        auto r = row(scale);
        r->setGap(Style::spaceMd * scale);
        r->setMinHeight(Tokens::spaceP48 * scale);
        r->addChild(roundSymbol("printer", kIcon, scale));
        r->addChild(nameAndStatus(f.label, nullptr, nullptr, f.uri, false, scale));
        if (!busy) {
          r->addChild(plainButton(i18n::tr("settings.printers.add"), ButtonVariant::Default, scale,
                                  [model, f] { model->add(f); }));
        }
        card->addChild(std::move(r));
      }
      addActionRow(*card, i18n::tr("settings.printers.add-by-address"), false, scale, ctx.addByAddress);
      addFootnote(*card, i18n::tr("settings.printers.driverless"), scale);
    }

  } // namespace

  void addSettingsPrinters(Flex& content, const SettingsPrintersContext& ctx) {
    const float scale = ctx.scale;
    PrintersModel* model = ctx.model;
    model->show();
    if (model->service() == PrintersModel::Service::Unknown) {
      return;
    }
    if (model->service() == PrintersModel::Service::Missing) {
      addEmptyState(content, "printer-off", i18n::tr("settings.printers.unavailable"), scale);
      return;
    }
    if (ctx.detail.starts_with("printer:")) {
      addDetail(content, ctx);
      return;
    }

    const auto& printers = model->printers();
    if (printers.empty()) {
      Flex* card = addCard(content, scale);
      auto r = row(scale);
      r->setGap(Style::spaceMd * scale);
      r->setMinHeight(Tokens::spaceP48 * scale);
      r->addChild(roundSymbol("printer", kIcon, scale));
      r->addChild(nameAndStatus(i18n::tr("settings.printers.no-printers"), nullptr, nullptr,
                                i18n::tr("settings.printers.no-printers-hint"), false, scale));
      card->addChild(std::move(r));
    } else {
      Flex* card = addSection(content, i18n::tr("settings.printers.mine"), nullptr, scale);
      for (const auto& p : printers) {
        addPrinterRow(*card, p, ctx);
        const std::vector<JobInfo> jobs = jobsOf(*model, p.name);
        for (std::size_t i = 0; i < std::min(jobs.size(), kJobsOnPage); ++i) {
          addJobRow(*card, jobs[i], true, ctx);
        }
        if (jobs.size() > kJobsOnPage) {
          addActionRow(*card, i18n::tr("settings.printers.more-jobs", "count", std::to_string(jobs.size() - kJobsOnPage)),
                       true, scale, [open = ctx.openDetail, id = detailId(p)] { open(id); });
        }
      }
    }

    if (const auto& adding = model->adding()) {
      Flex* card = addCard(content, scale);
      auto r = row(scale);
      r->setGap(Style::spaceMd * scale);
      r->setMinHeight(Tokens::spaceP48 * scale);
      r->addChild(roundSymbol("printer", kIcon, scale));
      const std::string status = adding->error ? *adding->error : i18n::tr("settings.printers.adding");
      r->addChild(nameAndStatus(adding->printer.label, nullptr, adding->error ? &*adding->error : nullptr, status,
                                false, scale));
      if (adding->error) {
        r->addChild(plainButton(i18n::tr("settings.printers.retry"), ButtonVariant::Default, scale,
                                [model, f = adding->printer] { model->add(f); }));
      } else {
        r->addChild(ui::spinner({.spinnerSize = Tokens::sizeSymbolInline * scale, .spinning = true}));
      }
      card->addChild(std::move(r));
    }

    addNearby(content, ctx);
  }

} // namespace settings
