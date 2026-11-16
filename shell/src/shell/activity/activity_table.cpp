#include "shell/activity/activity_ui.h"
#include "shell/activity/activity_window.h"

#include "dbus/upower/upower_service.h"
#include "render/scene/accessible.h"
#include "shell/settings/settings_selection_style.h"
#include "core/deferred_call.h"
#include "system/activity_monitor.h"
#include "system/battery_impact.h"
#include "system/system_monitor_service.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"

#include <ctime>

using namespace activity_ui;

namespace {

  constexpr double kPressureHigh = 10.0; // avg10 %: apps waited for memory a tenth of the time
  constexpr double kRamHigh = 90.0;
  constexpr double kCpuBusy = 90.0;
  constexpr std::size_t kCpuBusySamples = 10;

  std::uint64_t nowUsec() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count()
    );
  }

  std::string percentText(double value) { return std::format("{:.0f} %", value); }

  std::string upTimeText() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    const auto minutes = ts.tv_sec / 60;
    return std::format("{} h {} m", minutes / 60, minutes % 60);
  }

  // Strip chip (mockup .chipk): 20 high capsule, an 8 colour key, caption text.
  std::unique_ptr<Flex> chip(Label** out, std::string text, ColorRole key, bool warn, float scale) {
    auto pill = ui::row({
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP4 * scale,
        .paddingH = Style::spaceSm * scale,
        .fill = warn ? colorSpecFromRole(ColorRole::Warning, 0.14F) : colorSpecFromRole(ColorRole::Fill1),
        .radius = Tokens::spaceP20 * scale / 2.0F,
        .height = Tokens::spaceP20 * scale,
    });
    if (!warn) {
      pill->addChild(ui::box({
          .fill = colorSpecFromRole(key),
          .radius = Tokens::spaceP2 * scale,
          .width = Tokens::spaceP8 * scale,
          .height = Tokens::spaceP8 * scale,
      }));
    }
    pill->addChild(ui::label({
        .out = out,
        .text = std::move(text),
        .fontSize = Tokens::typeCaption.size * scale,
        .fontWeight = FontWeight::Medium,
        .fontFeatures = std::string("tnum"),
        .color = colorSpecFromRole(warn ? ColorRole::Warning : ColorRole::Label1),
    }));
    return pill;
  }

} // namespace

std::vector<ActivityWindow::Column> ActivityWindow::columnsFor(Pane pane) const {
  const Column cpu{
      .header = i18n::tr("activity.column-cpu"),
      .text = [](const ActivityRow& r) { return cpuText(r.cpuPercent); },
      .value = [](const ActivityRow& r) { return r.cpuPercent; },
      .group = 'c',
  };
  const Column memory{
      .header = i18n::tr("activity.column-memory"),
      .text = [](const ActivityRow& r) { return memoryText(r.memoryBytes); },
      .value = [](const ActivityRow& r) { return static_cast<double>(r.memoryBytes); },
      .group = 'm',
  };
  const Column gpu{
      .header = i18n::tr("activity.column-gpu"),
      .text = [this](const ActivityRow& r) { return percentText(gpuOf(r)); },
      .value = [this](const ActivityRow& r) { return gpuOf(r); },
      .muted = true,
  };
  const Column running{
      .header = i18n::tr("activity.column-running"),
      .text = [](const ActivityRow& r) { return uptimeText(r.startedUsec); },
      .value = [](const ActivityRow& r) {
        return r.startedUsec == 0 ? 0.0 : static_cast<double>(nowUsec() - std::min(nowUsec(), r.startedUsec));
      },
      .muted = true,
  };
  const Column energy{
      .header = i18n::tr("activity.column-energy"),
      .text = [this](const ActivityRow& r) { return energyText(energyBars(r.cpuPercent, gpuOf(r))); },
      .value = [this](const ActivityRow& r) {
        return energyBars(r.cpuPercent, gpuOf(r)) * 1000.0 + r.cpuPercent + gpuOf(r);
      },
  };
  const Column threads{
      .header = i18n::tr("activity.column-threads"),
      .text = [](const ActivityRow& r) { return std::to_string(r.threads); },
      .value = [](const ActivityRow& r) { return static_cast<double>(r.threads); },
      .muted = true,
  };
  const auto muted = [](Column column) {
    column.muted = true;
    return column;
  };
  switch (pane) {
  case Pane::Cpu:
    return {
        cpu,
        {.header = i18n::tr("activity.column-cpu-time"),
         .text = [](const ActivityRow& r) { return clockText(r.cpuUsageUsec); },
         .value = [](const ActivityRow& r) { return static_cast<double>(r.cpuUsageUsec); },
         .muted = true},
        threads,
        {.header = i18n::tr("activity.column-wakeups"),
         .text = [this](const ActivityRow& r) {
           const auto rate = m_details != nullptr ? m_details->wakeupsPerSec(r.key) : std::nullopt;
           return rate.has_value() ? std::format("{:.0f}", *rate) : std::string("–");
         },
         .value = [this](const ActivityRow& r) {
           return m_details != nullptr ? m_details->wakeupsPerSec(r.key).value_or(0.0) : 0.0;
         },
         .muted = true},
    };
  case Pane::Memory:
    return {
        memory,
        cpu,
        muted(energy),
        threads,
        {.header = i18n::tr("activity.column-user"),
         .text = [](const ActivityRow& r) {
           return i18n::tr(r.systemManager ? "activity.user-system" : "activity.user-you");
         },
         .value = [](const ActivityRow& r) { return r.systemManager ? 1.0 : 0.0; },
         .muted = true},
    };
  case Pane::Energy:
    return {energy, cpu, gpu, running};
  case Pane::Disk:
    return {
        {.header = i18n::tr("activity.column-read"),
         .text = [](const ActivityRow& r) { return rateText(r.ioReadBytesPerSec); },
         .value = [](const ActivityRow& r) { return r.ioReadBytesPerSec; }},
        {.header = i18n::tr("activity.column-written"),
         .text = [](const ActivityRow& r) { return rateText(r.ioWriteBytesPerSec); },
         .value = [](const ActivityRow& r) { return r.ioWriteBytesPerSec; }},
        muted(cpu),
        muted(memory),
    };
  case Pane::Network:
  case Pane::Containers:
    return {
        {.header = i18n::tr("activity.column-network"),
         .text = [this](const ActivityRow& r) {
           const auto rate = m_details != nullptr ? m_details->networkBytesPerSec(r.key) : std::nullopt;
           return rate.has_value() ? rateText(*rate) : std::string("–");
         },
         .value = [this](const ActivityRow& r) {
           return m_details != nullptr ? m_details->networkBytesPerSec(r.key).value_or(0.0) : 0.0;
         }},
        muted(cpu),
        muted(memory),
    };
  }
  return {};
}

