#include "get_info_sheet.h"

#include "core/blocking_pool.h"
#include "core/deferred_call.h"
#include "core/files/file_info.h"
#include "core/log.h"
#include "core/text_clipboard.h"
#include "files/file_ops_client.h"
#include "permission_access.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/files/file_format.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <fcntl.h>
#include <format>
#include <gio/gio.h>
#include <grp.h>
#include <map>
#include <pwd.h>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files-info");
  std::unique_ptr<Flex> section(const char* caption, float s);
  const float& kFieldLabel = Tokens::componentFilesFieldLabel;
  constexpr std::uint64_t kCountPostEvery = 4096; // entries between live size updates

  std::int64_t ns(const statx_timestamp& t) { return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec; }

  std::string where(const fs::path& parent) {
    std::string text = parent.string();
    const char* home = g_get_home_dir();
    if (home != nullptr && text.starts_with(home)) {
      text = "Home" + text.substr(std::string_view(home).size());
    }
    std::string out;
    for (const fs::path& part : fs::path(text)) {
      if (part == "/") {
        continue;
      }
      out += (out.empty() ? "" : " › ") + part.string();
    }
    return out.empty() ? "/" : out;
  }

  std::string userName(std::uint32_t uid) {
    const passwd* pw = ::getpwuid(uid);
    std::string name = pw != nullptr ? pw->pw_name : std::to_string(uid);
    return uid == ::getuid() ? name + " (you)" : name;
  }

  std::string groupName(std::uint32_t gid) {
    const group* gr = ::getgrgid(gid);
    return gr != nullptr ? gr->gr_name : std::to_string(gid);
  }

  // People who can own files: root, the current owner, and login accounts (uid 1000 up to nobody).
  std::vector<std::pair<std::uint32_t, std::string>> ownerChoices(std::optional<std::uint32_t> now) {
    std::vector<std::pair<std::uint32_t, std::string>> out;
    ::setpwent();
    while (const passwd* pw = ::getpwent()) {
      if (pw->pw_uid == 0 || pw->pw_uid == now || (pw->pw_uid >= 1000 && pw->pw_uid < 65534)) {
        out.emplace_back(pw->pw_uid, userName(pw->pw_uid));
      }
    }
    ::endpwent();
    if (now.has_value() && std::ranges::none_of(out, [&](const auto& e) { return e.first == *now; })) {
      out.emplace_back(*now, userName(*now));
    }
    std::ranges::sort(out, {}, &std::pair<std::uint32_t, std::string>::second);
    return out;
  }

  // The groups the user is in, user groups (gid 1000 up to nogroup), and the current group.
  std::vector<std::pair<std::uint32_t, std::string>> groupChoices(std::optional<std::uint32_t> now) {
    std::set<std::uint32_t> mine;
    const int n = ::getgroups(0, nullptr);
    std::vector<gid_t> groups(static_cast<std::size_t>(std::max(n, 0)));
    if (n > 0 && ::getgroups(n, groups.data()) == n) {
      mine.insert(groups.begin(), groups.end());
    }
    mine.insert(::getgid());
    std::vector<std::pair<std::uint32_t, std::string>> out;
    ::setgrent();
    while (const group* gr = ::getgrent()) {
      if (mine.contains(gr->gr_gid) || gr->gr_gid == now || (gr->gr_gid >= 1000 && gr->gr_gid < 65534)) {
        out.emplace_back(gr->gr_gid, gr->gr_name);
      }
    }
    ::endgrent();
    if (now.has_value() && std::ranges::none_of(out, [&](const auto& e) { return e.first == *now; })) {
      out.emplace_back(*now, groupName(*now));
    }
    std::ranges::sort(out, {}, &std::pair<std::uint32_t, std::string>::second);
    return out;
  }

  std::string defaultApp(const std::string& contentType) {
    GAppInfo* app = g_app_info_get_default_for_type(contentType.c_str(), FALSE);
    if (app == nullptr) {
      return "None";
    }
    std::string name = g_app_info_get_display_name(app);
    g_object_unref(app);
    return name;
  }

  std::string normalizedHex(std::string text) {
    std::erase_if(text, [](unsigned char c) { return std::isspace(c) != 0; });
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  }
} // namespace

