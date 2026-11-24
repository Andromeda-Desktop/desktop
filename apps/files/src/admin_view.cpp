#include "admin_view.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>
#include <gio/gio.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files-admin");

  std::string owned(gchar* text) {
    std::string out = text != nullptr ? text : "";
    g_free(text);
    return out;
  }

  std::shared_ptr<const FileInfo> rowOf(const fs::path& dir, const nlohmann::json& e) {
    auto info = std::make_shared<FileInfo>();
    const std::string name = e.value("name", std::string());
    const std::string kind = e.value("kind", std::string());
    info->name = (dir / name).string();
    info->setDisplayName(name);
    info->collateKey = owned(g_utf8_collate_key_for_filename(name.c_str(), -1));
    info->isDir = kind == "dir";
    info->kind = kind == "dir" ? FileKind::Directory : kind == "link" ? FileKind::Symlink : kind == "file" ? FileKind::File : FileKind::Other;
    info->hidden = name.starts_with('.');
    info->size = info->isDir ? 0 : e.value("size", std::uint64_t{0});
    info->mtimeNs = e.value("mtime_ns", std::int64_t{0});
    info->mode = e.value("mode", std::uint32_t{0});
    if (info->isDir) {
      info->contentType = "inode/directory";
    } else {
      gboolean uncertain = FALSE;
      gchar* type = g_content_type_guess(name.c_str(), nullptr, 0, &uncertain);
      gchar* mime = type != nullptr ? g_content_type_get_mime_type(type) : nullptr;
      g_free(type);
      info->contentType = mime != nullptr ? owned(mime) : "application/octet-stream";
    }
    return info;
  }

  nlohmann::json pathList(const std::vector<fs::path>& paths) {
    nlohmann::json out = nlohmann::json::array();
    for (const fs::path& p : paths) {
      out.push_back(p.string());
    }
    return out;
  }
} // namespace

AdminView::AdminView(Host host) : m_host(std::move(host)) {}

void AdminView::start() {
  // A failed session (password dismissed, helper gone) starts again on the next open: that is Try Again.
  if (m_process != nullptr && m_process->session().phase() != AdminSession::Phase::Failed) {
    return;
  }
  m_process = std::make_unique<AdminProcess>([this, alive = std::weak_ptr<bool>(m_alive)] {
    // The session changes phase inside its own calls: act after they return.
    DeferredCall::callLater([this, alive] {
      if (!alive.expired()) {
        releaseIfIdle();
        show(false);
      }
    });
  });
}

AdminSession::Phase AdminView::phase() const {
  return m_process != nullptr ? m_process->session().phase() : AdminSession::Phase::Closed;
}

void AdminView::release() {
  m_unused = true;
  releaseIfIdle();
}

void AdminView::releaseIfIdle() {
  // A request in flight keeps the helper: its result still shows as a toast.
  if (!m_unused || m_process == nullptr || phase() == AdminSession::Phase::Busy) {
    return;
  }
  m_process.reset();
  kLog.info("admin helper closed: no tab shows an administrator folder");
}

void AdminView::open(const fs::path& dir, bool navigate) {
  m_dir = dir;
  m_unused = false;
  start();
  show(navigate);
  m_process->session().request({{"op", "list"}, {"path", dir.string()}},
                               [this, alive = std::weak_ptr<bool>(m_alive), dir](const nlohmann::json& reply) {
                                 if (alive.expired() || dir != m_dir) {
                                   return;
                                 }
                                 m_rows.clear();
                                 m_error = reply.value("ok", false) ? std::string() : reply.value("error", i18n::tr("files.admin.folder-unreadable"));
                                 for (const auto& e : reply.value("entries", nlohmann::json::array())) {
                                   m_rows.push_back(rowOf(dir, e));
                                 }
                                 kLog.debug("admin list {} {} items", dir.string(), m_rows.size());
                                 show(false);
                               });
}

void AdminView::change(nlohmann::json op, std::string done, std::function<void(const std::string& error)> then) {
  start();
  m_process->session().request(std::move(op), [this, alive = std::weak_ptr<bool>(m_alive), done = std::move(done),
                                               then = std::move(then)](const nlohmann::json& reply) {
    if (alive.expired()) {
      return;
    }
    const bool ok = reply.value("ok", false);
    const std::string error = ok ? std::string() : reply.value("error", i18n::tr("files.admin.change-failed"));
    kLog.debug("admin {} {}", done, ok ? "ok" : error);
    m_host.toast(ok ? done : error);
    if (then) {
      then(error);
    }
    refresh();
  });
}

void AdminView::newFolder(std::function<void(const fs::path&)> made) {
  std::string name = i18n::tr("files.admin.new-folder");
  for (int n = 2; std::ranges::any_of(m_rows, [&](const auto& r) { return r->displayName() == name; }); ++n) {
    name = i18n::tr("files.admin.new-folder-numbered", "n", n);
  }
  const fs::path path = m_dir / name;
  change({{"op", "mkdir"}, {"path", path.string()}}, i18n::tr("files.admin.made", "name", name),
         [made = std::move(made), path](const std::string& error) {
           if (error.empty()) {
             made(path);
           }
         });
}