std::size_t ActivityWindow::sortColumn() const {
  const auto it = m_sortBy.find(m_pane);
  return it != m_sortBy.end() ? it->second : 0;
}

void ActivityWindow::computeOrder() {
  m_orderAt = std::chrono::steady_clock::now();
  m_order.clear();
  if (m_monitor == nullptr || m_pane == Pane::Containers) {
    return;
  }
  const auto columns = columnsFor(m_pane);
  const auto& value = columns[std::min(sortColumn(), columns.size() - 1)].value;
  std::vector<std::pair<int, const ActivityRow*>> rows;
  for (const auto& row : m_monitor->activity().rows()) {
    if (matchesQuery(row)) {
      rows.emplace_back(row.systemManager ? 2 : row.hasWindow ? 0 : 1, &row);
    }
  }
  std::vector<double> values;
  values.reserve(rows.size());
  for (const auto& [section, row] : rows) {
    values.push_back(value(*row));
  }
  std::vector<std::size_t> index(rows.size());
  for (std::size_t i = 0; i < index.size(); ++i) {
    index[i] = i;
  }
  std::ranges::stable_sort(index, [&](std::size_t a, std::size_t b) {
    if (rows[a].first != rows[b].first) {
      return rows[a].first < rows[b].first;
    }
    return values[a] > values[b];
  });
  for (const std::size_t i : index) {
    m_order.push_back(rows[i].second->key);
  }
}

ActivityWindow::Problem ActivityWindow::problem(std::string* rowKey) const {
  const auto& rows = m_monitor->activity().rows();
  const auto top = [&](auto value) -> const ActivityRow* {
    const ActivityRow* best = nullptr;
    for (const auto& row : rows) {
      if (!row.systemManager && (best == nullptr || value(row) > value(*best))) {
        best = &row;
      }
    }
    return best;
  };
  const double pressure = m_pressureHistory.empty() ? 0.0 : m_pressureHistory.back();
  const double ram = m_sysmon != nullptr ? m_sysmon->latest().ramUsagePercent : 0.0;
  if (pressure >= kPressureHigh || ram >= kRamHigh) {
    if (const ActivityRow* row = top([](const ActivityRow& r) { return r.memoryBytes; }); row != nullptr) {
      *rowKey = row->key;
      return Problem::Memory;
    }
  }
  const auto cpuBusy = [this] {
    const auto stats = m_sysmon->history();
    return stats.size() >= kCpuBusySamples
        && std::all_of(stats.end() - kCpuBusySamples, stats.end(), [](const SystemStats& s) {
             return s.cpuUsagePercent >= kCpuBusy;
           });
  };
  if (m_sysmon != nullptr && m_sysmon->latest().cpuUsagePercent >= kCpuBusy && cpuBusy()) {
    if (const ActivityRow* row = top([](const ActivityRow& r) { return r.cpuPercent; }); row != nullptr) {
      *rowKey = row->key;
      return Problem::Cpu;
    }
  }
  for (const auto& row : rows) {
    if (needsAttention(row)) {
      *rowKey = row.key;
      return Problem::Background;
    }
  }
  return Problem::None;
}

std::unique_ptr<Flex> ActivityWindow::buildToolbar(float scale) {
  auto bar = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
      .height = kToolbarHeight * scale,
      .configure = [scale](Flex& flex) { flex.setPadding(0.0F, Style::spaceMd * scale, 0.0F, Style::spaceLg * scale); },
  });
  Label* count = nullptr;
  bar->addChild(ui::column(
      {.align = FlexAlign::Start},
      text(i18n::tr("activity.title"), Tokens::typeTitle3.size * scale, ColorRole::Label1, FontWeight::SemiBold),
      text(
          i18n::tr("activity.count", "count", std::to_string(m_monitor->activity().rows().size())),
          Style::fontSizeCaption * scale, ColorRole::Label2, FontWeight::Medium, &count
      )
  ));
  m_liveText.push_back({count, [this] {
                          return i18n::tr("activity.count", "count", std::to_string(m_monitor->activity().rows().size()));
                        }});
  bar->addChild(ui::spacer());

  std::vector<ui::SegmentedOption> panes;
  for (const char* key : {"activity.pane-cpu", "activity.pane-memory", "activity.pane-energy", "activity.pane-disk",
                          "activity.pane-network"}) {
    panes.push_back({.label = i18n::tr(key)});
  }
  if (containersAvailable()) {
    panes.push_back({.label = i18n::tr("activity.tab-containers")});
  }
  bar->addChild(ui::segmented({
      .options = std::move(panes),
      .selectedIndex = static_cast<std::size_t>(m_pane),
      .fontSize = Style::fontSizeBody * scale,
      .scale = scale,
      .onChange = [this](std::size_t index) { setPane(static_cast<Pane>(index)); },
  }));
  bar->addChild(ui::spacer());

  if (m_keptSearch != nullptr && m_searchScale == scale) {
    bar->addChild(std::move(m_keptSearch));
    m_searchInput = std::exchange(m_keptSearchInput, nullptr);
  } else {
    m_keptSearch.reset();
    m_keptSearchInput = nullptr;
    m_searchScale = scale;
    bar->addChild(ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .paddingH = Tokens::spaceP8 * scale,
         .fill = colorSpecFromRole(ColorRole::Fill1),
         .radius = Tokens::radiusR8 * scale,
         .width = kSearchWidth * scale,
         .height = Style::controlHeight * scale},
        ui::glyph({
            .glyph = "search",
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
        }),
        ui::input({
            .out = &m_searchInput,
            .value = m_query,
            .placeholder = i18n::tr("activity.search"),
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .horizontalPadding = 0.0F,
            .frameVisible = false,
            .flexGrow = 1.0F,
            .onChange = [this](const std::string& query) {
              m_query = query;
              computeOrder();
              requestRebuild();
            },
        })
    ));
  }
  if (Style::closeButtonsEnabled()) {
    bar->addChild(ui::button({
        .glyph = "close",
        .glyphSize = Tokens::sizeSymbolInline * scale,
        .variant = ButtonVariant::Default,
        .minWidth = Tokens::sizeControlS * scale,
        .minHeight = Tokens::sizeControlS * scale,
        .padding = 0.0F,
        .radius = Tokens::sizeControlS * 0.5F * scale,
        .onClick = [this] { DeferredCall::callLater([this] { close(); }); },
        .configure = [](Button& button) {
          button.setTabStop(false);
          button.accessibleState().name = i18n::tr("activity.close");
        },
    }));
  }
  return bar;
}

