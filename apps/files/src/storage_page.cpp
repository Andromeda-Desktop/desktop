#include "storage_page.h"

#include "core/blocking_pool.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "files/file_ops_client.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/files/file_format.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <chrono>
#include <format>
#include <glib.h>
#include <nlohmann/json.hpp>
#include <sys/statvfs.h>
#include <thread>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files-storage");
  // Category -> bar colour; categories not listed use the last entry.
  struct Swatch {
    const char* name; // the helper's category name
    ColorRole role;
    float alpha;
    const char* label; // catalog key
  };
  constexpr std::array<Swatch, 8> kSwatches{{
      {"Apps and system", ColorRole::Label2, 1.0F, "files.storage.cat-apps"},
      {"Documents", ColorRole::Accent, 1.0F, "files.storage.cat-documents"},
      {"Pictures", ColorRole::Warning, 1.0F, "files.storage.cat-pictures"},
      {"Videos", ColorRole::Danger, 1.0F, "files.storage.cat-videos"},
      {"Music", ColorRole::Success, 1.0F, "files.storage.cat-music"},
      {"Archives", ColorRole::Accent, 0.5F, "files.storage.cat-archives"},
      {"Caches", ColorRole::Label3, 1.0F, "files.storage.cat-caches"},
      {"Other", ColorRole::Fill3, 1.0F, "files.storage.cat-other"},
  }};
  constexpr float kBarGrow = 1000.0F; // flex weight of a whole bar; a segment gets its share

  const Swatch& swatchFor(const std::string& name) {
    for (const Swatch& s : kSwatches) {
      if (name == s.name) {
        return s;
      }
    }
    return kSwatches.back();
  }

  ColorSpec swatch(const std::string& name) {
    const Swatch& s = swatchFor(name);
    return colorSpecFromRole(s.role, s.alpha);
  }

  std::string shortPath(const fs::path& path) {
    const std::string text = path.string();
    const char* home = g_get_home_dir();
    if (home != nullptr && text.starts_with(home)) {
      const std::string rest = text.substr(std::string_view(home).size());
      return rest.empty() ? i18n::tr("files.storage.home") : "~" + rest;
    }
    return text;
  }

  std::unique_ptr<Label> text(std::string value, float size, ColorRole role, float grow = 0.0F) {
    return ui::label({.text = std::move(value), .fontSize = size, .color = colorSpecFromRole(role), .maxLines = 1,
                      .ellipsize = TextEllipsize::Middle, .flexGrow = grow});
  }
} // namespace

std::optional<StoragePage::Summary> StoragePage::parse(const std::string& json) {
  const auto doc = nlohmann::json::parse(json, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    return std::nullopt;
  }
  Summary out;
  out.total = doc.value("total", std::uint64_t{0});
  out.trash = doc.value("trash", std::uint64_t{0});
  out.caches = doc.value("caches", std::uint64_t{0});
  out.oldDownloads = doc.value("old_downloads", std::uint64_t{0});
  out.scannedAt = doc.value("scanned_at", std::uint64_t{0});
  out.cached = doc.value("cached", false);
  for (const auto& c : doc.value("categories", nlohmann::json::array())) {
    out.categories.emplace_back(c.at(0).get<std::string>(), c.at(1).get<std::uint64_t>());
  }
  for (const auto& l : doc.value("largest", nlohmann::json::array())) {
    out.largest.emplace_back(l.at(0).get<std::string>(), l.at(1).get<std::uint64_t>());
  }
  for (const auto& d : doc.value("old_download_items", nlohmann::json::array())) {
    out.oldDownloadItems.emplace_back(d.at(0).get<std::string>());
  }
  return out;
}

StoragePage::StoragePage(FileOpsClient* ops, fs::path root, Actions actions)
    : m_ops(ops), m_root(std::move(root)), m_actions(std::move(actions)) {}