GetInfoSheet::GetInfoSheet(std::vector<fs::path> items, FileOpsClient* ops, std::function<void()> close,
                           std::function<void()> rebuild, std::function<void()> invalidate)
    : m_ops(ops), m_close(std::move(close)), m_rebuild(std::move(rebuild)), m_invalidate(std::move(invalidate)) {
  for (fs::path& path : items) {
    m_items.push_back({.path = std::move(path)});
  }
  load();
  startCount();
}

GetInfoSheet::~GetInfoSheet() {
  *m_alive = false;
  m_count->stop = true;
  if (m_ops != nullptr) {
    for (const Sum* sum : {&m_sha, &m_md5}) {
      if ((sum->state == Sum::State::Running || sum->state == Sum::State::Cancelling) && !sum->job.empty()) {
        m_ops->cancel(sum->job);
      }
    }
  }
}

void GetInfoSheet::later(std::function<void()> fn) {
  DeferredCall::callLater([alive = std::weak_ptr<bool>(m_alive), fn = std::move(fn)] {
    if (auto a = alive.lock(); a && *a) {
      fn();
    }
  });
}

void GetInfoSheet::load() {
  m_versions = m_items.size() == 1 ? previousVersions(m_items.front().path) : std::vector<Version>{};
  if (!m_versions.empty()) {
    kLog.debug("previous versions {}", m_versions.size());
  }
  for (Item& item : m_items) {
    struct statx st{};
    if (::statx(AT_FDCWD, item.path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME, &st) != 0) {
      continue;
    }
    item.mode = st.stx_mode & 07777;
    item.uid = st.stx_uid;
    item.gid = st.stx_gid;
    item.isDir = S_ISDIR(st.stx_mode);
    item.isRegular = S_ISREG(st.stx_mode);
    item.size = item.isDir ? 0 : st.stx_size;
    item.createdNs = (st.stx_mask & STATX_BTIME) != 0 ? ns(st.stx_btime) : 0;
    item.modifiedNs = ns(st.stx_mtime);
    item.openedNs = ns(st.stx_atime);
  }
}

void GetInfoSheet::startCount() {
  std::vector<fs::path> dirs;
  for (const Item& item : m_items) {
    if (item.isDir) {
      dirs.push_back(item.path);
    }
  }
  if (dirs.empty()) {
    m_count->done = true;
    return;
  }
  // Each file once (hard links share an inode) and this file system only (a mount inside is not part of the folder).
  BlockingPool::instance().run([this, count = m_count, dirs = std::move(dirs), alive = std::weak_ptr<bool>(m_alive)] {
    const auto post = [&] {
      if (!count->posted.exchange(true)) {
        DeferredCall::callLater([this, count, alive] {
          count->posted = false;
          if (auto a = alive.lock(); a && *a) {
            updateSize();
          }
        });
      }
    };
    std::set<std::pair<dev_t, ino_t>> linked;
    for (const fs::path& dir : dirs) {
      struct stat root{};
      if (::stat(dir.c_str(), &root) != 0) {
        continue;
      }
      std::error_code ec;
      for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
           !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (count->stop) {
          return;
        }
        struct stat st{};
        if (::lstat(it->path().c_str(), &st) != 0) {
          continue;
        }
        if (S_ISDIR(st.st_mode) && st.st_dev != root.st_dev) {
          it.disable_recursion_pending();
        } else if (S_ISREG(st.st_mode) && (st.st_nlink < 2 || linked.emplace(st.st_dev, st.st_ino).second)) {
          count->bytes += static_cast<std::uint64_t>(st.st_size);
        }
        if (++count->items % kCountPostEvery == 0) {
          post();
        }
      }
    }
    count->done = true;
    post();
  });
}

std::string GetInfoSheet::sizeText() const {
  std::uint64_t bytes = m_count->bytes;
  for (const Item& item : m_items) {
    bytes += item.size;
  }
  const bool dirs = std::ranges::any_of(m_items, [](const Item& i) { return i.isDir; });
  if (!dirs) {
    return formatFileSize(bytes);
  }
  const std::uint64_t items = m_count->items;
  std::string text = std::format("{} · {} {}", formatFileSize(bytes), items, items == 1 ? "item" : "items");
  return m_count->done ? text : "Counting… " + text;
}

void GetInfoSheet::updateSize() {
  if (m_size != nullptr) {
    m_size->setText(sizeText());
    m_invalidate();
  }
}