std::unique_ptr<Flex> ActivityWindow::buildProblemCard(float scale) {
  std::string key;
  const Problem kind = problem(&key);
  const ActivityRow* row = kind != Problem::None ? findRow(key) : nullptr;
  if (row == nullptr) {
    return nullptr;
  }
  const std::string name = identity(*row).name;
  std::string title;
  std::string action;
  std::function<std::string()> body;
  switch (kind) {
  case Problem::Memory:
    title = i18n::tr("activity.problem-memory-title");
    action = i18n::tr("activity.action-quit-app", "name", name);
    body = [this, key] {
      const ActivityRow* live = findRow(key);
      if (live == nullptr) {
        return std::string();
      }
      const double pressure = m_pressureHistory.empty() ? 0.0 : m_pressureHistory.back();
      return i18n::tr(
          "activity.problem-memory-body", "percent", std::format("{:.0f}", pressure), "name", identity(*live).name,
          "memory", memoryText(live->memoryBytes)
      );
    };
    break;
  case Problem::Cpu:
    title = i18n::tr("activity.problem-cpu-title");
    action = i18n::tr("activity.action-show-app", "name", name);
    body = [this, key] {
      const ActivityRow* live = findRow(key);
      return live != nullptr ? i18n::tr("activity.problem-cpu-body", "name", identity(*live).name, "cpu",
                                        cpuText(live->cpuPercent))
                             : std::string();
    };
    break;
  case Problem::Background: {
    std::size_t count = 0;
    for (const auto& r : m_monitor->activity().rows()) {
      count += needsAttention(r) ? 1 : 0;
    }
    title = count == 1 ? i18n::tr("activity.problem-background-one", "name", name)
                       : i18n::tr("activity.problem-background-many", "count", std::to_string(count));
    action = i18n::tr("activity.action-review");
    body = [this, key] {
      const ActivityRow* live = findRow(key);
      const auto* state = rowState(key);
      return live != nullptr && state != nullptr ? BackgroundActivityMonitor::reasonText(*state, live->memoryBytes)
                                                 : std::string();
    };
    break;
  }
  case Problem::None:
    return nullptr;
  }

  Label* bodyLabel = nullptr;
  auto card = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
      .paddingV = Style::spaceMd * scale,
      .paddingH = Style::spaceLg * scale,
      .fill = m_glass ? colorSpecFromRole(ColorRole::SolidThin, 0.5F)
                      : fixedColorSpec(rgbaHex(isResolvedLightTheme() ? Tokens::schemeLight.card : Tokens::schemeDark.card)),
      .radius = Tokens::radiusR8 * scale,
      .border = colorSpecFromRole(m_glass ? ColorRole::LensEdge : ColorRole::Stroke),
      .borderWidth = 0.5F * scale,
  });
  card->addChild(ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .fill = colorSpecFromRole(ColorRole::Warning, 0.18F),
       .radius = Tokens::sizeControlM * scale / 2.0F,
       .width = Tokens::sizeControlM * scale,
       .height = Tokens::sizeControlM * scale},
      ui::glyph({
          .glyph = "alert-triangle",
          .glyphSize = Tokens::sizeSymbol * scale,
          .color = colorSpecFromRole(ColorRole::Warning),
      })
  ));
  auto words = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  words->addChild(text(title, Tokens::typeHeadline.size * scale, ColorRole::Label1, FontWeight::SemiBold));
  words->addChild(ui::label({
      .out = &bodyLabel,
      .text = body(),
      .fontSize = Style::fontSizeCaption * scale,
      .fontWeight = FontWeight::Medium,
      .color = colorSpecFromRole(ColorRole::Label2),
      .maxLines = 2,
      .ellipsize = TextEllipsize::End,
  }));
  card->addChild(std::move(words));
  m_liveText.push_back({bodyLabel, std::move(body)});
  card->addChild(ui::button({
      .text = action,
      .fontSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Default,
      .minHeight = Style::controlHeight * scale,
      .paddingH = Style::spaceMd * scale,
      .radius = Tokens::radiusR8 * scale,
      .onClick = [this, key, kind] {
        select(key);
        if (kind == Problem::Memory) {
          m_confirm = Confirm::Quit;
        }
      },
  }));
  auto wrap = ui::column({.align = FlexAlign::Stretch});
  wrap->setPadding(0.0F, Style::spaceLg * scale, Style::spaceSm * scale, Style::spaceLg * scale);
  wrap->addChild(std::move(card));
  return wrap;
}

