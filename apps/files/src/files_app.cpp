#include "files_app.h"

#include "app/app_notifier.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "files_window.h"
#include "i18n/i18n.h"
#include "job_text.h"
#include "protocols/uri.h"
#include "tag_manager_sheet.h"
#include "terminal_pane.h"
#include "select_pattern_sheet.h"
#include "smart_folder_sheet.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <glib.h>
#include <string_view>
#include <sys/stat.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("files");

  // A chooser browses and picks; it never changes files.
  // The compact Save sheet (mockup §6 dialog 456 wide): header, three field rows, buttons.
  constexpr std::uint32_t kSaveWidth = 456;
  constexpr std::uint32_t kSaveHeight = 272;
  // Print (mockup system-dialogs §3): preview left, 420 wide choices column right.
  constexpr std::uint32_t kPrintWidth = 960;
  constexpr std::uint32_t kPrintHeight = 560;
  constexpr std::uint32_t kPrintMinWidth = 760;
  constexpr std::uint32_t kPrintMinHeight = 480;
  // Open With (mockup §4) and the Access dialog (FILES-UI-SPEC §11).
  constexpr std::uint32_t kChooserWidth = 416;
  constexpr std::uint32_t kChooserHeight = 560;
  constexpr std::uint32_t kAccessWidth = 400;
  constexpr std::uint32_t kAccessHeight = 280;
  constexpr std::uint32_t kDialogMinHeight = 240;
  constexpr std::uint32_t kPortalHeight = 360;
  constexpr std::uint32_t kWallpaperWidth = 400;
  constexpr std::uint32_t kWallpaperHeight = 420;

  bool allowedInPicker(std::string_view id) {
    return (id.starts_with("go.") && id != "go.trash") || id.starts_with("view.") || id.starts_with("picker.")
        || id == "file.open" || id == "edit.select-all";
  }

  fs::path pathFromUri(const std::string& uri) {
    if (uri.find("://") == std::string::npos) {
      std::error_code ec;
      const fs::path abs = fs::absolute(uri, ec);
      return ec ? fs::path(uri) : abs.lexically_normal();
    }
    return uri::pathFromFileUri(uri).value_or("");
  }
} // namespace

FilesApp::FilesApp(bool picker, bool service)
    : DeskApp({.id = picker ? kPickerAppId : kAppId, .name = "Files"}), m_picker(picker),
      m_service(service) {
  i18n::Service::instance().addDomain("files", ANDROMEDA_TRANSLATIONS_DIR);
}

void FilesApp::showFromFileManager(FileManager1::Show show, const std::vector<std::string>& uris,
                                   const std::string& startupId) {
  kLog.debug("FileManager1 show {} {} items", static_cast<int>(show), uris.size());
  if (show == FileManager1::Show::Folders) {
    open(uris, startupId);
    return;
  }
  // ShowItems and ShowItemProperties reveal each item in its folder, folders too.
  for (const std::string& uri : uris) {
    const fs::path path = pathFromUri(uri);
    if (path.empty() || !path.has_parent_path()) {
      continue;
    }
    FilesWindow& w = openWindow(path.parent_path());
    w.selectNames({path.filename().string()});
    if (show == FileManager1::Show::Properties) {
      w.getInfo({path});
    }
    if (windows().back()->isOpen()) {
      windows().back()->activate(startupId);
    }
  }
}

void FilesApp::searchAnswered(const std::string& text, std::vector<std::shared_ptr<const FileInfo>> rows) {
  for (auto& [window, files] : m_windows) {
    files->searchAnswered(text, rows);
  }
}

FilesApp::~FilesApp() {
  if (m_mounts != nullptr) {
    g_signal_handlers_disconnect_by_data(m_mounts, this);
    g_object_unref(m_mounts);
  }
}

void FilesApp::startup() {
  loop().addSource(&m_watcher);
  if (bus() != nullptr) {
    m_fileOps = std::make_unique<FileOpsClient>(*bus());
    m_fileOps->setJobChangedCallback([this](const FileOpsClient::Job& job) { onJobChanged(job); });
    m_notifier = std::make_unique<AppNotifier>(*this);
  } else {
    kLog.warn("no session bus: file operations are off");
  }
  loop().addSource(&m_thumbnails);
  loop().addSource(&m_index);
  m_tags = std::make_shared<TagIndex>([this] {
    for (auto& [window, files] : m_windows) {
      files->tagsChanged();
    }
  });
  // The kernel mount table, watched locally: GVolumeMonitor would start gvfs monitors over D-Bus at startup.
  m_mounts = g_unix_mount_monitor_get();
  const auto onMounts = +[](GUnixMountMonitor*, gpointer self) { static_cast<FilesApp*>(self)->refreshPlaces(); };
  g_signal_connect(m_mounts, "mounts-changed", G_CALLBACK(onMounts), this);
  addCommands();
  if (m_picker) {
    if (bus() == nullptr) {
      kLog.error("the picker needs the session bus");
      quit(1);
      return;
    }
    m_portal = std::make_unique<PickerPortal>(
        *bus(), [this](std::uint64_t id, const PickerPortal::Request& request) { openPicker(id, request); },
        [this](std::uint64_t id) { closePicker(id); }
    );
    m_chooserPortal = std::make_unique<AppChooserPortal>(*bus(), AppChooserPortal::Handlers{
        .choose = [this](std::uint64_t id, const AppChooserPortal::ChooseRequest& r) { openChooser(id, r); },
        .updateChoices =
            [this](std::uint64_t id, const std::vector<std::string>& choices) {
              for (auto& [window, dialog] : m_dialogs) {
                if (dialog.id == id && dialog.chooser != nullptr) {
                  dialog.chooser->updateChoices(choices);
                }
              }
            },
        .access = [this](std::uint64_t id, const AppChooserPortal::AccessRequest& r) { openAccess(id, r); },
        .closed =
            [this](std::uint64_t id) {
              for (auto& [window, dialog] : m_dialogs) {
                if (dialog.id == id) {
                  dialog.answered = true;
                  closeWindow(*window);
                  return;
                }
              }
            },
    });
    m_systemPortals = std::make_unique<SystemPortals>(*bus(), SystemPortals::Handlers{
        .ask = [this](std::uint64_t id, const SystemPortals::Request& r) { openPortalSheet(id, r); },
        .closed =
            [this](std::uint64_t id) {
              for (auto& [window, dialog] : m_dialogs) {
                if (dialog.id == id && dialog.portal != nullptr) {
                  dialog.answered = true;
                  closeWindow(*window);
                  return;
                }
              }
            },
    });
    m_emailPortal = std::make_unique<EmailPortal>(*bus());
    m_printPortal = std::make_unique<PrintPortal>(
        *bus(), *m_notifier,
        PrintPortal::Handlers{
            .open = [this](std::uint64_t id, const PrintPortal::PrintRequest& r) { openPrint(id, r); },
            .closed =
                [this](std::uint64_t id) {
                  for (auto& [window, dialog] : m_dialogs) {
                    if (dialog.id == id && dialog.print != nullptr) {
                      dialog.answered = true;
                      closeWindow(*window);
                      return;
                    }
                  }
                },
        },
        std::filesystem::path(g_get_user_state_dir()) / "andromeda" / "print.ini");
    scheduleIdleQuit();
    return;
  }
  if (bus() != nullptr) {
    m_linkShare = std::make_unique<LinkShare>(*bus());
    m_linkShare->setChanged([this] { refreshPlaces(); });
    m_fileManager = std::make_unique<FileManager1>(
        *bus(), [this](FileManager1::Show show, const std::vector<std::string>& uris, const std::string& startupId) {
          showFromFileManager(show, uris, startupId);
        });
  }
  if (m_service) {
    m_idleQuit.start(std::chrono::minutes(2), [this] { quitWhenIdle(); });
  }
}