void StoragePage::later(std::function<void()> fn) {
  DeferredCall::callLater([weak = weak_from_this(), fn = std::move(fn)] {
    if (weak.lock()) {
      fn();
    }
  });
}

void StoragePage::scan(bool fresh) {
  if (m_ops == nullptr || m_scanning) {
    return;
  }
  m_scanning = true;
  m_error.clear();
  std::weak_ptr<StoragePage> weak = weak_from_this();
  BlockingPool::instance().run([weak, root = m_root] {
    struct statvfs fsInfo{};
    std::uint64_t used = 0;
    std::uint64_t total = 0;
    if (::statvfs(root.c_str(), &fsInfo) == 0) {
      const std::uint64_t capacity = static_cast<std::uint64_t>(fsInfo.f_blocks) * fsInfo.f_frsize;
      const std::uint64_t free = static_cast<std::uint64_t>(fsInfo.f_bavail) * fsInfo.f_frsize;
      used = capacity - free;
      total = capacity;
    }
    DeferredCall::callLater([weak, used, total] {
      if (auto self = weak.lock()) {
        self->m_usage = total == 0 ? std::string()
                                  : i18n::tr("files.storage.used", "used", formatFileSize(used), "total", formatFileSize(total));
        self->m_actions.rebuild();
      }
    });
  });
  const auto start = std::chrono::steady_clock::now();
  m_ops->storageScan(m_root, fresh, {}, [weak, start, fresh](const std::string& json, const std::string& error) {
    auto self = weak.lock();
    if (self == nullptr) {
      return;
    }
    self->m_scanning = false;
    self->m_summary = json.empty() ? std::nullopt : parse(json);
    const Summary empty;
    const Summary& sum = self->m_summary ? *self->m_summary : empty;
    kLog.debug("storage scan {} categories, trash {}, caches {}, old downloads {} in {} ms", sum.categories.size(),
               sum.trash, sum.caches, sum.oldDownloadItems.size(),
               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    kLog.debug("storage total {}", sum.total);
    self->m_error = self->m_summary ? std::string() : error.empty() ? i18n::tr("files.storage.no-result") : error;
    if (self->m_drill) {
      self->drill(*self->m_drill);
    }
    self->m_actions.rebuild();
    // The daemon answered with a scan kept from before (a restart): show it, then walk again for current numbers.
    constexpr std::uint64_t kStaleSeconds = 600;
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    if (!fresh && self->m_summary && (self->m_summary->cached || self->m_summary->scannedAt + kStaleSeconds < now)) {
      kLog.debug("storage scan from {} s ago: scanning again", now - self->m_summary->scannedAt);
      self->scan(true);
    }
  });
  m_actions.rebuild();
}

void StoragePage::drill(const fs::path& folder) {
  if (m_ops == nullptr) {
    return;
  }
  m_drill = folder;
  m_drillRows.clear();
  std::weak_ptr<StoragePage> weak = weak_from_this();
  m_ops->storageFolder(folder, [weak, folder](std::vector<FileOpsClient::SizedPath> items, const std::string& error) {
    auto self = weak.lock();
    if (self == nullptr || self->m_drill != folder) {
      return;
    }
    self->m_drillRows.clear();
    for (auto& item : items) {
      self->m_drillRows.push_back({.path = std::move(item.path), .bytes = item.bytes, .isDir = item.isDir});
    }
    self->m_error = error;
    self->m_actions.rebuild();
  });
  m_actions.rebuild();
}

std::unique_ptr<Node> StoragePage::build(float scale) {
  const float s = scale;
  auto page = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP24 * s, .padding = Tokens::spaceP24 * s});

  auto header = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
  header->addChild(ui::label({.text = i18n::tr("files.storage.title"), .fontSize = Tokens::typeTitle3.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
  header->addChild(text(m_usage, Style::fontSizeBody * s, ColorRole::Label2, 1.0F));
  header->addChild(ui::button({.text = m_scanning ? i18n::tr("files.storage.scanning") : i18n::tr("files.storage.scan-again"), .enabled = !m_scanning,
                               .variant = ButtonVariant::Default, .onClick = [this] { later([this] { scan(true); }); }}));
  page->addChild(std::move(header));

  if (!m_summary) {
    page->addChild(text(m_scanning ? i18n::tr("files.storage.scanning-drive") : m_error, Style::fontSizeBody * s,
                        m_error.empty() ? ColorRole::Label2 : ColorRole::DangerText));
    return page;
  }
  page->addChild(buildBar(s));
  page->addChild(buildFolders(s));
  page->addChild(buildRecommendations(s));
  if (!m_error.empty()) {
    page->addChild(text(m_error, Style::fontSizeCaption * s, ColorRole::DangerText));
  }
  return page;
}

std::unique_ptr<Node> StoragePage::buildBar(float s) const {
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
  auto bar = ui::row({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Fill1),
                      .radius = Style::scaledRadius(Tokens::radiusR4, s), .clipChildren = true,
                      .height = Tokens::spaceP12 * s});
  auto legend = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Tokens::spaceP16 * s});
  const std::uint64_t total = m_summary->total;
  for (const auto& [name, bytes] : m_summary->categories) {
    if (total > 0) {
      bar->addChild(ui::box({.fill = swatch(name), .width = 0.0F, .height = Tokens::spaceP12 * s,
                             .flexGrow = kBarGrow * static_cast<float>(static_cast<double>(bytes) / static_cast<double>(total))}));
    }
    auto item = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
    item->addChild(ui::box({.fill = swatch(name), .radius = Tokens::spaceP8 * s / 2.0F, .width = Tokens::spaceP8 * s,
                            .height = Tokens::spaceP8 * s}));
    item->addChild(text(std::format("{} {}", i18n::tr(swatchFor(name).label), formatFileSize(bytes)), Style::fontSizeCaption * s, ColorRole::Label2));
    legend->addChild(std::move(item));
  }
  box->addChild(std::move(bar));
  box->addChild(std::move(legend));
  return box;
}

