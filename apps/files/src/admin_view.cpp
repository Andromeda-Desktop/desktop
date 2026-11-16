#include "admin_view.h"

#include "core/deferred_call.h"
#include "core/log.h"
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
    info->displayName = name;
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
    // The session changes phase inside its own calls: show the page after they return.
    DeferredCall::callLater([this, alive] {
      if (!alive.expired()) {
        show();
      }
    });
  });
}

void AdminView::open(const fs::path& dir) {
  m_dir = dir;
  start();
  show();
  m_process->session().request({{"op", "list"}, {"path", dir.string()}},
                               [this, alive = std::weak_ptr<bool>(m_alive), dir](const nlohmann::json& reply) {
                                 if (alive.expired() || dir != m_dir) {
                                   return;
                                 }
                                 m_rows.clear();
                                 m_error = reply.value("ok", false) ? std::string() : reply.value("error", std::string("The folder could not be read."));
                                 for (const auto& e : reply.value("entries", nlohmann::json::array())) {
                                   m_rows.push_back(rowOf(dir, e));
                                 }
                                 kLog.debug("admin list {} {} items", dir.string(), m_rows.size());
                                 show();
                               });
}

void AdminView::change(nlohmann::json op, std::string done) {
  start();
  m_process->session().request(std::move(op), [this, alive = std::weak_ptr<bool>(m_alive), done = std::move(done)](
                                                  const nlohmann::json& reply) {
    if (alive.expired()) {
      return;
    }
    const bool ok = reply.value("ok", false);
    kLog.debug("admin {} {}", done, ok ? "ok" : reply.value("error", std::string()));
    m_host.toast(ok ? done : reply.value("error", std::string("The change failed.")));
    refresh();
  });
}

void AdminView::newFolder(const std::string& name) {
  change({{"op", "mkdir"}, {"path", (m_dir / name).string()}}, std::format("Made {} as the administrator", name));
}

void AdminView::rename(const fs::path& path, const std::string& name) {
  change({{"op", "rename"}, {"path", path.string()}, {"name", name}}, std::format("Renamed to {}", name));
}

void AdminView::remove(const std::vector<fs::path>& paths) {
  change({{"op", "delete"}, {"paths", pathList(paths)}},
         paths.size() == 1 ? std::format("Deleted {}", paths.front().filename().string()) : std::format("Deleted {} items", paths.size()));
}

void AdminView::transfer(const std::vector<fs::path>& sources, const fs::path& dest, bool move) {
  change({{"op", move ? "move" : "copy"}, {"sources", pathList(sources)}, {"dest", dest.string()}},
         std::format("{} {} as the administrator", move ? "Moved" : "Copied",
                     sources.size() == 1 ? sources.front().filename().string() : std::format("{} items", sources.size())));
}

void AdminView::chmod(const std::vector<fs::path>& paths, std::uint32_t mode, std::uint32_t mask, bool recursive) {
  change({{"op", "chmod"}, {"paths", pathList(paths)}, {"mode", mode}, {"mask", mask}, {"recursive", recursive}},
         "Permissions changed");
}

void AdminView::chown(const std::vector<fs::path>& paths, std::optional<std::uint32_t> uid,
                      std::optional<std::uint32_t> gid, bool recursive) {
  nlohmann::json op{{"op", "chown"}, {"paths", pathList(paths)}, {"recursive", recursive}};
  if (uid.has_value()) {
    op["uid"] = *uid;
  }
  if (gid.has_value()) {
    op["gid"] = *gid;
  }
  change(std::move(op), "Owner changed");
}

void AdminView::show() {
  const AdminSession::Phase phase = m_process->session().phase();
  const bool listed = phase == AdminSession::Phase::Ready || phase == AdminSession::Phase::Busy;
  FileBrowser::Listing listing{.id = std::string(kPrefix) + m_dir.string(),
                               .title = m_dir == "/" ? std::string("/") : m_dir.filename().string(),
                               .rows = listed && m_error.empty() ? m_rows : std::vector<std::shared_ptr<const FileInfo>>{},
                               .activatable = true,
                               .renamable = true};
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
  m_host.show(std::move(listing));
}

std::unique_ptr<Node> AdminView::banner(float s) {
  auto bar = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s,
                      .fill = colorSpecFromRole(ColorRole::Danger, 0.16F),
                      .height = Tokens::sizeRowCompact * s + Tokens::spaceP8 * s});
  bar->addChild(ui::glyph({.glyph = "shield-lock", .glyphSize = Tokens::sizeSymbolInline * s,
                           .color = colorSpecFromRole(ColorRole::DangerText)}));
  bar->addChild(ui::label({.text = std::string("Administrator: changes here run as root and cannot be undone."),
                           .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                           .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 1,
                           .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
  bar->addChild(ui::button({.text = std::string("Close"), .variant = ButtonVariant::Default,
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
  const AdminSession& session = m_process->session();
  auto page = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP12 * s,
                          .flexGrow = 1.0F});
  const bool failed = session.phase() == AdminSession::Phase::Failed;
  const bool waiting = !failed && m_error.empty();
  if (waiting) {
    page->addChild(ui::spinner({.color = colorSpecFromRole(ColorRole::Label2), .spinnerSize = Tokens::sizeSymbol * s}));
  }
  const std::string text = failed            ? session.failure()
                           : !m_error.empty() ? m_error
                           : session.phase() == AdminSession::Phase::Starting ? "Waiting for the administrator password…"
                                                                               : "Reading the folder…";
  page->addChild(ui::label({.text = text, .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label2)}));
  if (failed) {
    page->addChild(ui::button({.text = std::string("Try Again"), .variant = ButtonVariant::Default,
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