std::uint64_t ActivityWindow::TableAdapter::itemKey(std::size_t index) const {
  const TableItem& item = m_window.m_tableItems[index];
  return std::hash<std::string>{}(std::format("{}|{}|{}|{}", static_cast<int>(item.kind), item.section, item.key, item.group));
}

bool ActivityWindow::TableAdapter::itemInteractive(std::size_t index) const {
  return m_window.m_tableItems[index].kind == TableItem::Kind::Row;
}

float ActivityWindow::TableAdapter::measureItem(Renderer& /*renderer*/, std::size_t /*index*/, float /*width*/) {
  return kRowHeight * m_window.uiScale();
}

std::unique_ptr<Node> ActivityWindow::TableAdapter::createItem() {
  auto root = ui::row({.align = FlexAlign::Center, .radius = Tokens::radiusR4 * m_window.uiScale()});
  m_window.m_tableSlots[root.get()].root = root.get();
  return root;
}

void ActivityWindow::TableAdapter::bindItem(
    Renderer& renderer, Node& item, std::size_t index, float /*width*/, bool hovered
) {
  m_window.bindTableSlot(renderer, m_window.m_tableSlots.at(&item), m_window.m_tableItems[index], hovered);
}

void ActivityWindow::TableAdapter::onActivate(std::size_t index) { m_window.select(m_window.m_tableItems[index].key); }

void ActivityWindow::rebuildTableItems() {
  m_tableItems.clear();
  std::size_t section = SIZE_MAX;
  for (const auto& key : m_order) {
    const ActivityRow* row = findRow(key);
    if (row == nullptr) {
      continue;
    }
    const std::size_t rowSection = row->systemManager ? 2 : row->hasWindow ? 0 : 1;
    if (rowSection != section) {
      section = rowSection;
      m_tableItems.push_back({.kind = TableItem::Kind::Section, .section = section});
    }
    m_tableItems.push_back({.kind = TableItem::Kind::Row, .section = section, .key = key});
    const auto* groups = m_details != nullptr && m_details->expanded(key) ? m_details->groups(key) : nullptr;
    for (std::size_t i = 0; groups != nullptr && i < groups->size(); ++i) {
      m_tableItems.push_back({.kind = TableItem::Kind::Child, .section = section, .key = key, .group = i});
    }
  }
}

void ActivityWindow::buildTableSlot(TableSlot& slot, TableItem::Kind kind) {
  const float scale = uiScale();
  Flex& root = *slot.root;
  while (!root.children().empty()) {
    root.removeChild(root.children().back().get());
  }
  slot = TableSlot{.kind = kind, .built = true, .root = &root};
  const auto label = [&](Label** out, ColorRole role, TextAlign align, bool numeric) {
    return ui::label({
        .out = out,
        .fontSize = Style::fontSizeBody * scale,
        .fontFeatures = numeric ? std::optional<std::string>("tnum") : std::nullopt,
        .color = colorSpecFromRole(role),
        .maxLines = 1,
        .textAlign = align,
        .ellipsize = numeric ? std::nullopt : std::optional(TextEllipsize::Middle),
        .flexGrow = numeric ? 0.0F : 1.0F,
    });
  };
  if (kind == TableItem::Kind::Section) {
    root.setAlign(FlexAlign::End);
    root.setPadding(0.0F, Style::spaceMd * scale);
    root.addChild(ui::label({
        .out = &slot.name,
        .fontSize = Style::fontSizeCaption * scale,
        .fontWeight = FontWeight::SemiBold,
        .color = colorSpecFromRole(ColorRole::Label2),
        .maxLines = 1,
    }));
    return;
  }
  root.setAlign(FlexAlign::Center);
  root.setPadding(0.0F);
  auto name = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .width = m_nameWidth,
                       .height = kRowHeight * scale});
  if (kind == TableItem::Kind::Child) {
    name->setPadding(0.0F, Style::spaceMd * scale, 0.0F, kChildIndent * scale);
    name->addChild(label(&slot.name, ColorRole::Label2, TextAlign::Start, false));
  } else {
    name->setPadding(0.0F, Style::spaceMd * scale);
    if (m_details != nullptr) {
      name->addChild(ui::button({
          .variant = ButtonVariant::Ghost,
          .padding = 0.0F,
          .width = Tokens::sizeSymbol * scale,
          .height = Tokens::sizeSymbol * scale,
          .onClick = [this, root = &root] {
            const std::string& key = m_tableSlots.at(root).key;
            m_details->setExpanded(key, !m_details->expanded(key)); // groups fill in on the next sample
            rebuildTableItems();
            if (m_tableList != nullptr) {
              m_tableList->notifyDataChanged();
            }
            invalidate();
          },
          .configure =
              [&](Button& disclosure) {
                disclosure.setTabStop(false);
                disclosure.addChild(ui::glyph({
                    .out = &slot.disclosure,
                    .glyph = "chevron-right",
                    .glyphSize = Tokens::sizeSymbolInline * scale,
                    .color = colorSpecFromRole(ColorRole::Label2),
                }));
              },
      }));
    }
    name->addChild(ui::image({.out = &slot.icon, .width = Tokens::sizeSymbol * scale,
                              .height = Tokens::sizeSymbol * scale}));
    name->addChild(label(&slot.name, ColorRole::Label1, TextAlign::Start, false));
    name->addChild(ui::label({
        .out = &slot.count,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Label3),
        .maxLines = 1,
    }));
  }
  root.addChild(std::move(name));
  for (std::size_t i = 0; i < m_tableColumns.size(); ++i) {
    Flex* cell = nullptr;
    Label* value = nullptr;
    root.addChild(ui::row(
        {.out = &cell,
         .align = FlexAlign::Center,
         .justify = FlexJustify::End,
         .paddingH = Style::spaceMd * scale,
         .width = m_columnWidth,
         .height = kRowHeight * scale},
        label(&value, ColorRole::Label1, TextAlign::End, true)
    ));
    slot.cells.push_back(cell);
    slot.values.push_back(value);
  }
  auto status = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::End,
       .paddingH = Style::spaceSm * scale,
       .width = kStatusWidth * scale,
       .height = kRowHeight * scale}
  );
  if (kind == TableItem::Kind::Row) {
    status->addChild(ui::glyph({.out = &slot.status, .glyphSize = Tokens::sizeSymbolInline * scale}));
  }
  root.addChild(std::move(status));
}