std::unique_ptr<Node> StoragePage::buildFolders(float s) {
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP4 * s});
  auto title = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  if (m_drill) {
    title->addChild(ui::button({.glyph = std::string("chevron-left"), .variant = ButtonVariant::Ghost, .onClick = [this] {
                                  later([this] {
                                    const fs::path up = m_drill->parent_path();
                                    if (*m_drill == m_root || up.string().size() < m_root.string().size()) {
                                      m_drill.reset();
                                      m_drillRows.clear();
                                      m_actions.rebuild();
                                    } else {
                                      drill(up);
                                    }
                                  });
                                }}));
  }
  title->addChild(ui::label({.text = m_drill ? shortPath(*m_drill) : i18n::tr("files.storage.largest"),
                             .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::SemiBold,
                             .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                             .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
  if (m_drill && m_actions.open) {
    title->addChild(ui::button({.text = i18n::tr("files.storage.show-in-files"), .variant = ButtonVariant::Default,
                                .onClick = [this] { m_actions.open(*m_drill); }}));
  }
  box->addChild(std::move(title));

  std::vector<Row> rows;
  if (m_drill) {
    rows = m_drillRows;
  } else {
    for (const auto& [path, bytes] : m_summary->largest) {
      rows.push_back({.path = path, .bytes = bytes, .isDir = true});
    }
  }
  for (const Row& r : rows) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP8 * s,
                        .height = Tokens::sizeRowCompact * s});
    row->addChild(text(m_drill ? r.path.filename().string() : shortPath(r.path), Style::fontSizeBody * s, ColorRole::Label1, 1.0F));
    row->addChild(text(formatFileSize(r.bytes), Style::fontSizeBody * s, ColorRole::Label2));
    if (r.isDir) {
      row->addChild(ui::button({.glyph = std::string("chevron-right"), .variant = ButtonVariant::Ghost,
                                .onClick = [this, path = r.path] { later([this, path] { drill(path); }); }}));
    }
    box->addChild(std::move(row));
  }
  return box;
}