void FilesApp::scheduleIdleQuit() {
  // D-Bus activation starts the picker again for the next request.
  m_idleQuit.start(std::chrono::minutes(2), [this] {
    if (windows().empty() && m_portal->idle() && m_chooserPortal->idle() && m_systemPortals->idle() && m_emailPortal->idle()
        && m_printPortal->idle()
        && m_pendingTags.empty()) {
      quitWhenIdle();
    } else {
      scheduleIdleQuit();
    }
  });
}

void FilesApp::openPicker(std::uint64_t id, const PickerPortal::Request& request) {
  using Kind = PickerPortal::Request::Kind;
  const std::string title = !request.title.empty() ? request.title
      : request.kind == Kind::Open                    ? (request.directory ? i18n::tr("files.picker.select-folder") : i18n::tr("files.picker.open-file"))
                                                      : i18n::tr("files.picker.save-file");
  // Save opens as the compact sheet (mockup §6); its chevron grows the window to the full browser.
  const bool save = request.kind == Kind::Save;
  AppWindow& window = addWindow({
      .appId = info().id,
      .title = title,
      .stateKey = save ? "picker-save" : "picker",
      .width = save ? kSaveWidth : FilesWindow::kPickerWidth,
      .height = save ? kSaveHeight : FilesWindow::kPickerHeight,
      .minWidth = save ? kSaveWidth : 480,
      .minHeight = save ? kSaveHeight : 320,
      .navigation = true,
      .sidebarWidth = 200.0F,
      .parentWindow = request.parentWindow,
  });
  std::error_code ec;
  const fs::path folder = !request.currentFolder.empty() && fs::is_directory(request.currentFolder, ec)
      ? request.currentFolder
      : fs::path(g_get_home_dir());
  if (save) {
    window.setSidebarVisible(false);
  }
  auto& files = m_windows[&window] = std::make_unique<FilesWindow>(*this, window, folder);
  files->setPicker(id, request);
  if (!window.open()) {
    pickerAnswered(id, {.response = 2, .uris = {}, .filter = {}});
    closeWindow(window);
    return;
  }
  window.activate({});
}

void FilesApp::openPrint(std::uint64_t id, const PrintPortal::PrintRequest& request) {
  AppWindow& window = addWindow({.appId = info().id, .title = i18n::tr("files.window.print"), .stateKey = "print", .width = kPrintWidth,
                                 .height = kPrintHeight, .minWidth = kPrintMinWidth, .minHeight = kPrintMinHeight,
                                 .parentWindow = request.parentWindow});
  Dialog& dialog = m_dialogs[&window];
  dialog.id = id;
  const auto answered = [this, &window] {
    const auto it = m_dialogs.find(&window);
    if (it == m_dialogs.end() || it->second.answered)
      return false;
    it->second.answered = true;
    return true;
  };
  dialog.print = std::make_unique<PrintSheet>(
      loop(), request,
      PrintSheet::Callbacks{
          .print =
              [this, &window, answered, id](const PrintOptions& options, const std::string& printerLabel,
                                             int pages) {
                if (!answered())
                  return;
                m_printPortal->print(id, options, printerLabel, pages);
                closeWindow(window);
              },
          .savePdf =
              [this, &window, id, title = request.title] {
                std::string name = title.empty() ? i18n::tr("files.picker.document") : title;
                if (!name.ends_with(".pdf"))
                  name = fs::path(name).stem().string() + ".pdf";
                pickSaveFile(i18n::tr("files.picker.save-pdf"), name, [this, &window, id](std::vector<fs::path> paths) {
                  const auto it = m_dialogs.find(&window);
                  if (paths.empty() || it == m_dialogs.end() || it->second.id != id || it->second.answered)
                    return;
                  it->second.answered = true;
                  m_printPortal->savePdf(id, paths.front());
                  closeWindow(window);
                });
              },
          .cancel =
              [this, &window, answered, id] {
                if (!answered())
                  return;
                m_printPortal->cancel(id);
                closeWindow(window);
              },
          .rebuild = [&window] { window.requestRebuild(); },
      });
  window.setContentBuilder([sheet = dialog.print.get(), &window](Renderer& renderer, float width, float height) {
    return sheet->build(renderer, window.uiScale(), width, height);
  });
  window.setFirstKeyHandler([sheet = dialog.print.get(), &window](const KeyboardEvent& e) {
    return sheet->firstKey(e, window.input().focusedArea());
  });
  window.setKeyHandler([sheet = dialog.print.get()](const KeyboardEvent& e) { return sheet->key(e); });
  if (!window.open()) {
    m_printPortal->cancel(id);
    m_dialogs.erase(&window);
    closeWindow(window);
    return;
  }
  window.activate({});
}

void FilesApp::openChooser(std::uint64_t id, const AppChooserPortal::ChooseRequest& request) {
  AppWindow& window = addWindow({.appId = info().id, .title = i18n::tr("files.window.open-with"), .stateKey = "open-with", .width = kChooserWidth,
                                 .height = kChooserHeight, .minWidth = kChooserWidth, .minHeight = kDialogMinHeight,
                                 .parentWindow = request.parentWindow});
  Dialog& dialog = m_dialogs[&window];
  dialog.id = id;
  dialog.chooser = std::make_unique<AppChooserSheet>(
      request,
      [this, &window](std::uint32_t response, const std::string& choice) {
        if (const auto it = m_dialogs.find(&window); it != m_dialogs.end() && !it->second.answered) {
          it->second.answered = true;
          m_chooserPortal->answerChoose(it->second.id, response, choice);
          closeWindow(window);
        }
      },
      [&window] { window.requestRebuild(); });
  window.setContentBuilder([chooser = dialog.chooser.get(), &window](Renderer&, float, float) {
    return chooser->build(window.uiScale());
  });
  if (!window.open()) {
    m_chooserPortal->answerChoose(id, 2, {});
    m_dialogs.erase(&window);
    closeWindow(window);
    return;
  }
  window.activate({});
}