std::unique_ptr<Node> GetInfoSheet::build(float scale) {
  const float s = scale;
  auto sheet = ui::column({.align = FlexAlign::Stretch, .width = Tokens::componentDialogWidth * s});

  std::size_t files = 0;
  std::size_t folders = 0;
  for (const Item& item : m_items) {
    (item.isDir ? folders : files) += 1;
  }
  std::string title = m_items.size() == 1 ? m_items.front().path.filename().string() : std::format("{} Items", m_items.size());
  auto header = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::componentDialogPadding * s,
                         .height = Tokens::componentDialogHeader * s});
  header->addChild(ui::label({.text = std::move(title), .fontSize = Tokens::typeTitle3.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1),
                              .maxLines = 1, .ellipsize = TextEllipsize::Middle}));
  sheet->addChild(std::move(header));

  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP16 * s,
                          .paddingH = Tokens::componentDialogPadding * s});
  if (m_items.size() > 1) {
    body->addChild(ui::label({.text = std::format("{} {}, {} {}", files, files == 1 ? "file" : "files", folders,
                                                  folders == 1 ? "folder" : "folders"),
                              .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label2)}));
  }
  body->addChild(buildGeneral(s));
  body->addChild(buildPermissions(s));
  if (m_items.size() == 1 && m_items.front().isRegular) {
    body->addChild(buildChecksums(s));
    if (!m_versions.empty()) {
      body->addChild(buildVersions(s));
    }
  }
  if (!m_error.empty()) {
    body->addChild(ui::label({.text = m_error, .fontSize = Style::fontSizeCaption * s,
                              .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 3}));
  }
  if (m_admin.has_value()) {
    body->addChild(ui::button({.text = std::string("Change as Administrator…"), .variant = ButtonVariant::Default,
                               .onClick = [this] { DeferredCall::callLater([this] { changeAsAdmin(); }); }}));
  }
  sheet->addChild(std::move(body));

  auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s,
                         .padding = Tokens::componentDialogPadding * s});
  footer->addChild(ui::button({.text = std::string("Done"), .variant = ButtonVariant::Primary, .onClick = [this] {
                                 DeferredCall::callLater([close = m_close] { close(); });
                               }}));
  sheet->addChild(std::move(footer));
  return sheet;
}

std::vector<GetInfoSheet::Version> GetInfoSheet::previousVersions(const fs::path& file) {
  constexpr std::size_t kMaxVersions = 10;
  std::vector<Version> out;
  struct stat now{};
  if (::stat(file.c_str(), &now) != 0) {
    return out;
  }
  const auto nsOf = [](const struct stat& st) {
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
  };
  std::error_code ec;
  for (fs::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0 || st.st_dev != now.st_dev) {
      break; // snapper keeps .snapshots at the root of the subvolume: past the device boundary is another one
    }
    const fs::path snapshots = dir / ".snapshots";
    if (fs::is_directory(snapshots, ec)) {
      const fs::path rel = file.lexically_relative(dir);
      for (const auto& entry : fs::directory_iterator(snapshots, ec)) {
        const fs::path candidate = entry.path() / "snapshot" / rel;
        struct stat old{};
        if (::stat(candidate.c_str(), &old) == 0 && S_ISREG(old.st_mode) &&
            (nsOf(old) != nsOf(now) || old.st_size != now.st_size)) {
          out.push_back({.path = candidate, .modifiedNs = nsOf(old), .size = static_cast<std::uint64_t>(old.st_size)});
        }
      }
    }
    if (dir == dir.root_path()) {
      break;
    }
  }
  std::ranges::sort(out, std::greater<>{}, &Version::modifiedNs);
  // The same version kept in several snapshots shows once.
  const auto [first, last] = std::ranges::unique(out, [](const Version& a, const Version& b) {
    return a.modifiedNs == b.modifiedNs && a.size == b.size;
  });
  out.erase(first, last);
  if (out.size() > kMaxVersions) {
    out.resize(kMaxVersions);
  }
  return out;
}