void AdminView::rename(const fs::path& path, const std::string& name, std::function<void(const std::string&)> done) {
  change({{"op", "rename"}, {"path", path.string()}, {"name", name}}, i18n::tr("files.admin.renamed", "name", name),
         std::move(done));
}

void AdminView::remove(const std::vector<fs::path>& paths) {
  change({{"op", "delete"}, {"paths", pathList(paths)}},
         i18n::tr("files.admin.deleted", "items",
                  paths.size() == 1 ? paths.front().filename().string() : i18n::trp("files.job.items", static_cast<long>(paths.size()))));
}

void AdminView::transfer(const std::vector<fs::path>& sources, const fs::path& dest, bool move) {
  change({{"op", move ? "move" : "copy"}, {"sources", pathList(sources)}, {"dest", dest.string()}},
         i18n::tr(move ? "files.admin.moved" : "files.admin.copied", "items",
                  sources.size() == 1 ? sources.front().filename().string()
                                      : i18n::trp("files.job.items", static_cast<long>(sources.size()))));
}

void AdminView::chmod(const std::vector<fs::path>& paths, std::uint32_t mode, std::uint32_t mask, bool recursive,
                      std::function<void(const std::string& error)> done) {
  change({{"op", "chmod"}, {"paths", pathList(paths)}, {"mode", mode}, {"mask", mask}, {"recursive", recursive}},
         i18n::tr("files.admin.permissions-changed"), std::move(done));
}

void AdminView::chown(const std::vector<fs::path>& paths, std::optional<std::uint32_t> uid,
                      std::optional<std::uint32_t> gid, bool recursive, std::function<void(const std::string& error)> done) {
  nlohmann::json op{{"op", "chown"}, {"paths", pathList(paths)}, {"recursive", recursive}};
  if (uid.has_value()) {
    op["uid"] = *uid;
  }
  if (gid.has_value()) {
    op["gid"] = *gid;
  }
  change(std::move(op), i18n::tr("files.admin.owner-changed"), std::move(done));
}

void AdminView::show(bool navigate) {
  const AdminSession::Phase now = phase();
  const bool listed = now == AdminSession::Phase::Ready || now == AdminSession::Phase::Busy;
  FileBrowser::Listing listing{.id = std::string(kPrefix) + m_dir.string(),
                               .title = m_dir == "/" ? std::string("/") : m_dir.filename().string(),
                               .rows = listed && m_error.empty() ? m_rows : std::vector<std::shared_ptr<const FileInfo>>{},
                               .activatable = true,
                               .renamable = true,
                               .dropFolder = m_dir};
  if (!listed || !m_error.empty()) {
    listing.page = [this](float s) { return statusPage(s); };
  }
  listing.open = [this](const FileInfo& row) {
    if (!row.isDir) {
      return false; // a file opens in its app as the user
    }
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive), next = fs::path(row.name)] {
      if (!alive.expired()) {
        open(next);
      }
    });
    return true;
  };
  listing.banner = [this](float s) { return banner(s); };
  m_host.show(std::move(listing), navigate);
}

std::unique_ptr<Node> AdminView::banner(float s) {
  auto bar = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s,
                      .fill = colorSpecFromRole(ColorRole::Danger, Tokens::componentFilesAdminWash),
                      .height = Tokens::sizeRowCompact * s + Tokens::spaceP8 * s});
  bar->addChild(ui::glyph({.glyph = "shield-lock", .glyphSize = Tokens::sizeSymbolInline * s,
                           .color = colorSpecFromRole(ColorRole::DangerText)}));
  bar->addChild(ui::label({.text = i18n::tr("files.admin.banner"),
                           .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                           .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 1,
                           .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
  bar->addChild(ui::button({.text = i18n::tr("files.admin.close"), .variant = ButtonVariant::Default,
                            .onClick = [this] {
                              DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive), dir = m_dir] {
                                if (!alive.expired()) {
                                  m_host.leave(dir);
                                }
                              });
                            }}));
  return bar;
}

std::unique_ptr<Node> AdminView::statusPage(float s) {
  const AdminSession::Phase now = phase(); // Closed: a listing from history, before its tab opens a helper again
  auto page = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP12 * s,
                          .flexGrow = 1.0F});
  const bool failed = now == AdminSession::Phase::Failed;
  const bool waiting = !failed && m_error.empty();
  if (waiting) {
    page->addChild(ui::spinner({.color = colorSpecFromRole(ColorRole::Label2), .spinnerSize = Tokens::sizeSymbol * s}));
  }
  const std::string text = failed            ? m_process->session().failure()
                           : !m_error.empty() ? m_error
                           : now == AdminSession::Phase::Starting || now == AdminSession::Phase::Closed
                               ? i18n::tr("files.admin.waiting-password")
                                                                               : i18n::tr("files.admin.reading-folder");
  page->addChild(ui::label({.text = text, .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label2)}));
  if (failed) {
    page->addChild(ui::button({.text = i18n::tr("files.admin.try-again"), .variant = ButtonVariant::Default,
                               .onClick = [this] {
                                 DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
                                   if (!alive.expired()) {
                                     open(m_dir);
                                   }
                                 });
                               }}));
  }
  return page;
}