void FilesApp::openPortalSheet(std::uint64_t id, const SystemPortals::Request& request) {
  const bool wide = request.kind == SystemPortals::Kind::Wallpaper;
  AppWindow& window = addWindow({.appId = info().id, .title = PortalSheet::titleFor(request), .stateKey = "portal",
                                 .width = wide ? kWallpaperWidth : kAccessWidth,
                                 .height = wide ? kWallpaperHeight : kPortalHeight,
                                 .minWidth = wide ? kWallpaperWidth : kAccessWidth, .minHeight = kDialogMinHeight,
                                 .parentWindow = request.parentWindow});
  Dialog& dialog = m_dialogs[&window];
  dialog.id = id;
  dialog.portal = std::make_unique<PortalSheet>(request, [this, &window](std::uint32_t response, const std::string& name) {
    if (const auto it = m_dialogs.find(&window); it != m_dialogs.end() && !it->second.answered) {
      it->second.answered = true;
      m_systemPortals->answer(it->second.id, response, name);
      closeWindow(window);
    }
  });
  window.setContentBuilder([portal = dialog.portal.get(), &window](Renderer&, float, float) {
    return portal->build(window.uiScale());
  });
  if (!window.open()) {
    m_systemPortals->answer(id, 2);
    m_dialogs.erase(&window);
    closeWindow(window);
    return;
  }
  window.activate({});
}

void FilesApp::openAccess(std::uint64_t id, const AppChooserPortal::AccessRequest& request) {
  AppWindow& window = addWindow({.appId = info().id, .title = request.title, .stateKey = "access", .width = kAccessWidth,
                                 .height = kAccessHeight, .minWidth = kAccessWidth, .minHeight = kDialogMinHeight,
                                 .parentWindow = request.parentWindow});
  Dialog& dialog = m_dialogs[&window];
  dialog.id = id;
  dialog.access = std::make_unique<AccessSheet>(
      request, [this, &window](std::uint32_t response, const std::vector<AppChooserPortal::AccessChoice>& choices) {
        if (const auto it = m_dialogs.find(&window); it != m_dialogs.end() && !it->second.answered) {
          it->second.answered = true;
          m_chooserPortal->answerAccess(it->second.id, response, choices);
          closeWindow(window);
        }
      });
  window.setContentBuilder([access = dialog.access.get(), &window](Renderer&, float, float) {
    return access->build(window.uiScale());
  });
  if (!window.open()) {
    m_chooserPortal->answerAccess(id, 2, {});
    m_dialogs.erase(&window);
    closeWindow(window);
    return;
  }
  window.activate({});
}

void FilesApp::closePicker(std::uint64_t id) {
  for (auto& [appWindow, window] : m_windows) {
    if (window->pickerId() == id) {
      closeWindow(*appWindow);
    }
  }
}

void FilesApp::pickFiles(std::string title, std::string acceptLabel,
                         std::function<void(std::vector<fs::path>)> done, bool directory) {
  const std::uint64_t id = m_nextLocalPick++;
  m_localPicks.emplace(id, std::move(done));
  PickerPortal::Request request;
  request.title = std::move(title);
  request.acceptLabel = std::move(acceptLabel);
  request.multiple = !directory;
  request.directory = directory;
  openPicker(id, request);
}

void FilesApp::pickSaveFile(std::string title, std::string currentName,
                            std::function<void(std::vector<fs::path>)> done) {
  const std::uint64_t id = m_nextLocalPick++;
  m_localPicks.emplace(id, std::move(done));
  PickerPortal::Request request;
  request.kind = PickerPortal::Request::Kind::Save;
  request.title = std::move(title);
  request.acceptLabel = i18n::tr("files.picker.save");
  request.currentName = std::move(currentName);
  openPicker(id, request);
}

void FilesApp::pickerAnswered(std::uint64_t id, const PickerPortal::Answer& answer) {
  if (auto it = m_localPicks.find(id); it != m_localPicks.end()) {
    auto done = std::move(it->second);
    m_localPicks.erase(it);
    std::vector<fs::path> files;
    for (const std::string& uri : answer.response == 0 ? answer.uris : std::vector<std::string>{}) {
      if (auto path = uri::pathFromFileUri(uri)) {
        files.emplace_back(*path);
      }
    }
    done(std::move(files));
    return;
  }
  if (m_portal != nullptr) {
    m_portal->answer(id, answer);
  }
}

namespace {
  struct FileStamp {
    bool exists = false;
    std::uint64_t inode = 0;
    std::int64_t mtimeNs = 0;
  };
  FileStamp stampOf(const fs::path& file) {
    struct stat st {};
    if (::stat(file.c_str(), &st) != 0) {
      return {};
    }
    return {.exists = true, .inode = static_cast<std::uint64_t>(st.st_ino),
            .mtimeNs = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec};
  }
} // namespace

void FilesApp::tagWhenWritten(const fs::path& file, std::vector<std::string> tags) {
  const FileStamp before = stampOf(file);
  m_pendingTags.push_back({.file = file, .tags = std::move(tags), .deadline = std::chrono::steady_clock::now() + kTagWaitTime,
                           .existed = before.exists, .inode = before.inode, .mtimeNs = before.mtimeNs});
  if (m_tagPoll.active()) {
    return;
  }
  m_tagPoll.startRepeating(std::chrono::milliseconds(500), [this] {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_pendingTags, [&](const PendingTags& p) {
      const FileStamp stamp = stampOf(p.file);
      const bool written = stamp.exists && (!p.existed || stamp.inode != p.inode || stamp.mtimeNs != p.mtimeNs);
      if (written && m_fileOps != nullptr) {
        m_fileOps->setTags({p.file}, p.tags, "add");
        return true;
      }
      return now > p.deadline;
    });
    if (m_pendingTags.empty()) {
      m_tagPoll.stop();
    }
  });
}

void FilesApp::refreshPlaces() {
  for (auto& [window, files] : m_windows) {
    files->refreshPlaces();
  }
}

void FilesApp::unmounted(const std::filesystem::path& mount, const std::string& error) {
  if (!error.empty()) {
    if (FilesWindow* active = activeFilesWindow()) {
      active->unmounted(mount, error);
    }
    return;
  }
  for (auto& [window, files] : m_windows) {
    files->unmounted(mount, error);
  }
}

AppWindow& FilesApp::createWindow() {
  // Photos shares this process but is its own app: its own dock icon and window, nothing saved into Files.
  const bool photos = std::exchange(m_nextPhotos, false);
  AppWindow& window = addWindow({
      .appId = photos ? std::string("dev.andromeda.Photos") : info().id,
      .title = photos ? i18n::tr("files.window.photos") : info().name,
      .stateKey = photos ? std::string("photos") : std::string("main"),
      .navigation = true,
      .sidebarWidth = 240.0F,
      .moreMenu = true,
  });
  const bool plainLaunch = m_nextLocation.empty() && !m_picker;
  std::string last = plainLaunch ? state().get<std::string>("/files/last_place", "") : "";
  std::error_code ec;
  const bool lastFolder = last.starts_with('/') && fs::is_directory(last, ec);
  fs::path location = !m_nextLocation.empty() ? m_nextLocation : lastFolder ? fs::path(last) : fs::path(g_get_home_dir());
  m_nextLocation.clear();
  // Read before the window exists: its first navigation saves its single tab over the saved ones.
  const nlohmann::json savedTabs = plainLaunch ? state().get<nlohmann::json>("/files/tabs", nlohmann::json{}) : nlohmann::json{};
  auto& files = m_windows[&window] = std::make_unique<FilesWindow>(*this, window, std::move(location), photos);
  if (!lastFolder && !last.empty()) {
    files->reopenPlace(last);
  }
  if (plainLaunch) {
    files->restoreTabs(savedTabs);
  }
  return window;
}