std::unique_ptr<Node> GetInfoSheet::buildVersions(float s) {
  auto box = section("Previous Versions", s);
  for (const Version& v : m_versions) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    row->addChild(ui::label({.text = std::format("{} · {}", formatFileDate(v.modifiedNs), formatFileSize(v.size)),
                             .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label1),
                             .flexGrow = 1.0F}));
    row->addChild(ui::button({.text = std::string("Open"), .variant = ButtonVariant::Default,
                              .onClick = [this, path = v.path] {
                                if (m_open) {
                                  m_open(path);
                                }
                              }}));
    row->addChild(ui::button({.text = std::string("Restore"), .variant = ButtonVariant::Default,
                              .onClick = [this, path = v.path] {
                                if (m_restore && !m_items.empty()) {
                                  kLog.debug("restore version {}", path.string());
                                  m_restore(path, m_items.front().path.parent_path());
                                }
                              }}));
    box->addChild(std::move(row));
  }
  return box;
}

void GetInfoSheet::failed(const std::string& error, AdminChange change) {
  m_error = error;
  // EPERM: the items belong to another user; root can make the change after polkit asks.
  if (error.find("Operation not permitted") != std::string::npos || error.find("os error 1)") != std::string::npos) {
    m_admin = std::move(change);
    m_error = "These items belong to another user.";
  }
  load();
  later(m_rebuild);
}

void GetInfoSheet::changeAsAdmin() {
  if (!m_admin.has_value()) {
    return;
  }
  const char* override = std::getenv("ANDROMEDA_FILEOPS_ADMIN"); // tests point this at a stand-in
  const std::string helper = override != nullptr && *override != '\0' ? override : "/usr/lib/andromeda/andromeda-fileops-admin";
  const auto id = [](std::optional<std::uint32_t> v) { return v.has_value() ? std::to_string(*v) : std::string("-"); };
  std::vector<std::string> args{"pkexec", helper};
  if (const auto* mode = std::get_if<ModeChange>(&m_admin->what)) {
    args.insert(args.end(), {"chmod", std::format("{:o}", mode->mode), std::format("{:o}", mode->mask)});
  } else {
    const auto& owner = std::get<OwnerChange>(m_admin->what);
    args.insert(args.end(), {"chown", id(owner.uid), id(owner.gid)});
  }
  args.emplace_back(m_admin->recursive ? "1" : "0");
  for (const auto& p : m_admin->paths) {
    args.push_back(p.string());
  }
  std::vector<const char*> argv;
  for (const auto& a : args) {
    argv.push_back(a.c_str());
  }
  argv.push_back(nullptr);
  GError* error = nullptr;
  GSubprocess* proc = g_subprocess_newv(argv.data(), static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
  if (proc == nullptr) {
    m_error = std::format("Could not ask for the administrator password: {}", error->message);
    g_error_free(error);
    later(m_rebuild);
    return;
  }
  m_error = "Waiting for the administrator password…";
  later(m_rebuild);
  struct Ctx {
    GetInfoSheet* self;
    std::weak_ptr<bool> alive;
  };
  g_subprocess_communicate_utf8_async(
      proc, nullptr, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Ctx> ctx(static_cast<Ctx*>(data));
        auto* p = G_SUBPROCESS(source);
        gchar* out = nullptr;
        gchar* err = nullptr;
        const bool ran = g_subprocess_communicate_utf8_finish(p, result, &out, &err, nullptr) != FALSE;
        const bool ok = ran && g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0;
        if (!ctx->alive.expired()) {
          GetInfoSheet* self = ctx->self;
          // pkexec exits 126 when the user dismissed the password dialog.
          const bool dismissed = ran && g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 126;
          self->m_error = ok ? std::string() : dismissed ? std::string("No change: the password was not given.")
                                                         : std::string(err != nullptr && *err != '\0' ? err : "The change failed.");
          if (ok) {
            self->m_admin.reset();
          }
          self->load();
          self->later(self->m_rebuild);
        }
        g_free(out);
        g_free(err);
        g_object_unref(p);
      },
      new Ctx{.self = this, .alive = m_alive});
}