void ActivityWindow::bindTableSlot(Renderer& renderer, TableSlot& slot, const TableItem& item, bool hovered) {
  static constexpr std::array<const char*, 3> kSections{"activity.group-apps", "activity.tab-background",
                                                        "activity.group-system"};
  if (!slot.built || slot.kind != item.kind) {
    buildTableSlot(slot, item.kind);
  }
  slot.key = item.key;
  if (item.kind == TableItem::Kind::Section) {
    slot.name->setText(i18n::tr(kSections[item.section]));
    return;
  }
  const ActivityRow* row = findRow(item.key);
  if (item.kind == TableItem::Kind::Child) {
    const auto* groups = m_details != nullptr ? m_details->groups(item.key) : nullptr;
    const ProcessGroupUsage* group = groups != nullptr && item.group < groups->size() ? &(*groups)[item.group] : nullptr;
    slot.name->setText(
        group == nullptr       ? std::string()
            : group->count > 1 ? std::format("{} ({})", group->command, group->count)
                               : group->command
    );
    for (std::size_t i = 0; i < m_tableColumns.size(); ++i) {
      const char kind = m_tableColumns[i].group;
      slot.values[i]->setText(
          group == nullptr ? std::string()
              : kind == 'c' ? cpuText(group->cpuPercent)
              : kind == 'm' ? memoryText(group->residentBytes)
                            : std::string()
      );
    }
    slot.root->setFill(clearColorSpec());
    return;
  }
  if (row == nullptr) {
    return;
  }
  const float scale = uiScale();
  const bool selected = row->key == m_selectedKey;
  const auto& selection = settings::selectionStyle();
  slot.root->setFill(
      selected ? selection.rowFill : hovered ? colorSpecFromRole(ColorRole::Fill3) : clearColorSpec()
  );
  const RowIdentity id = identity(*row);
  Accessible& accessible = slot.root->accessibleState();
  accessible.role = AccessibleRole::ListItem;
  accessible.name = id.name;
  accessible.description = captionFor(*row);
  accessible.selected = selected;
  const int iconPx = static_cast<int>(std::round(Tokens::sizeSymbol * scale));
  if (std::string path = iconPath(id.icon, iconPx); path != slot.iconPath) {
    slot.icon->setSourceFile(renderer, path, iconPx);
    slot.iconPath = std::move(path);
  }
  slot.name->setText(id.name);
  slot.name->setColor(selected ? selection.rowLabel : colorSpecFromRole(ColorRole::Label1));
  const bool open = m_details != nullptr && m_details->expanded(row->key);
  if (slot.disclosure != nullptr) {
    slot.disclosure->setGlyph(open ? "chevron-down" : "chevron-right");
  }
  int processes = 0;
  if (const auto* groups = open ? m_details->groups(row->key) : nullptr; groups != nullptr) {
    for (const auto& group : *groups) {
      processes += group.count;
    }
  }
  slot.count->setText(processes > 0 ? std::to_string(processes) : std::string());
  slot.count->setVisible(processes > 0);
  slot.count->setParticipatesInLayout(processes > 0);
  const std::size_t sortIndex = sortColumn();
  for (std::size_t i = 0; i < m_tableColumns.size(); ++i) {
    const Column& column = m_tableColumns[i];
    slot.values[i]->setText(column.text(*row));
    slot.values[i]->setColor(
        selected                          ? selection.rowLabel
            : column.muted && i != sortIndex ? colorSpecFromRole(ColorRole::Label2)
                                             : colorSpecFromRole(ColorRole::Label1)
    );
    const float share = i == sortIndex && m_heatTop > 0.0
        ? static_cast<float>(std::clamp(column.value(*row) / m_heatTop, 0.0, 1.0))
        : 0.0F;
    slot.cells[i]->setFill(colorSpecFromRole(ColorRole::Accent, share * kHeatAlpha));
  }
  std::string glyph;
  ColorRole role = ColorRole::Label3;
  const Limits limits = m_limitedKeys.contains(row->key) ? limitsOf(*row) : Limits{};
  if (needsAttention(*row)) {
    glyph = "alert-triangle";
    role = ColorRole::Warning;
  } else if (m_limitedKeys.contains(row->key) && isFrozen(*row)) {
    glyph = "player-pause";
  } else if (limits.cpu != 0 || limits.memory != 0 || rowEfficiency(*row) == ActivityMonitor::Efficiency::On) {
    glyph = "leaf";
    role = ColorRole::Success;
  }
  slot.status->setVisible(!glyph.empty());
  if (!glyph.empty()) {
    slot.status->setGlyph(glyph);
    slot.status->setColor(colorSpecFromRole(role));
  }
}