void FilesApp::gpuLost() {
  m_thumbnails.abandonGpuResources();
  for (auto& [window, dialog] : m_dialogs) {
    if (dialog.print) {
      dialog.print->abandonGpuResources();
    }
  }
}

void FilesApp::windowClosed(AppWindow& window) {
  if (const auto it = m_dialogs.find(&window); it != m_dialogs.end()) {
    // Closed from the frame: the request ends as cancelled.
    if (!it->second.answered && it->second.print != nullptr) {
      m_printPortal->cancel(it->second.id);
    } else if (!it->second.answered && it->second.portal != nullptr) {
      m_systemPortals->answer(it->second.id, 1);
    } else if (!it->second.answered && m_chooserPortal != nullptr) {
      if (it->second.chooser != nullptr) {
        m_chooserPortal->answerChoose(it->second.id, 1, {});
      } else {
        m_chooserPortal->answerAccess(it->second.id, 1, {});
      }
    }
    m_dialogs.erase(it);
    return;
  }
  if (const auto it = m_windows.find(&window); it != m_windows.end()) {
    if (const auto id = it->second->pickerId()) {
      // Closed from the window frame or Ctrl+W: the request ends as cancelled.
      pickerAnswered(*id, {.response = 1, .uris = {}, .filter = {}});
    }
    for (auto& [other, files] : m_windows) {
      if (other != &window && other->isOpen() && !files->isPicker()) {
        files->adoptJobs(it->second->takeJobs());
        break;
      }
    }
    m_windows.erase(it);
  }
}

FilesWindow& FilesApp::openWindow(const fs::path& location) {
  m_nextLocation = location;
  m_hadWindow = true;
  // GVolumeMonitor starts the gvfs monitors over D-Bus: after the first frame, never in the picker.
  if (!m_picker && m_drives == nullptr) {
    m_drivesStart.start(std::chrono::milliseconds(300), [this] {
      if (m_drives == nullptr) {
        m_drives = std::make_unique<Drives>([this] { refreshPlaces(); });
        refreshPlaces();
      }
    });
  }
  AppWindow& window = createWindow();
  window.open();
  return *m_windows.at(&window);
}

FilesWindow* FilesApp::activeFilesWindow() const {
  AppWindow* window = activeWindow();
  if (window == nullptr) {
    return nullptr;
  }
  const auto it = m_windows.find(window);
  return it != m_windows.end() ? it->second.get() : nullptr;
}

void FilesApp::activate(const std::string& activationToken) {
  if (m_service && !m_hadWindow) { // the activation start itself; the request that started us opens the window
    return;
  }
  if (!m_picker) {
    DeskApp::activate(activationToken);
  }
}

void FilesApp::open(const std::vector<std::string>& uris, const std::string& activationToken) {
  if (m_picker) {
    return;
  }
  for (const std::string& uri : uris) {
    fs::path path = pathFromUri(uri);
    if (path.empty()) {
      kLog.warn("cannot open {}", uri);
      continue;
    }
    std::error_code ec;
    const bool folder = fs::is_directory(path, ec);
    const fs::path dir = folder ? path : path.parent_path();
    // A window that already shows the folder comes to the front instead of a second one.
    const auto shown = std::ranges::find_if(m_windows, [&dir](const auto& entry) { return entry.second->location() == dir; });
    AppWindow* window = shown != m_windows.end() ? shown->first : nullptr;
    FilesWindow& w = window != nullptr ? *shown->second : openWindow(dir);
    if (window == nullptr) {
      window = windows().back().get();
    }
    if (!folder) {
      // A file opens its folder; selecting it there is the reveal behaviour.
      w.selectNames({path.filename().string()});
    }
    if (window->isOpen()) {
      window->activate(activationToken);
    }
  }
  if (windows().empty()) {
    activate(activationToken);
  }
}