namespace {
  std::unique_ptr<Flex> section(const char* caption, float s) {
    auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
    box->addChild(ui::label({.text = std::string(caption), .fontSize = Style::fontSizeBody * s,
                             .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
    return box;
  }

  std::unique_ptr<Flex> field(const std::string& label, float s) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    row->addChild(ui::label({.text = label, .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label2),
                             .width = kFieldLabel * s, .maxLines = 1, .ellipsize = TextEllipsize::End}));
    return row;
  }

  std::unique_ptr<Label> value(std::string text, float s, Label** out = nullptr) {
    return ui::label({.out = out, .text = std::move(text), .fontSize = Style::fontSizeBody * s,
                      .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1, .ellipsize = TextEllipsize::Middle,
                      .flexGrow = 1.0F});
  }
} // namespace

std::unique_ptr<Node> GetInfoSheet::buildGeneral(float s) {
  auto box = section("General", s);
  const auto shared = [&](auto get, auto text) -> std::string {
    const auto first = get(m_items.front());
    const bool same = std::ranges::all_of(m_items, [&](const Item& i) { return get(i) == first; });
    return same ? text(first) : std::string("Mixed");
  };
  if (m_items.size() == 1) {
    const auto info = statFile(m_items.front().path);
    auto kind = field("Kind", s);
    kind->addChild(value(info ? fileKindLabel(*info) : std::string("Unknown"), s));
    box->addChild(std::move(kind));
    if (info && !info->isDir) {
      auto open = field("Open With", s);
      open->addChild(value(defaultApp(info->contentType), s));
      box->addChild(std::move(open));
    }
  }
  auto size = field("Size", s);
  size->addChild(value(sizeText(), s, &m_size));
  box->addChild(std::move(size));
  auto at = field("Where", s);
  at->addChild(value(shared([](const Item& i) { return i.path.parent_path(); }, where), s));
  box->addChild(std::move(at));
  const auto date = [&](const char* label, std::int64_t Item::*member) {
    auto row = field(label, s);
    row->addChild(value(shared([member](const Item& i) { return i.*member; },
                               [](std::int64_t t) { return t == 0 ? std::string("Unknown") : formatFileDate(t); }),
                        s));
    box->addChild(std::move(row));
  };
  date("Created", &Item::createdNs);
  date("Modified", &Item::modifiedNs);
  date("Opened", &Item::openedNs);
  return box;
}

std::unique_ptr<Node> GetInfoSheet::buildPermissions(float s) {
  auto box = section("Permissions", s);
  std::vector<std::uint32_t> modes;
  for (const Item& item : m_items) {
    modes.push_back(item.mode);
  }
  const Item& first = m_items.front();
  const bool oneOwner = std::ranges::all_of(m_items, [&](const Item& i) { return i.uid == first.uid; });
  const bool oneGroup = std::ranges::all_of(m_items, [&](const Item& i) { return i.gid == first.gid; });
  const auto picker = [&](const char* label, const std::vector<std::pair<std::uint32_t, std::string>>& ids,
                          std::optional<std::uint32_t> now, std::function<void(std::uint32_t)> pick) {
    std::vector<std::string> options;
    std::optional<std::size_t> selected;
    for (const auto& [n, name] : ids) {
      if (now == n) {
        selected = options.size();
      }
      options.push_back(name);
    }
    auto row = field(label, s);
    row->addChild(ui::select({.options = std::move(options), .selectedIndex = selected,
                              .placeholder = std::string("Mixed"), .fontSize = Style::fontSizeBody * s,
                              .controlHeight = Style::controlHeight * s,
                              .onSelectionChanged = [ids, pick = std::move(pick)](std::size_t i, std::string_view) {
                                if (i < ids.size()) {
                                  pick(ids[i].first);
                                }
                              }}));
    box->addChild(std::move(row));
  };
  picker("Owner", ownerChoices(oneOwner ? std::optional(first.uid) : std::nullopt),
         oneOwner ? std::optional(first.uid) : std::nullopt,
         [this](std::uint32_t uid) { setOwner({.uid = uid, .gid = std::nullopt}); });
  picker("Group", groupChoices(oneGroup ? std::optional(first.gid) : std::nullopt),
         oneGroup ? std::optional(first.gid) : std::nullopt,
         [this](std::uint32_t gid) { setOwner({.uid = std::nullopt, .gid = gid}); });
  const std::array<std::string, 3> names{oneOwner ? userName(first.uid) : std::string("Owner"),
                                         oneGroup ? groupName(first.gid) : std::string("Group"), std::string("Everyone")};
  for (int cls = 0; cls < 3; ++cls) {
    const auto common = commonAccess(modes, static_cast<PermClass>(cls));
    std::vector<ui::SegmentedOption> options{{.label = "No access"}, {.label = "Read only"}, {.label = "Read and write"}};
    std::size_t selected = common.has_value() ? static_cast<std::size_t>(*common) : 3;
    if (selected >= 3) {
      options.push_back({.label = common.has_value() ? accessLabel(*common) : "Mixed"});
      selected = 3;
    }
    auto row = field(names[static_cast<std::size_t>(cls)], s);
    row->addChild(ui::segmented({.options = std::move(options), .selectedIndex = selected, .scale = s, .compact = true,
                                 .onChange = [this, cls](std::size_t index) {
                                   if (index < 3) {
                                     setAccess(cls, static_cast<int>(index));
                                   }
                                 }}));
    box->addChild(std::move(row));
  }
  const auto check = [&](const char* label, bool checked, std::function<void(bool)> set) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    row->addChild(ui::checkbox({.checked = checked, .scale = s, .onChange = std::move(set)}));
    row->addChild(ui::label({.text = std::string(label), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::Label1)}));
    box->addChild(std::move(row));
  };
  if (std::ranges::none_of(m_items, [](const Item& i) { return i.isDir; })) {
    const bool runs = std::ranges::all_of(m_items, [](const Item& i) { return (i.mode & 0111) != 0; });
    check("Allow running as a program", runs, [this](bool on) { setRun(on); });
  } else {
    check("Apply to enclosed items", m_recursive, [this](bool on) { m_recursive = on; });
  }
  if (m_items.size() > 1) {
    box->addChild(ui::label({.text = std::format("A change here applies to all {} items.", m_items.size()),
                             .fontSize = Style::fontSizeCaption * s, .color = colorSpecFromRole(ColorRole::Label2)}));
  }
  return box;
}