std::unique_ptr<Node> StoragePage::buildRecommendations(float s) {
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP4 * s});
  box->addChild(ui::label({.text = i18n::tr("files.storage.recommendations"), .fontSize = Style::fontSizeBody * s,
                           .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
  const Summary& sum = *m_summary;
  const auto recommend = [&](Confirm kind, std::string label, std::uint64_t bytes, std::string action, std::string question,
                             std::string confirmLabel, std::function<void()> act, std::function<void()> review) {
    if (bytes == 0) {
      return;
    }
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP8 * s,
                        .minHeight = Tokens::sizeRowCompact * s});
    if (m_confirm == kind) {
      row->addChild(text(std::move(question), Style::fontSizeBody * s, ColorRole::Label1, 1.0F));
      row->addChild(ui::button({.text = i18n::tr("common.actions.cancel"), .variant = ButtonVariant::Default, .onClick = [this] {
                                  later([this] {
                                    m_confirm = Confirm::None;
                                    m_actions.rebuild();
                                  });
                                }}));
      row->addChild(ui::button({.text = std::move(confirmLabel), .variant = ButtonVariant::Primary,
                                .onClick = [this, act = std::move(act)] {
                                  later([this, act] {
                                    m_confirm = Confirm::None;
                                    if (act) {
                                      act();
                                    }
                                    scan(true);
                                  });
                                }}));
    } else {
      row->addChild(text(label, Style::fontSizeBody * s, ColorRole::Label1, 1.0F));
      row->addChild(text(formatFileSize(bytes), Style::fontSizeBody * s, ColorRole::Label2));
      if (review) {
        row->addChild(ui::button({.text = i18n::tr("files.storage.review"), .variant = ButtonVariant::Default,
                                  .onClick = std::move(review)}));
      }
      row->addChild(ui::button({.text = std::move(action), .variant = ButtonVariant::Default, .onClick = [this, kind] {
                                  later([this, kind] {
                                    m_confirm = kind;
                                    m_actions.rebuild();
                                  });
                                }}));
    }
    box->addChild(std::move(row));
  };
  recommend(Confirm::EmptyTrash, i18n::tr("files.storage.empty-trash"), sum.trash, i18n::tr("files.storage.empty-action"),
            i18n::tr("files.storage.empty-confirm"), i18n::tr("files.storage.empty-trash"), m_actions.emptyTrash, {});
  const fs::path downloads = fs::path(g_get_home_dir()) / "Downloads";
  recommend(Confirm::OldDownloads, i18n::tr("files.storage.old-downloads"), sum.oldDownloads,
            i18n::tr("files.storage.trash-action"),
            i18n::trp("files.storage.trash-confirm", static_cast<long>(sum.oldDownloadItems.size())),
            i18n::tr("files.storage.trash-button"),
            [this, items = sum.oldDownloadItems] {
              if (m_actions.trash) {
                m_actions.trash(items);
              }
            },
            m_actions.open ? std::function<void()>([this, downloads] { m_actions.open(downloads); }) : nullptr);
  const fs::path caches = fs::path(g_get_home_dir()) / ".cache";
  recommend(Confirm::Caches, i18n::tr("files.storage.caches"), sum.caches, i18n::tr("files.storage.clear-action"),
            i18n::tr("files.storage.caches-confirm"), i18n::tr("files.storage.caches-button"),
            [this, caches] {
              std::vector<fs::path> items;
              std::error_code ec;
              for (const auto& entry : fs::directory_iterator(caches, ec)) {
                items.push_back(entry.path());
              }
              if (m_actions.remove && !items.empty()) {
                m_actions.remove(std::move(items));
              }
            },
            {});
  if (box->children().size() == 1) {
    box->addChild(text(i18n::tr("files.storage.nothing"), Style::fontSizeBody * s, ColorRole::Label2));
  }
  return box;
}