void FilesApp::action(const std::string& name, const std::vector<std::string>& parameter, const std::string& activationToken) {
  // These answer sheets and dialogs or type into a terminal for E2E scripts; any session client can call
  // ActivateAction, so they need the test environment.
  static const bool testHooks = std::getenv("DESK_FILES_TEST") != nullptr;
  static constexpr std::array<std::string_view, 7> kTestOnly{"sheet.password", "drive.erase", "terminal.type",
                                                             "terminal.dump", "chooser.", "portal.", "access."};
  if (!testHooks && std::ranges::any_of(kTestOnly, [&](std::string_view p) {
        return p.ends_with('.') ? name.starts_with(p) : name == p;
      })) {
    kLog.warn("action {} is for tests only (DESK_FILES_TEST)", name);
    return;
  }
  if (name == "new-window") { // the desktop entry's New Window and Super+E: a window at home beside any open ones
    openWindow(fs::path(g_get_home_dir()));
    if (!windows().empty() && windows().back()->isOpen()) {
      windows().back()->activate(activationToken);
    }
    return;
  }
  if (name == "gallery") { // Photos (dev.andromeda.Photos.desktop): a photo in the Gallery, else Pictures as a Gallery
    fs::path photo = parameter.empty() ? fs::path() : pathFromUri(parameter.front());
    std::error_code ec;
    m_nextPhotos = true;
    if (photo.empty()) {
      // Photos opened on its own: the whole photo library, not the few files at the top of Pictures.
      const char* pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
      FilesWindow& w = openWindow(pictures != nullptr ? fs::path(pictures) : fs::path(g_get_home_dir()) / "Pictures");
      w.showPhotos();
    } else if (fs::is_directory(photo, ec)) {
      FilesWindow& w = openWindow(photo);
      w.setView(FileBrowser::View::Gallery);
    } else {
      FilesWindow& w = openWindow(photo.parent_path());
      w.showInGallery(photo.filename().string());
    }
    if (!windows().empty() && windows().back()->isOpen()) {
      windows().back()->activate(activationToken);
    }
    return;
  }
  FilesWindow* window = activeFilesWindow();
  if (name == "go" && window != nullptr && !parameter.empty()) {
    window->go(pathFromUri(parameter.front()));
  } else if (name == "trash") { // the dock's Trash: shown in a plain Files window, brought to the front
    FilesWindow* target = window != nullptr && !window->isPicker() && !window->isPhotos() ? window : nullptr;
    if (target == nullptr) {
      target = &openWindow(fs::path(g_get_home_dir()));
    }
    target->goTrash();
    const auto it = std::ranges::find_if(m_windows, [&](const auto& entry) { return entry.second.get() == target; });
    if (it != m_windows.end() && it->first->isOpen()) {
      it->first->activate(activationToken);
    }
  } else if (name == "storage" && window != nullptr && !window->isPicker()) {
    window->showStorage();
  } else if (name == "vault.new" && window != nullptr && !window->isPicker() && !parameter.empty()) {
    window->askNewVault(parameter.front());
  } else if (name == "sheet.password" && window != nullptr && !parameter.empty()) {
    window->submitSheetPassword(parameter.front());
  } else if (name == "drive.erase" && window != nullptr) {
    window->eraseDrive();
  } else if (name == "phone.open" && window != nullptr && !window->isPicker() && !parameter.empty()) {
    window->showPhone(parameter.front());
  } else if (name == "phone.import-view" && window != nullptr) {
    window->showPhoneImport();
  } else if (name == "phone.pick" && window != nullptr && !parameter.empty()) {
    std::size_t index = 0;
    if (std::from_chars(parameter.front().data(), parameter.front().data() + parameter.front().size(), index).ec ==
        std::errc{}) {
      window->phonePick(index);
    }
  } else if (name == "phone.import" && window != nullptr) {
    window->phoneImport(!parameter.empty() && parameter.front() == "all");
  } else if (name == "drive.mount" && window != nullptr && !window->isPicker() && !parameter.empty()) {
    window->mountDrive(parameter.front());
  } else if (name == "vault.unlock" && window != nullptr && !window->isPicker() && !parameter.empty()) {
    window->askUnlockVault(parameter.front());
  } else if (name == "vault.lock" && window != nullptr && !parameter.empty()) {
    window->lockVault(parameter.front());
  } else if (name.starts_with("pattern.") && window != nullptr && window->patternSheet() != nullptr) {
    SelectPatternSheet* sheet = window->patternSheet();
    if (name == "pattern.text" && !parameter.empty()) {
      sheet->setPattern(parameter.front(), true);
    } else if (name == "pattern.mode" && !parameter.empty()) {
      sheet->setMode(parameter.front() == "regex" ? SelectPatternSheet::Mode::Regex : SelectPatternSheet::Mode::Wildcard);
    } else if (name == "pattern.select" || name == "pattern.deselect") {
      sheet->apply(name == "pattern.select");
    }
  } else if (name == "compress.set" && window != nullptr && window->compressSheet() != nullptr && parameter.size() >= 2) {
    window->compressSheet()->set(parameter[0], parameter[1]);
  } else if (name == "compress.submit" && window != nullptr && window->compressSheet() != nullptr) {
    window->compressSheet()->submit();
  } else if (name == "extract.password" && window != nullptr && window->extractSheet() != nullptr && !parameter.empty()) {
    window->extractSheet()->setPassword(parameter.front());
  } else if (name == "extract.submit" && window != nullptr && window->extractSheet() != nullptr) {
    window->extractSheet()->submit();
  } else if (name == "rename.set" && window != nullptr && window->batchRenameSheet() != nullptr && parameter.size() >= 2) {
    window->batchRenameSheet()->set(parameter[0], parameter[1]);
  } else if (name == "rename.submit" && window != nullptr && window->batchRenameSheet() != nullptr) {
    window->batchRenameSheet()->submit();
  } else if (name == "smart.set" && window != nullptr && window->smartSheet() != nullptr && parameter.size() >= 2) {
    window->smartSheet()->set(parameter[0], parameter[1]);
  } else if (name == "smart.save" && window != nullptr && window->smartSheet() != nullptr) {
    window->smartSheet()->submit();
  } else if (name == "smart.go" && window != nullptr && !parameter.empty()) {
    window->goSmart(parameter.front());
  } else if (name == "pane.activate" && window != nullptr && !parameter.empty()) {
    window->activatePane(parameter.front() == "1" ? 1 : 0);
  } else if (name == "terminal.type" && window != nullptr && window->terminal() != nullptr && !parameter.empty()) {
    window->terminal()->sendText(parameter.front());
  } else if (name == "terminal.dump" && window != nullptr && window->terminal() != nullptr) {
    kLog.info("terminal screen:\n{}", window->terminal()->screenText());
  } else if (name.starts_with("tags.") && window != nullptr) {
    tagAction(*window, name, parameter);
  } else if (name == "select" && window != nullptr) {
    window->selectNames(parameter);
  } else if (name == "picker.name" && window != nullptr && !parameter.empty()) {
    window->setPickerName(parameter.front());
  } else if (name.starts_with("chooser.") || name.starts_with("access.") || name.starts_with("portal.")) {
    dialogAction(name, parameter);
    return;
  } else {
    DeskApp::action(name, parameter, activationToken);
    return;
  }
  if (AppWindow* w = activeWindow()) {
    w->activate(activationToken);
  }
}

void FilesApp::tagAction(FilesWindow& window, const std::string& name, const std::vector<std::string>& parameter) {
  // E2E: tags.<edit> with [tag, value] on the open Tags sheet.
  TagManagerSheet* sheet = window.tagManager();
  if (sheet == nullptr) {
    return;
  }
  const std::string tag = parameter.empty() ? std::string() : parameter[0];
  const std::string value = parameter.size() > 1 ? parameter[1] : std::string();
  if (name == "tags.select") {
    sheet->select(tag);
  } else if (name == "tags.rename") {
    sheet->rename(tag, value);
  } else if (name == "tags.move") {
    sheet->move(tag, value == "up" ? -1 : 1);
  } else if (name == "tags.sidebar") {
    sheet->setSidebar(tag, value == "on");
  } else if (name == "tags.color" && value.size() == 7 && value[0] == '#') {
    sheet->setColor(tag, static_cast<std::uint32_t>(std::stoul(value.substr(1), nullptr, 16)));
  } else if (name == "tags.delete") {
    sheet->remove(tag);
  } else if (name == "tags.new") {
    sheet->addTag();
  } else if (name == "tags.done") {
    sheet->done();
  }
}

void FilesApp::dialogAction(const std::string& name, const std::vector<std::string>& parameter) {
  // The active dialog, else the newest: a headless test has no keyboard focus.
  Dialog* dialog = nullptr;
  if (const auto it = m_dialogs.find(activeWindow()); it != m_dialogs.end()) {
    dialog = &it->second;
  } else if (!m_dialogs.empty()) {
    dialog = &std::ranges::max_element(m_dialogs, {}, [](const auto& d) { return d.second.id; })->second;
  }
  FilesWindow* files = activeFilesWindow();
  AppChooserSheet* sheet = files != nullptr ? files->openWithSheet() : nullptr;
  if (dialog == nullptr && sheet == nullptr) {
    return;
  }
  if (AppChooserSheet* chooser = dialog != nullptr ? dialog->chooser.get() : sheet) {
    if (name == "chooser.select" && !parameter.empty()) {
      chooser->select(parameter.front());
    } else if (name == "chooser.accept") {
      chooser->accept();
    } else if (name == "chooser.cancel") {
      chooser->cancel();
    }
  } else if (PortalSheet* portal = dialog != nullptr ? dialog->portal.get() : nullptr) {
    if (name == "portal.accept") {
      portal->accept();
    } else if (name == "portal.cancel") {
      portal->cancel();
    } else if (name == "portal.name" && !parameter.empty()) {
      portal->setName(parameter.front());
    }
  } else if (AccessSheet* access = dialog != nullptr ? dialog->access.get() : nullptr) {
    if (name == "access.grant" || name == "access.deny") {
      access->answer(name == "access.grant");
    }
  }
}