void GetInfoSheet::apply(AdminChange change, const std::vector<Item>& before) {
  if (m_adminSink) {
    // The sheet shows the change at once; a helper error puts the old values back (the toast says why).
    m_adminSink(change, [this, alive = std::weak_ptr<bool>(m_alive), paths = change.paths, before](const std::string& error) {
      if (alive.expired() || error.empty()) {
        return;
      }
      for (Item& item : m_items) {
        const auto was = std::ranges::find(before, item.path, &Item::path);
        if (was != before.end() && std::ranges::contains(paths, item.path)) {
          item.mode = was->mode;
          item.uid = was->uid;
          item.gid = was->gid;
        }
      }
      later(m_rebuild);
    });
    return;
  }
  auto done = [this, change](const std::string&, const std::string& error) {
    if (!error.empty()) {
      failed(error, change);
    }
  };
  if (const auto* mode = std::get_if<ModeChange>(&change.what)) {
    m_ops->setPermissions(change.paths, mode->mode, mode->mask, change.recursive, std::move(done));
  } else {
    const auto& owner = std::get<OwnerChange>(change.what);
    m_ops->setOwner(change.paths, owner.uid, owner.gid, change.recursive, std::move(done));
  }
}

void GetInfoSheet::setOwner(OwnerChange change) {
  if (m_ops == nullptr && !m_adminSink) {
    return;
  }
  const std::vector<Item> before = m_items;
  std::vector<fs::path> paths;
  for (Item& item : m_items) {
    paths.push_back(item.path);
    item.uid = change.uid.value_or(item.uid);
    item.gid = change.gid.value_or(item.gid);
  }
  apply({.paths = std::move(paths), .what = change, .recursive = m_recursive}, before);
  later(m_rebuild);
}

void GetInfoSheet::setAccess(int cls, int access) {
  const auto [mode, mask] = accessBits(static_cast<PermClass>(cls), static_cast<Access>(access));
  if (m_ops == nullptr && !m_adminSink) {
    return;
  }
  const std::vector<Item> before = m_items;
  std::vector<fs::path> paths;
  for (Item& item : m_items) {
    paths.push_back(item.path);
    item.mode = (item.mode & ~mask) | (mode & mask);
  }
  apply({.paths = std::move(paths), .what = ModeChange{.mode = mode, .mask = mask}, .recursive = m_recursive}, before);
  later(m_rebuild);
}

void GetInfoSheet::setRun(bool allow) {
  if (m_ops == nullptr && !m_adminSink) {
    return;
  }
  // Each file gets x where it reads: one job per distinct mode keeps the rule exact.
  const std::vector<Item> before = m_items;
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<fs::path>> jobs;
  for (Item& item : m_items) {
    const auto [mode, mask] = runBits(item.mode, allow);
    item.mode = (item.mode & ~mask) | (mode & mask);
    jobs[{mode, mask}].push_back(item.path);
  }
  for (auto& [bits, paths] : jobs) {
    apply({.paths = std::move(paths), .what = ModeChange{.mode = bits.first, .mask = bits.second}}, before);
  }
  later(m_rebuild);
}