std::unique_ptr<Flex> ActivityWindow::buildTable(Renderer& /*renderer*/, float width, float scale) {
  auto table = ui::column({.align = FlexAlign::Stretch, .paddingH = Style::spaceSm * scale, .flexGrow = 1.0F});
  auto columns = columnsFor(m_pane);
  // Columns drop from the right until the name keeps its minimum width.
  const float inner = width - 2.0F * Style::spaceSm * scale - kStatusWidth * scale;
  while (columns.size() > 1 && inner - static_cast<float>(columns.size()) * kColumnWidth * scale < kNameMinWidth * scale) {
    columns.pop_back();
  }
  // Past the name's comfortable width, the value columns take the extra room (grow then clamp).
  const auto count = static_cast<float>(columns.size());
  m_columnWidth = std::clamp((inner - kNameMaxWidth * scale) / count, kColumnWidth * scale, kColumnMaxWidth * scale);
  const float nameWidth = inner - count * m_columnWidth;
  const std::size_t sortIndex = sortColumn();

  auto header = ui::row({.align = FlexAlign::Center, .height = kRowHeight * scale});
  header->addChild(ui::row(
      {.align = FlexAlign::Center, .paddingH = Style::spaceMd * scale, .width = nameWidth},
      text(i18n::tr("activity.column-name"), Style::fontSizeCaption * scale, ColorRole::Label2, FontWeight::Medium)
  ));
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const bool sorted = i == sortIndex;
    header->addChild(ui::button({
        .text = sorted ? columns[i].header + " ▾" : columns[i].header,
        .fontSize = Style::fontSizeCaption * scale,
        .contentAlign = ButtonContentAlign::End,
        .variant = ButtonVariant::Ghost,
        .customPalette = Button::ButtonPalette{
            .normal = {.bg = clearColorSpec(), .border = clearColorSpec(),
                       .label = colorSpecFromRole(sorted ? ColorRole::Label1 : ColorRole::Label2)},
            .hover = {.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Label1)},
            .pressed = {.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Label1)},
            .disabled = {.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Label3)},
        },
        .paddingV = 0.0F,
        .paddingH = Style::spaceMd * scale,
        .width = m_columnWidth,
        .height = kRowHeight * scale,
        .onClick = [this, i] {
          m_sortBy[m_pane] = i;
          computeOrder();
          requestRebuild();
        },
        .configure = [](Button& button) { button.setTabStop(false); },
    }));
  }
  header->addChild(ui::node({.width = kStatusWidth * scale}));
  table->addChild(std::move(header));
  table->addChild(ui::separator({}));

  const bool sampled = m_monitor->activity().sampled();
  if (!sampled && std::chrono::steady_clock::now() - m_openedAt < kSkeletonFor) {
    for (int i = 0; i < kSkeletonRows; ++i) {
      table->addChild(ui::row(
          {.align = FlexAlign::Center,
           .gap = Style::spaceSm * scale,
           .paddingH = Style::spaceMd * scale,
           .height = kRowHeight * scale},
          ui::box({.fill = colorSpecFromRole(ColorRole::Fill2), .radius = Tokens::radiusR4 * scale,
                   .width = Tokens::sizeSymbol * scale, .height = Tokens::sizeSymbol * scale}),
          ui::box({.fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::radiusR4 * scale,
                   .width = nameWidth / static_cast<float>(2 + i % 3), .height = Tokens::spaceP12 * scale})
      ));
    }
    return table;
  }
  const auto centered = [&](std::unique_ptr<Node> first, std::unique_ptr<Node> second = nullptr) {
    auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceMd * scale,
                           .flexGrow = 1.0F});
    box->addChild(std::move(first));
    if (second != nullptr) {
      box->addChild(std::move(second));
    }
    table->addChild(std::move(box));
  };
  if (!sampled) {
    centered(text(i18n::tr("activity.loading"), Style::fontSizeBody * scale, ColorRole::Label2));
    return table;
  }
  if (m_order.empty()) {
    if (m_query.empty()) {
      centered(text(i18n::tr("activity.empty"), Style::fontSizeBody * scale, ColorRole::Label2));
    } else {
      centered(
          text(i18n::tr("activity.empty-search", "query", m_query), Style::fontSizeBody * scale, ColorRole::Label2),
          ui::button({
              .text = i18n::tr("activity.action-clear"),
              .fontSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Default,
              .minHeight = Style::controlHeight * scale,
              .paddingH = Style::spaceMd * scale,
              .radius = Tokens::radiusR8 * scale,
              .onClick = [this] {
                m_query.clear();
                if (m_searchInput != nullptr) {
                  m_searchInput->setValue("");
                }
                computeOrder();
                requestRebuild();
              },
          })
      );
    }
    return table;
  }

  if (sortIndex < columns.size()) {
    m_liveHeat.push_back({nullptr, [this, value = columns[sortIndex].value] {
                            m_heatTop = 0.0;
                            for (const auto& key : m_order) {
                              if (const ActivityRow* row = findRow(key); row != nullptr) {
                                m_heatTop = std::max(m_heatTop, value(*row));
                              }
                            }
                            return 0.0F;
                          }});
    m_liveHeat.front().share();
  }
  m_tableColumns = std::move(columns);
  m_nameWidth = nameWidth;
  rebuildTableItems();
  auto list = std::make_unique<VirtualListView>();
  list->setFlexGrow(1.0F);
  list->setContentScale(scale);
  list->bindScrollState(&m_listScroll);
  list->setAdapter(&m_tableAdapter);
  m_tableList = list.get();
  table->addChild(std::move(list));
  return table;
}