void FilesApp::onJobChanged(const FileOpsClient::Job& job) {
  // Every rename and move (inline, batch, drag, cut and paste, undo) moves the starred and pinned paths with it.
  // Once per job: the finished state can arrive again with later property changes.
  if (job.finished() && !job.moves.empty() && m_movesApplied.insert(job.path).second) {
    starred().moved(job.moves);
    pins().moved(job.moves);
  }
  if (!job.finished()) {
    if (!m_jobLeases.contains(job.path)) {
      m_jobLeases.emplace(job.path, lease());
    }
  } else if (const auto held = m_jobLeases.find(job.path); held != m_jobLeases.end()) {
    // With no window open, the toast cannot show: the result goes to a desktop notification.
    if (windows().empty() && job.state != "cancelled" && m_notifier != nullptr) {
      m_notifier->notify({.body = jobResultText(job)}, {}, [this](const std::string& token, int /*button*/) { activate(token); });
    }
    m_jobLeases.erase(held);
  }
  for (auto& [appWindow, window] : m_windows) {
    window->onJobChanged(job);
  }
}

void FilesApp::addCommands() {
  commands().setGroupTitle("Conflict", i18n::tr("files.menu.conflict"));
  commands().setGroupTitle("Picker", i18n::tr("files.menu.picker"));
  const auto chord = [](std::uint32_t sym, std::uint32_t mods) { return KeyChord{.sym = sym, .modifiers = mods}; };
  struct Spec {
    const char* id;
    std::string label;
    const char* group;
    std::optional<KeyChord> shortcut;
    bool (*enabled)(const FilesWindow&);
    void (*run)(FilesWindow&);
    std::optional<KeyChord> alternate{};
  };
  const auto always = [](const FilesWindow&) { return true; };
  const auto selection = [](const FilesWindow& w) { return w.hasSelection(); };
  const auto conflict = [](const FilesWindow& w) { return w.hasConflict(); };
  const Spec specs[] = {
      {"go.back", i18n::tr("files.commands.go-back"), "Go", chord(XKB_KEY_Left, KeyMod::Alt), [](const FilesWindow& w) { return w.canBack(); },
       [](FilesWindow& w) { w.back(); }},
      {"go.forward", i18n::tr("files.commands.go-forward"), "Go", chord(XKB_KEY_Right, KeyMod::Alt),
       [](const FilesWindow& w) { return w.canForward(); }, [](FilesWindow& w) { w.forward(); }},
      {"go.up", i18n::tr("files.commands.go-up"), "Go", chord(XKB_KEY_Up, KeyMod::Alt), [](const FilesWindow& w) { return !w.inTrash(); },
       [](FilesWindow& w) { w.up(); }},
      {"go.home", i18n::tr("files.commands.go-home"), "Go", chord(XKB_KEY_Home, KeyMod::Alt), always,
       [](FilesWindow& w) { w.go(g_get_home_dir()); }},
      {"go.trash", i18n::tr("files.commands.go-trash"), "Go", std::nullopt, always, [](FilesWindow& w) { w.goTrash(); }},
      {"file.open", i18n::tr("files.commands.file-open"), "File", chord(XKB_KEY_Return, 0), selection, [](FilesWindow& w) { w.openSelection(); }},
      {"file.open-with", i18n::tr("files.commands.file-open-with"), "File", std::nullopt, [](const FilesWindow& w) { return w.canOpenWith(); },
       [](FilesWindow& w) { w.openWith(); }},
      {"file.import", i18n::tr("files.commands.file-import"), "File", std::nullopt,
       [](const FilesWindow& w) { return !w.isPicker() && !w.importSource().empty(); },
       [](FilesWindow& w) { w.importPhotos(); }},
      {"drive.format", i18n::tr("files.commands.drive-format"), "File", std::nullopt,
       [](const FilesWindow& w) { return !w.isPicker() && !w.formatTarget().empty(); },
       [](FilesWindow& w) { w.askFormat(); }},
      {"file.new-vault", i18n::tr("files.commands.file-new-vault"), "File", std::nullopt,
       [](const FilesWindow& w) { return !w.isPicker() && Vaults::available(); },
       [](FilesWindow& w) { w.askNewVault(); }},
      {"file.pin", i18n::tr("files.commands.file-pin"), "File", std::nullopt,
       [](const FilesWindow& w) { return !w.isPicker() && !w.inTrash() && w.selectionIsFolders(); },
       [](FilesWindow& w) { w.togglePin(); }},
      {"file.star", i18n::tr("files.commands.file-star"), "File", chord(XKB_KEY_d, KeyMod::Ctrl),
       [](const FilesWindow& w) { return !w.isPicker() && !w.inTrash() && w.hasSelection(); },
       [](FilesWindow& w) { w.toggleStar(); }},
      {"edit.copy-location", i18n::tr("files.commands.edit-copy-location"), "Edit", chord(XKB_KEY_c, KeyMod::Ctrl | KeyMod::Alt),
       always, [](FilesWindow& w) { w.copyLocation(); }},
      {"edit.select-pattern", i18n::tr("files.commands.edit-select-pattern"), "Edit", chord(XKB_KEY_s, KeyMod::Ctrl),
       [](const FilesWindow& w) { return !w.inTrash() || w.trashHasItems(); }, [](FilesWindow& w) { w.selectByPattern(); }},
      {"file.new-smart-folder", i18n::tr("files.commands.file-new-smart-folder"), "File", std::nullopt, [](const FilesWindow& w) { return !w.isPicker(); },
       [](FilesWindow& w) { w.newSmartFolder(false); }},
      {"edit.save-search", i18n::tr("files.commands.edit-save-search"), "Edit", std::nullopt, [](const FilesWindow& w) { return w.canSaveSearch(); },
       [](FilesWindow& w) { w.newSmartFolder(true); }},
      {"file.edit-smart-folder", i18n::tr("files.commands.file-edit-smart-folder"), "File", std::nullopt,
       [](const FilesWindow& w) { return w.inSmartFolder(); }, [](FilesWindow& w) { w.editSmartFolder(); }},
      {"file.remove-smart-folder", i18n::tr("files.commands.file-remove-smart-folder"), "File", std::nullopt,
       [](const FilesWindow& w) { return w.inSmartFolder(); }, [](FilesWindow& w) { w.removeSmartFolder(); }},
      {"view.split", i18n::tr("files.commands.view-split"), "View", std::nullopt, [](const FilesWindow& w) { return !w.isPicker(); },
       [](FilesWindow& w) { w.toggleSplitView(); }},
      {"view.compare", i18n::tr("files.commands.view-compare"), "View", std::nullopt, [](const FilesWindow& w) { return w.canCompare(); },
       [](FilesWindow& w) { w.toggleCompare(); }},
      {"view.sync", i18n::tr("files.commands.view-sync"), "View", std::nullopt, [](const FilesWindow& w) { return w.comparing(); },
       [](FilesWindow& w) { w.syncCompare(); }},
      {"view.terminal", i18n::tr("files.commands.view-terminal"), "View", chord(XKB_KEY_F4, 0), [](const FilesWindow& w) { return !w.isPicker(); },
       [](FilesWindow& w) { w.toggleTerminal(); }},
      {"view.terminal-split", i18n::tr("files.commands.view-terminal-split"), "View", std::nullopt,
       [](const FilesWindow& w) { return w.terminalShown(); }, [](FilesWindow& w) { w.terminal()->split(); }},
      {"view.terminal-close", i18n::tr("files.commands.view-terminal-close"), "View", std::nullopt,
       [](const FilesWindow& w) { return w.terminalShown(); }, [](FilesWindow& w) { w.terminal()->closeActive(); }},
      {"file.open-terminal", i18n::tr("files.commands.file-open-terminal"), "File", chord(XKB_KEY_F4, KeyMod::Shift),
       [](const FilesWindow& w) { return !w.inTrash(); }, [](FilesWindow& w) { w.openTerminalHere(); }},
      {"file.open-admin", i18n::tr("files.commands.file-open-admin"), "File", std::nullopt,
       [](const FilesWindow& w) { return !w.inTrash() && !w.isPicker(); }, [](FilesWindow& w) { w.openAsAdmin(); }},
      {"file.restore", i18n::tr("files.commands.file-restore"), "File", std::nullopt,
       [](const FilesWindow& w) { return w.inTrash() && w.hasSelection(); }, [](FilesWindow& w) { w.restoreSelection(); }},
      {"file.new-folder", i18n::tr("files.commands.file-new-folder"), "File", chord(XKB_KEY_N, KeyMod::Ctrl | KeyMod::Shift),
       [](const FilesWindow& w) { return w.canAddHere(); }, [](FilesWindow& w) { w.newFolder(); }},
      {"edit.find", i18n::tr("files.commands.edit-find"), "Edit", chord(XKB_KEY_f, KeyMod::Ctrl), [](const FilesWindow&) { return true; },
       [](FilesWindow& w) { w.focusSearch(); }},
      {"go.connect", i18n::tr("files.commands.go-connect"), "Go", chord(XKB_KEY_k, KeyMod::Ctrl), [](const FilesWindow&) { return true; },
       [](FilesWindow& w) { w.openConnect(""); }},
      {"go.path", i18n::tr("files.commands.go-path"), "Go", chord(XKB_KEY_l, KeyMod::Ctrl), always, [](FilesWindow& w) { w.openPathField(""); }},
      {"file.info", i18n::tr("files.commands.file-info"), "File", chord(XKB_KEY_i, KeyMod::Ctrl), [](const FilesWindow& w) { return !w.inTrash() && w.hasSelection(); },
       [](FilesWindow& w) { w.getInfo(); }},
      {"trash.empty", i18n::tr("files.commands.trash-empty"), "File", chord(XKB_KEY_Delete, KeyMod::Ctrl | KeyMod::Shift),
       [](const FilesWindow& w) { return w.trashHasItems(); }, [](FilesWindow& w) { w.askEmptyTrash(); }},
      {"file.tags", i18n::tr("files.commands.file-tags"), "File", std::nullopt, [](const FilesWindow& w) { return !w.isPicker(); },
       [](FilesWindow& w) { w.openTagManager(); }},
      {"file.make-link", i18n::tr("files.commands.file-make-link"), "File", std::nullopt, [](const FilesWindow& w) { return w.inFolder() && w.hasSelection(); },
       [](FilesWindow& w) { w.makeLink(); }},
      {"file.compress", i18n::tr("files.commands.file-compress"), "File", std::nullopt, [](const FilesWindow& w) { return !w.inTrash() && w.hasSelection(); },
       [](FilesWindow& w) { w.compressSelection(); }},
      {"file.extract-here", i18n::tr("files.commands.file-extract-here"), "File", std::nullopt, [](const FilesWindow& w) { return w.canExtract(); },
       [](FilesWindow& w) { w.extractSelection(true); }},
      {"file.extract-to", i18n::tr("files.commands.file-extract-to"), "File", std::nullopt, [](const FilesWindow& w) { return w.canExtract(); },
       [](FilesWindow& w) { w.extractSelection(false); }},
      {"file.rename", i18n::tr("files.commands.file-rename"), "File", chord(XKB_KEY_F2, 0),
       [](const FilesWindow& w) { return !w.inTrash() && w.hasSelection(); }, [](FilesWindow& w) { w.renameSelection(); }},
      {"edit.copy", i18n::tr("desk.edit.copy"), "Edit", chord(XKB_KEY_c, KeyMod::Ctrl), selection, [](FilesWindow& w) { w.copySelection(false); }},
      {"edit.cut", i18n::tr("desk.edit.cut"), "Edit", chord(XKB_KEY_x, KeyMod::Ctrl), [](const FilesWindow& w) { return w.hasSelection() && !w.inAdmin(); },
       [](FilesWindow& w) { w.copySelection(true); }},
      {"edit.paste", i18n::tr("desk.edit.paste"), "Edit", chord(XKB_KEY_v, KeyMod::Ctrl), [](const FilesWindow& w) { return w.canAddHere(); },
       [](FilesWindow& w) { w.paste(); }},
      {"edit.trash", i18n::tr("files.commands.edit-trash"), "Edit", chord(XKB_KEY_Delete, 0),
       [](const FilesWindow& w) { return !w.inTrash() && w.hasSelection(); }, [](FilesWindow& w) { w.trashSelection(); }},
      {"edit.delete", i18n::tr("files.commands.edit-delete"), "Edit", chord(XKB_KEY_Delete, KeyMod::Shift),
       [](const FilesWindow& w) { return !w.inTrash() && w.hasSelection(); }, [](FilesWindow& w) { w.askDeleteSelection(); }},
      {"edit.undo", i18n::tr("files.commands.edit-undo"), "Edit", chord(XKB_KEY_z, KeyMod::Ctrl), [](const FilesWindow& w) { return w.canUndo(); },
       [](FilesWindow& w) { w.undo(); }},
      {"edit.redo", i18n::tr("files.commands.edit-redo"), "Edit", chord(XKB_KEY_Z, KeyMod::Ctrl | KeyMod::Shift),
       [](const FilesWindow& w) { return w.canRedo(); }, [](FilesWindow& w) { w.redo(); }, chord(XKB_KEY_y, KeyMod::Ctrl)},
      {"edit.select-all", i18n::tr("desk.edit.select-all"), "Edit", chord(XKB_KEY_a, KeyMod::Ctrl), always, [](FilesWindow& w) { w.selectAll(); }},
      {"edit.invert-selection", i18n::tr("files.commands.edit-invert-selection"), "Edit", chord(XKB_KEY_I, KeyMod::Ctrl | KeyMod::Shift), always,
       [](FilesWindow& w) { w.invertSelection(); }},
      {"view.hidden", i18n::tr("files.commands.view-hidden"), "View", chord(XKB_KEY_h, KeyMod::Ctrl), always,
       [](FilesWindow& w) { w.toggleHidden(); }},
      {"edit.copy-other-pane", i18n::tr("files.commands.edit-copy-other-pane"), "Edit", chord(XKB_KEY_F5, 0),
       [](const FilesWindow& w) { return w.canCompare() && w.hasSelection(); },
       [](FilesWindow& w) { w.sendToOtherPane(false); }},
      {"edit.move-other-pane", i18n::tr("files.commands.edit-move-other-pane"), "Edit", chord(XKB_KEY_F6, 0),
       [](const FilesWindow& w) { return w.canCompare() && w.hasSelection(); },
       [](FilesWindow& w) { w.sendToOtherPane(true); }},
      // F5 reloads only while Copy to Other Pane is off: the first enabled match wins.
      {"view.reload", i18n::tr("files.commands.view-reload"), "View", chord(XKB_KEY_r, KeyMod::Ctrl), always, [](FilesWindow& w) { w.reload(); },
       chord(XKB_KEY_F5, 0)},
      {"view.extensions", i18n::tr("files.commands.view-extensions"), "View", std::nullopt, always,
       [](FilesWindow& w) { w.browser().setShowExtensions(!w.browser().showExtensions()); }},
      {"view.group-none", i18n::tr("files.commands.view-group-none"), "View", std::nullopt, always,
       [](FilesWindow& w) { w.groupBy(FileGroupBy::None); }},
      {"view.group-kind", i18n::tr("files.commands.view-group-kind"), "View", std::nullopt, always,
       [](FilesWindow& w) { w.groupBy(FileGroupBy::Kind); }},
      {"view.group-date", i18n::tr("files.commands.view-group-date"), "View", std::nullopt, always,
       [](FilesWindow& w) { w.groupBy(FileGroupBy::Date); }},
      {"view.group-size", i18n::tr("files.commands.view-group-size"), "View", std::nullopt, always,
       [](FilesWindow& w) { w.groupBy(FileGroupBy::Size); }},
      {"view.group-tags", i18n::tr("files.commands.view-group-tags"), "View", std::nullopt, always,
       [](FilesWindow& w) { w.groupBy(FileGroupBy::Tags); }},
      {"view.options", i18n::tr("files.commands.view-options"), "View", chord(XKB_KEY_j, KeyMod::Ctrl),
       [](const FilesWindow& w) { return !w.isPicker(); }, [](FilesWindow& w) { w.openViewOptions(); }},
      {"view.zoom-in", i18n::tr("files.commands.view-zoom-in"), "View", chord(XKB_KEY_plus, KeyMod::Ctrl), always,
       [](FilesWindow& w) { w.stepIconSize(1); }, chord(XKB_KEY_equal, KeyMod::Ctrl)},
      {"view.zoom-out", i18n::tr("files.commands.view-zoom-out"), "View", chord(XKB_KEY_minus, KeyMod::Ctrl), always,
       [](FilesWindow& w) { w.stepIconSize(-1); }},
      {"view.zoom-reset", i18n::tr("files.commands.view-zoom-reset"), "View", chord(XKB_KEY_0, KeyMod::Ctrl), always,
       [](FilesWindow& w) { w.stepIconSize(0); }},
      {"view.details", i18n::tr("files.commands.view-details"), "View", chord(XKB_KEY_p, KeyMod::Alt), [](const FilesWindow& w) { return !w.isPicker(); },
       [](FilesWindow& w) { w.toggleDetails(); }},
      // Ctrl+(Shift+)Tab lives in FileBrowser::onKey (pickers have no commands); these entries put the tabs in the More menu.
      {"file.new-tab", i18n::tr("files.commands.file-new-tab"), "File", chord(XKB_KEY_t, KeyMod::Ctrl), [](const FilesWindow& w) { return !w.isPicker(); },
       [](FilesWindow& w) { w.newTab(); }},
      {"view.next-tab", i18n::tr("files.commands.view-next-tab"), "View", std::nullopt, [](const FilesWindow& w) { return w.tabCount() > 1; },
       [](FilesWindow& w) { w.stepTab(1); }},
      {"view.previous-tab", i18n::tr("files.commands.view-previous-tab"), "View", std::nullopt,
       [](const FilesWindow& w) { return w.tabCount() > 1; }, [](FilesWindow& w) { w.stepTab(-1); }},
      {"picker.accept", i18n::tr("files.commands.picker-accept"), "Picker", std::nullopt, [](const FilesWindow& w) { return w.isPicker(); },
       [](FilesWindow& w) { w.pickerAccept(); }},
      {"picker.cancel", i18n::tr("common.actions.cancel"), "Picker", chord(XKB_KEY_Escape, 0), [](const FilesWindow& w) { return w.isPicker(); },
       [](FilesWindow& w) { w.pickerCancel(); }},
      {"conflict.replace", i18n::tr("files.commands.conflict-replace"), "Conflict", std::nullopt, conflict,
       [](FilesWindow& w) { w.resolveConflict(FileOpsClient::Resolution::Replace); }},
      {"conflict.skip", i18n::tr("files.commands.conflict-skip"), "Conflict", std::nullopt, conflict,
       [](FilesWindow& w) { w.resolveConflict(FileOpsClient::Resolution::Skip); }},
      {"conflict.keep-both", i18n::tr("files.commands.conflict-keep-both"), "Conflict", std::nullopt, conflict,
       [](FilesWindow& w) { w.resolveConflict(FileOpsClient::Resolution::KeepBoth); }},
      {"conflict.apply-all", i18n::tr("files.commands.conflict-apply-all"), "Conflict", std::nullopt, conflict,
       [](FilesWindow& w) { w.toggleApplyToAll(); }},
  };
  for (const Spec& spec : specs) {
    commands().add({
        .id = spec.id,
        .label = spec.label,
        .group = spec.group,
        .shortcut = spec.shortcut,
        .enabled = [this, enabled = spec.enabled, chooser = allowedInPicker(spec.id),
                    edit = std::string_view(spec.group) == "Edit",
                    ctrlKey = spec.shortcut.has_value() && (spec.shortcut->modifiers & KeyMod::Ctrl) != 0 &&
                              (spec.shortcut->modifiers & KeyMod::Alt) == 0] {
          const FilesWindow* w = activeFilesWindow();
          // The inline rename field keeps Ctrl+A/C/X/V/Z for its text; the terminal pane keeps every Ctrl key
          // (Ctrl+C is SIGINT there).
          return w != nullptr && (chooser || !w->isPicker()) && !(edit && w->renaming()) &&
                 !(ctrlKey && w->terminalFocused()) && enabled(*w);
        },
        .run = [this, run = spec.run] {
          if (FilesWindow* w = activeFilesWindow()) {
            run(*w);
          }
        },
        .alternate = spec.alternate,
    });
  }
  // The header × closes the window with all its tabs; Ctrl+W closes the current tab first, the window only with one
  // tab left (Finder, Dolphin). Close Window gives up Ctrl+W first: a held shortcut cannot be taken.
  commands().add({
      .id = "window.close",
      .label = i18n::tr("desk.window.close"),
      .group = "Window",
      .run = [this] {
        if (AppWindow* window = activeWindow()) {
          closeWindow(*window);
        }
      },
  });
  commands().add({
      .id = "window.close-tab",
      .label = i18n::tr("files.commands.window-close-tab"),
      .group = "Window",
      .shortcut = KeyChord{.sym = XKB_KEY_w, .modifiers = KeyMod::Ctrl},
      .run = [this] {
        FilesWindow* w = activeFilesWindow();
        if (w != nullptr && w->closeTab()) {
          return;
        }
        if (AppWindow* window = activeWindow()) {
          closeWindow(*window);
        }
      },
  });

  commands().add({
      .id = "file.new-window",
      .label = i18n::tr("files.commands.file-new-window"),
      .group = "File",
      .shortcut = chord(XKB_KEY_n, KeyMod::Ctrl),
      .enabled = [this] { return !m_picker; },
      .run = [this] {
        const FilesWindow* w = activeFilesWindow();
        openWindow(w != nullptr && !w->inTrash() ? w->location() : fs::path(g_get_home_dir()));
      },
  });
}