std::unique_ptr<Node> GetInfoSheet::buildChecksums(float s) {
  auto box = section("Checksum", s);
  const auto row = [&](const char* label, Sum& sum, const char* algorithm) {
    auto r = field(label, s);
    switch (sum.state) {
    case Sum::State::Idle:
      r->addChild(value(std::string(), s));
      r->addChild(ui::button({.text = std::string("Show"), .variant = ButtonVariant::Default,
                              .onClick = [this, &sum, algorithm] { runChecksum(sum, algorithm); }}));
      break;
    case Sum::State::Running:
      r->addChild(value(std::string("Computing…"), s));
      r->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Default, .onClick = [this, &sum] {
                                sum.state = Sum::State::Cancelling;
                                if (!sum.job.empty()) {
                                  m_ops->cancel(sum.job);
                                }
                                later(m_rebuild);
                              }}));
      break;
    case Sum::State::Cancelling:
      r->addChild(value(std::string("Cancelling…"), s));
      break;
    case Sum::State::Done:
      r->addChild(value(sum.hex, s));
      r->addChild(ui::button({.text = std::string("Copy"), .variant = ButtonVariant::Default, .onClick = [&sum] {
                                if (TextClipboard* clipboard = Input::textClipboard()) {
                                  clipboard->setClipboardText(sum.hex);
                                }
                              }}));
      break;
    case Sum::State::Failed:
      r->addChild(ui::label({.text = sum.error, .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 1, .flexGrow = 1.0F}));
      r->addChild(ui::button({.text = std::string("Retry"), .variant = ButtonVariant::Default,
                              .onClick = [this, &sum, algorithm] { runChecksum(sum, algorithm); }}));
      break;
    }
    box->addChild(std::move(r));
  };
  row("SHA-256", m_sha, "sha256");
  row("MD5", m_md5, "md5");
  box->addChild(ui::input({.value = m_compare, .placeholder = std::string("Paste a checksum to compare"),
                           .fontSize = Style::fontSizeBody * s, .controlHeight = Style::controlHeight * s,
                           .onChange = [this](const std::string& v) {
                             m_compare = v;
                             updateCompare();
                           }}));
  box->addChild(ui::label({.out = &m_compareResult, .fontSize = Style::fontSizeCaption * s,
                           .color = colorSpecFromRole(ColorRole::Label2)}));
  updateCompare();
  return box;
}

void GetInfoSheet::updateCompare() {
  if (m_compareResult == nullptr) {
    return;
  }
  const std::string want = normalizedHex(m_compare);
  std::string text;
  if (!want.empty()) {
    if (m_sha.state == Sum::State::Done && want == m_sha.hex) {
      text = "Matches the SHA-256 checksum.";
    } else if (m_md5.state == Sum::State::Done && want == m_md5.hex) {
      text = "Matches the MD5 checksum.";
    } else if (m_sha.state == Sum::State::Done || m_md5.state == Sum::State::Done) {
      text = "Does not match.";
    } else {
      text = want.size() == 32 ? "Show the MD5 checksum to compare." : "Show the SHA-256 checksum to compare.";
    }
  }
  m_compareResult->setText(text);
  m_invalidate();
}

void GetInfoSheet::runChecksum(Sum& sum, const char* algorithm) {
  if (m_ops == nullptr) {
    return;
  }
  sum = {.state = Sum::State::Running, .job = {}, .hex = {}, .error = {}};
  const std::weak_ptr<bool> alive = m_alive;
  m_ops->checksum(
      m_items.front().path, algorithm,
      [ops = m_ops, alive, &sum](const std::string& job, const std::string&) {
        if (job.empty()) {
          return;
        }
        if (alive.expired()) {
          ops->cancel(job); // the sheet closed before the job had a path
          return;
        }
        sum.job = job;
        if (sum.state == Sum::State::Cancelling) {
          ops->cancel(job);
        }
      },
      [this, alive, &sum](const std::string& hex, const std::string& error) {
        if (alive.expired()) {
          return;
        }
        sum.state = hex.empty() ? (error == "cancelled" ? Sum::State::Idle : Sum::State::Failed) : Sum::State::Done;
        sum.hex = hex;
        sum.error = error;
        later(m_rebuild);
      });
  later(m_rebuild);
}