std::unique_ptr<Flex> ActivityWindow::buildStrip(float width, float scale) {
  auto strip = ui::row({
      .align = FlexAlign::Start,
      .gap = Style::spaceLg * scale + Style::spaceSm * scale,
      .paddingV = Style::spaceLg * scale,
      .paddingH = Tokens::spaceP24 * scale,
      .height = kStripHeight * scale,
  });
  const float gridWidth = (m_pane == Pane::Cpu ? kStripGridCpuWidth : kStripGridWidth) * scale;
  const float graphWidth = std::max(
      Tokens::spaceP64 * scale, width - 2.0F * Tokens::spaceP24 * scale - Tokens::spaceP24 * scale - gridWidth
  );
  const auto stats = [this] { return m_sysmon != nullptr ? m_sysmon->history() : std::vector<SystemStats>{}; };
  const auto last = [this] { return m_sysmon != nullptr ? m_sysmon->latest() : SystemStats{}; };
  const auto series = [stats](auto pick, float floor) {
    return [stats, pick, floor] {
      std::vector<float> values;
      for (const auto& sample : stats()) {
        values.push_back(static_cast<float>(pick(sample)));
      }
      return normalized(values, floor);
    };
  };
  const UPowerService* upower = m_upowerProvider ? m_upowerProvider() : nullptr;
  const bool battery = upower != nullptr && upower->state().isPresent;

  struct Cell {
    std::string label;
    std::function<std::string()> value;
  };
  std::string caption;
  std::vector<std::tuple<std::function<std::string()>, ColorRole, bool>> chips;
  std::function<std::vector<float>()> values;
  std::function<std::vector<float>()> values2; // second series, drawn in Label3
  ColorRole graphRole = ColorRole::Accent;
  std::vector<Cell> cells;
  const auto rowsSum = [this](auto pick) {
    double sum = 0.0;
    for (const auto& row : m_monitor->activity().rows()) {
      sum += pick(row);
    }
    return sum;
  };
  const auto peak = [](const std::deque<float>& history) {
    return history.empty() ? 0.0F : *std::ranges::max_element(history);
  };
  switch (m_pane) {
  case Pane::Cpu:
    caption = i18n::tr("activity.strip-cpu");
    chips.emplace_back([last] { return i18n::tr("activity.chip-user", "value", percentText(last().cpuUserPercent)); },
                       ColorRole::Accent, false);
    chips.emplace_back(
        [last] { return i18n::tr("activity.chip-system", "value", percentText(last().cpuSystemPercent)); },
        ColorRole::Label3, false
    );
    values = series([](const SystemStats& s) { return s.cpuUserPercent; }, 100.0F);
    values2 = series([](const SystemStats& s) { return s.cpuSystemPercent; }, 100.0F);
    cells = {
        {i18n::tr("activity.grid-idle"), [last] { return percentText(100.0 - last().cpuUsagePercent); }},
        {i18n::tr("activity.grid-load"), [last] { return std::format("{:.2f}", last().loadAvg1); }},
        {i18n::tr("activity.grid-running"), [this] { return std::to_string(m_monitor->activity().rows().size()); }},
        {i18n::tr("activity.grid-speed"),
         [last] {
           const SystemStats s = last();
           return s.cpuFreqAvailable ? std::format("{:.1f} GHz", s.cpuFreqMhz / 1000.0) : std::string("–");
         }},
        {i18n::tr("activity.grid-temperature"),
         [last] {
           const SystemStats s = last();
           return s.cpuTempC.has_value() ? std::format("{:.0f} °C", *s.cpuTempC) : std::string("–");
         }},
        {i18n::tr("activity.grid-uptime"), [] { return upTimeText(); }},
    };
    break;
  case Pane::Memory: {
    std::string key;
    const bool high = problem(&key) == Problem::Memory;
    caption = i18n::tr("activity.strip-memory");
    if (high) {
      chips.emplace_back([this] {
        const bool rising = m_pressureHistory.size() > 10
            && m_pressureHistory.back() > m_pressureHistory[m_pressureHistory.size() - 11];
        return i18n::tr(rising ? "activity.chip-rising" : "activity.chip-high");
      }, ColorRole::Warning, true);
      graphRole = ColorRole::Warning;
    } else {
      chips.emplace_back([this] {
        return i18n::tr("activity.chip-pressure", "value",
                        percentText(m_pressureHistory.empty() ? 0.0 : m_pressureHistory.back()));
      }, ColorRole::Accent, false);
    }
    values = [this] { return normalized(m_pressureHistory, 20.0F); };
    const auto mb = [](std::uint64_t value) { return memoryText(value * 1024 * 1024); };
    cells = {
        {i18n::tr("activity.grid-physical"), [last, mb] { return mb(last().ramTotalMb); }},
        {i18n::tr("activity.grid-used"), [last, mb] { return mb(last().ramUsedMb); }},
        {i18n::tr("activity.grid-app-memory"),
         [rowsSum] {
           return memoryText(static_cast<std::uint64_t>(rowsSum([](const ActivityRow& r) {
             return static_cast<double>(r.memoryBytes);
           })));
         }},
        {i18n::tr("activity.grid-swap"), [last, mb] { return mb(last().swapUsedMb); }},
        {i18n::tr("activity.grid-swap-size"), [last, mb] { return mb(last().swapTotalMb); }},
        {i18n::tr("activity.column-pressure"),
         [this] { return percentText(m_pressureHistory.empty() ? 0.0 : m_pressureHistory.back()); }},
    };
    break;
  }
  case Pane::Energy:
    caption = i18n::tr("activity.strip-energy");
    if (battery) {
      chips.emplace_back([this] {
        const UPowerService* live = m_upowerProvider ? m_upowerProvider() : nullptr;
        return i18n::tr("activity.chip-battery", "value",
                        percentText(live != nullptr ? live->state().percentage : 0.0));
      }, ColorRole::Success, false);
      values = [this] { return normalized(m_powerHistory, 10.0F); };
    } else {
      chips.emplace_back([last] { return i18n::tr("activity.chip-cpu", "value", percentText(last().cpuUsagePercent)); },
                         ColorRole::Accent, false);
      values = series([](const SystemStats& s) { return s.cpuUsagePercent; }, 100.0F);
    }
    cells = {
        {i18n::tr("activity.grid-battery"),
         [this] {
           const UPowerService* live = m_upowerProvider ? m_upowerProvider() : nullptr;
           return live != nullptr && live->state().isPresent ? percentText(live->state().percentage)
                                                             : i18n::tr("activity.battery-absent");
         }},
        {i18n::tr("activity.grid-power"),
         [this] {
           const UPowerService* live = m_upowerProvider ? m_upowerProvider() : nullptr;
           return live != nullptr && live->state().energyRate > 0.0 ? std::format("{:.1f} W", live->state().energyRate)
                                                                    : std::string("–");
         }},
        {i18n::tr("activity.grid-state"),
         [this] {
           const UPowerService* live = m_upowerProvider ? m_upowerProvider() : nullptr;
           if (live == nullptr || !live->state().isPresent) {
             return std::string("–");
           }
           return i18n::tr(live->state().onBattery ? "activity.state-on-battery" : "activity.state-plugged");
         }},
        {i18n::tr("activity.grid-high-energy"),
         [this] {
           int count = 0;
           for (const auto& row : m_monitor->activity().rows()) {
             count += energyBars(row.cpuPercent, gpuOf(row)) >= 3 ? 1 : 0;
           }
           return std::to_string(count);
         }},
        {i18n::tr("activity.column-gpu"),
         [last] {
           const SystemStats s = last();
           return s.gpuUsagePercent.has_value() ? percentText(*s.gpuUsagePercent) : std::string("–");
         }},
        {i18n::tr("activity.column-cpu"), [last] { return percentText(last().cpuUsagePercent); }},
    };
    break;
  case Pane::Disk:
    caption = i18n::tr("activity.strip-disk");
    chips.emplace_back([this] {
      return i18n::tr("activity.chip-total", "value", std::format("{:.1f} MB/s", m_diskHistory.empty() ? 0.0F
                                                                                                        : m_diskHistory.back()));
    }, ColorRole::Accent, false);
    values = [this] { return normalized(m_diskHistory, 1.0F); };
    cells = {
        {i18n::tr("activity.column-read"),
         [rowsSum] { return rateText(rowsSum([](const ActivityRow& r) { return r.ioReadBytesPerSec; })); }},
        {i18n::tr("activity.column-written"),
         [rowsSum] { return rateText(rowsSum([](const ActivityRow& r) { return r.ioWriteBytesPerSec; })); }},
        {i18n::tr("activity.grid-peak"), [this, peak] { return std::format("{:.1f} MB/s", peak(m_diskHistory)); }},
    };
    break;
  case Pane::Network:
  case Pane::Containers:
    caption = i18n::tr("activity.strip-network");
    chips.emplace_back([last] { return i18n::tr("activity.chip-in", "value", rateText(last().netRxBytesPerSec)); },
                       ColorRole::Accent, false);
    chips.emplace_back([last] { return i18n::tr("activity.chip-out", "value", rateText(last().netTxBytesPerSec)); },
                       ColorRole::Label3, false);
    values = series([](const SystemStats& s) { return s.netRxBytesPerSec + s.netTxBytesPerSec; }, 1024.0F * 1024.0F);
    cells = {
        {i18n::tr("activity.grid-received"), [last] { return rateText(last().netRxBytesPerSec); }},
        {i18n::tr("activity.grid-sent"), [last] { return rateText(last().netTxBytesPerSec); }},
        {i18n::tr("activity.grid-peak"),
         [stats] {
           double top = 0.0;
           for (const auto& s : stats()) {
             top = std::max(top, s.netRxBytesPerSec + s.netTxBytesPerSec);
           }
           return rateText(top);
         }},
    };
    break;
  }

  auto graphBox = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .width = graphWidth});
  auto captionRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  captionRow->addChild(text(caption, Style::fontSizeCaption * scale, ColorRole::Label2, FontWeight::SemiBold));
  for (auto& [chipText, role, warn] : chips) {
    Label* label = nullptr;
    captionRow->addChild(chip(&label, chipText(), role, warn, scale));
    m_liveText.push_back({label, std::move(chipText)});
  }
  graphBox->addChild(std::move(captionRow));
  auto* graph = static_cast<Graph*>(
      graphBox->addChild(makeGraph(values(), graphWidth, kStripGraphHeight * scale, graphRole))
  );
  if (values2) {
    graph->setValues2(values2());
    graph->setColor2(colorSpecFromRole(ColorRole::Label3));
  }
  m_liveGraphs.push_back({graph, std::move(values), std::move(values2)});
  graphBox->addChild(ui::row(
      {.align = FlexAlign::Center, .width = graphWidth},
      text(i18n::tr("activity.time-ago"), Tokens::typeCaption.size * scale, ColorRole::Label3, FontWeight::Medium),
      ui::spacer(),
      text(i18n::tr("activity.time-now"), Tokens::typeCaption.size * scale, ColorRole::Label3, FontWeight::Medium)
  ));
  strip->addChild(std::move(graphBox));

  auto grid = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd * scale, .width = gridWidth});
  grid->setPadding(Tokens::spaceP24 * scale, 0.0F, 0.0F, 0.0F);
  const float cellWidth = (gridWidth - 2.0F * Style::spaceLg * scale) / 3.0F;
  Flex* line = nullptr;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    if (i % 3 == 0) {
      line = static_cast<Flex*>(grid->addChild(ui::row({.align = FlexAlign::Start, .gap = Style::spaceLg * scale})));
    }
    Label* value = nullptr;
    line->addChild(ui::column(
        {.align = FlexAlign::Start, .width = cellWidth},
        text(cells[i].label, Style::fontSizeCaption * scale, ColorRole::Label2, FontWeight::Medium),
        ui::label({
            .out = &value,
            .text = cells[i].value(),
            .fontSize = Tokens::typeTitle3.size * scale,
            .fontWeight = FontWeight::SemiBold,
            .fontFeatures = std::string("tnum"),
            .color = colorSpecFromRole(ColorRole::Label1),
            .maxLines = 1,
        })
    ));
    m_liveText.push_back({value, std::move(cells[i].value)});
  }
  strip->addChild(std::move(grid));
  return strip;
}
