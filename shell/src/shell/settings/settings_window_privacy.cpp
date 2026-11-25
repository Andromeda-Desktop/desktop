#include "auth/biometric_enrollment.h"
#include "auth/password_changer.h"
#include "core/process/process.h"
#include "core/shared_worker.h"
#include "config/config_service.h"
#include "dbus/system_interactive_call.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_alert.h"
#include "shell/settings/settings_content_privacy.h"
#include "shell/settings/settings_window.h"
#include "compositors/andromeda/andromeda_runtime.h"
#include "compositors/compositor_platform.h"
#include "system/app_permissions.h"
#include "system/desktop_entry.h"
#include "ui/dialogs/file_dialog.h"

#include <nlohmann/json.hpp>
#include "util/parse_number.h"

#include <systemd/sd-bus.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <sstream>

namespace gs = settings::global_shortcuts;

namespace {

  template <class... F> struct Overload : F... {
    using F::operator()...;
  };

  const DesktopEntry* findAppEntry(const std::vector<DesktopEntry>& entries, const std::string& app) {
    const auto found = std::ranges::find_if(entries, [&](const DesktopEntry& entry) {
      return entry.id == app || entry.id == app + ".desktop";
    });
    return found != entries.end() ? &*found : nullptr;
  }

  std::string appEntryName(const std::string& app) {
    const auto entries = desktopEntriesSnapshot();
    const DesktopEntry* entry = findAppEntry(*entries, app);
    return entry != nullptr && !entry->name.empty() ? entry->name : app;
  }

  constexpr auto kBiometricsAction = "org.andromeda.biometrics.manage";
  constexpr auto kFaceAdminHelper = "/usr/lib/andromeda/andromeda-face-admin";
  constexpr auto kFingerAdminHelper = "/usr/lib/andromeda/andromeda-finger-admin";

  // nullopt: pam_gaze is not in /etc/pam.d/sudo; true: its line is active (not "#andromeda-off").
  std::optional<bool> faceAdminState() {
    std::ifstream file("/etc/pam.d/sudo");
    for (std::string line; std::getline(file, line);) {
      if (line.find("pam_gaze.so") == std::string::npos || line.find("sufficient") == std::string::npos) {
        continue;
      }
      return !line.starts_with("#");
    }
    return std::nullopt;
  }

  // An active pam_fprintd line in sudo, or in the stack every login includes (Debian's pam-auth-update).
  bool fingerAdminState() {
    for (const char* path : {"/etc/pam.d/sudo", "/etc/pam.d/common-auth", "/etc/pam.d/system-auth"}) {
      std::ifstream file(path);
      for (std::string line; std::getline(file, line);) {
        if (line.starts_with("auth") && line.find("pam_fprintd.so") != std::string::npos) {
          return true;
        }
      }
    }
    return false;
  }

  std::string currentUser() {
    const passwd* pw = getpwuid(getuid());
    return pw != nullptr && pw->pw_name != nullptr ? pw->pw_name : "";
  }

  // Field 22 of /proc/self/stat: polkit pairs it with the pid so a reused pid is not trusted.
  std::uint64_t processStartTime() {
    std::ifstream file("/proc/self/stat");
    std::string stat((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto close = stat.rfind(')');
    if (close == std::string::npos) {
      return 0;
    }
    std::istringstream fields(stat.substr(close + 2));
    std::string field;
    // After ")" the fields start at 3 (state); start-time is field 22.
    for (int i = 3; i <= 22 && (fields >> field); ++i) {
    }
    return parseNumber<std::uint64_t>(field).value_or(0);
  }

} // namespace

void SettingsWindow::authorizeBiometrics(std::function<void()> then) {
  if (m_systemBus == nullptr) {
    return;
  }
  constexpr std::uint32_t kAllowUserInteraction = 1;
  dbus::callInteractive(
      *m_systemBus, "org.freedesktop.PolicyKit1", "/org/freedesktop/PolicyKit1/Authority",
      "org.freedesktop.PolicyKit1.Authority", "CheckAuthorization",
      [](sd_bus_message* m) {
        return sd_bus_message_append(
            m, "(sa{sv})sa{ss}us", "unix-process", 2, "pid", "u", static_cast<std::uint32_t>(getpid()), "start-time",
            "t", static_cast<std::uint64_t>(processStartTime()), kBiometricsAction, 0, kAllowUserInteraction, ""
        );
      },
      [this, alive = std::weak_ptr<void>(m_aliveGuard), then = std::move(then)](sd_bus_message* result, std::string error) {
        if (alive.expired()) {
          return;
        }
        int authorized = 0;
        if (result != nullptr && sd_bus_message_enter_container(result, 'r', "bba{ss}") < 0) {
          result = nullptr;
        }
        if (result != nullptr) {
          sd_bus_message_read(result, "b", &authorized);
        }
        if (authorized == 0) {
          m_privacyStatus = error.empty() ? i18n::tr("settings.privacy.not-allowed") : error;
          if (isOpen()) {
            requestContentRebuild();
          }
          return;
        }
        then();
      }
  );
}

// passwd runs on SharedWorker: a wrong password holds it for the pam_faillock delay (about 2 s), a stalled PAM
// stack up to a minute (password_changer.h).
void SettingsWindow::changePassword() {
  settings::PasswordDraft& draft = m_passwordDraft;
  if (draft.busy) {
    return;
  }
  const auto refuse = [this, &draft](const char* key) {
    draft.status = i18n::tr(key);
    draft.statusError = true;
    requestContentRebuild();
  };
  if (draft.current.empty() || draft.next.empty()) {
    refuse("settings.privacy.password-empty");
    return;
  }
  if (draft.next != draft.verify) {
    refuse("settings.privacy.password-mismatch");
    return;
  }
  draft.busy = true;
  draft.status.clear();
  requestContentRebuild();
  SharedWorker::instance().post<password_change::Result>(
      m_aliveGuard,
      [current = draft.current, next = draft.next]() mutable { return password_change::change(current, next); },
      [this](password_change::Result result) {
        settings::PasswordDraft& d = m_passwordDraft;
        d.busy = false;
        d.statusError = !result.ok;
        if (result.ok) {
          d.status = i18n::tr(result.keyringFailed ? "settings.privacy.password-changed-keyring"
                                                   : "settings.privacy.password-changed");
        } else if (result.wrongCurrent) {
          d.status = i18n::tr("settings.privacy.password-wrong");
        } else {
          d.status = result.message.empty() ? i18n::tr("settings.privacy.change-failed") : result.message;
        }
        for (std::string* field : {&d.current, &d.next, &d.verify}) {
          std::fill(field->begin(), field->end(), '\0');
          field->clear();
        }
        if (isOpen()) {
          requestContentRebuild();
        }
      }
  );
}

void SettingsWindow::loadAppAccess() {
  static constexpr std::pair<const char*, const char*> kMeta[] = {
      {"settings.privacy.camera", "camera"},           {"settings.privacy.location", "map-pin"},
      {"settings.privacy.screenshots", "screenshot"},  {"settings.privacy.screen-sharing", "screen-share"},
      {"settings.privacy.remote-control", "device-desktop-cog"},
  };
  const auto& kinds = AppPermissions::kinds();
  if (m_access.empty()) {
    for (std::size_t i = 0; i < kinds.size(); ++i) {
      m_access.push_back({.table = kinds[i].table, .titleKey = kMeta[i].first, .glyph = kMeta[i].second,
                          .tokens = kinds[i].id.empty()});
    }
  }
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    m_permissions->load(kinds[i], [this, i](AppPermissions::Grants grants) {
      auto& view = m_access[i];
      view.loaded = true;
      view.apps.clear();
      for (const auto& grant : grants) {
        view.apps.push_back({.id = grant.app,
                             .label = grant.app.empty() ? i18n::tr("settings.privacy.host-apps") : grant.app,
                             .allowed = grant.allowed});
      }
      if (isOpen() && m_selectedSection == "privacy") {
        requestContentRebuild();
      }
    });
  }
}

void SettingsWindow::ensureEnrollment() {
  if (m_enroll != nullptr || m_systemBus == nullptr) {
    return;
  }
  m_enroll = std::make_unique<BiometricEnrollment>(m_systemBus, currentUser());
  m_enroll->setChangedCallback([this] {
    if (!isOpen()) {
      return;
    }
    // A sensor found or lost changes which Lock Screen rows exist.
    const std::pair present{m_enroll->facePresent(), m_enroll->fingerPresent()};
    const bool presenceChanged = std::exchange(m_biometricPresence, present) != present;
    if (presenceChanged) {
      requestContentRebuild(/*refreshRegistry=*/true, /*refreshFilterRow=*/true);
    } else if (m_selectedSection == "privacy") {
      requestContentRebuild();
    }
  });
  m_enroll->setFinishedCallback([this](BiometricEnrollment::Method /*method*/, bool ok) {
    m_privacyStatus = ok ? std::string{} : i18n::tr("settings.privacy.enroll-failed");
  });
  m_enroll->probe();
}

void SettingsWindow::leavePrivacy() {
  // Runs on every rebuild of another page, so it must not request a rebuild itself.
  for (const gs::Action& action : m_globalShortcuts.handle(gs::Leave{})) {
    if (std::holds_alternative<gs::CancelCapture>(action) && m_compositorSettings != nullptr) {
      m_compositorSettings->cancelCapture();
    }
  }
  m_trustedApps.load = settings::TrustedAppsView::Load::NotLoaded;
  if (m_enroll != nullptr && m_enroll->enrolling() != BiometricEnrollment::Method::None) {
    m_enroll->stop();
  }
  if (!m_passwordDraft.busy) {
    for (std::string* field : {&m_passwordDraft.current, &m_passwordDraft.next, &m_passwordDraft.verify}) {
      std::fill(field->begin(), field->end(), '\0');
      field->clear();
    }
    m_passwordDraft.status.clear();
  }
}

void SettingsWindow::addPrivacyContent(float scale) {
  if (m_selectedSection != "privacy") {
    leavePrivacy();
    return;
  }
  if (m_permissions == nullptr && m_sessionBus != nullptr) {
    m_permissions = std::make_unique<AppPermissions>(*m_sessionBus);
    loadAppAccess();
  }
  if (m_globalShortcuts.load == gs::Load::Loading) {
    (void)m_globalShortcuts.handle(gs::Loaded{.apps = fetchGlobalShortcuts()});
  }
  if (m_trustedApps.load == settings::TrustedAppsView::Load::NotLoaded) {
    loadTrustedApps();
  }
  ensureEnrollment();
  const auto& lock = m_config->config().lockscreen;
  // A 250-byte root-readable file in /etc: no disk wait worth a worker.
  const std::optional<bool> faceAdmin = faceAdminState();
  BiometricEnrollment* enroll = m_enroll.get();
  const auto done = [this](bool ok) {
    m_privacyStatus = ok ? std::string{} : i18n::tr("settings.privacy.change-failed");
    if (isOpen()) {
      requestContentRebuild();
    }
  };
  const auto confirmRemove = [this, scale](std::string name, std::function<void()> remove) {
    settings::showSettingsAlert(
        m_modalHost,
        settings::SettingsAlertRequest{
            .title = i18n::tr("settings.privacy.remove-confirm", "name", name),
            .buttons = {
                {.text = i18n::tr("common.actions.cancel")},
                {.text = i18n::tr("settings.privacy.remove"),
                 .variant = ButtonVariant::Destructive,
                 .onClick = std::move(remove)},
            },
            // Cancel is the default for a destructive action, so Enter never removes.
            .defaultIndex = 0,
            .scale = scale,
        }
    );
  };
  const auto enrolling = enroll == nullptr ? BiometricEnrollment::Method::None : enroll->enrolling();
  settings::addSettingsPrivacy(
      *m_contentContainer,
      settings::SettingsPrivacyContext{
          .scale = scale,
          .detail = m_detail,
          .biometricsKnown = enroll != nullptr,
          .facePresent = enroll != nullptr && enroll->facePresent(),
          .fingerPresent = enroll != nullptr && enroll->fingerPresent(),
          .fprintdMissing = enroll != nullptr && enroll->fprintdMissing(),
          .faces = enroll != nullptr ? enroll->faces() : std::vector<std::string>{},
          .fingers = enroll != nullptr ? enroll->fingers() : std::vector<std::string>{},
          .enrolling = enrolling == BiometricEnrollment::Method::Face ? 1
                       : enrolling == BiometricEnrollment::Method::Finger ? 2
                                                                          : 0,
          .enrollProgress = enroll != nullptr ? enroll->progress() : 0.0F,
          .enrollPrompt = enroll != nullptr ? enroll->prompt() : std::string{},
          .status = m_privacyStatus,
          .faceUnlock = lock.face,
          .faceOnRequest = lock.faceOnRequest,
          .fingerprintUnlock = lock.fingerprint,
          .faceAdminKnown = faceAdmin.has_value() && ::access(kFaceAdminHelper, X_OK) == 0,
          .faceAdmin = faceAdmin.value_or(false),
          .setFaceAdmin =
              [this](bool on) {
                const bool started = process::runAsync(
                    std::vector<std::string>{"pkexec", kFaceAdminHelper, on ? "on" : "off"},
                    process::RunCallbacks{
                        .onExit =
                            [this, alive = std::weak_ptr<void>(m_aliveGuard)](const process::RunResult& result) {
                              if (alive.expired()) {
                                return;
                              }
                              m_privacyStatus = result ? std::string{} : i18n::tr("settings.privacy.change-failed");
                              if (isOpen()) {
                                requestContentRebuild();
                              }
                            },
                    }
                );
                if (!started) {
                  m_privacyStatus = i18n::tr("settings.privacy.change-failed");
                  requestContentRebuild();
                }
              },
          .fingerAdminKnown = ::access(kFingerAdminHelper, X_OK) == 0,
          .fingerAdmin = fingerAdminState(),
          .setFingerAdmin =
              [this](bool on) {
                const bool started = process::runAsync(
                    std::vector<std::string>{"pkexec", kFingerAdminHelper, on ? "on" : "off"},
                    process::RunCallbacks{
                        .onExit =
                            [this, alive = std::weak_ptr<void>(m_aliveGuard)](const process::RunResult& result) {
                              if (alive.expired()) {
                                return;
                              }
                              m_privacyStatus = result ? std::string{} : i18n::tr("settings.privacy.change-failed");
                              if (isOpen()) {
                                requestContentRebuild();
                              }
                            },
                    }
                );
                if (!started) {
                  m_privacyStatus = i18n::tr("settings.privacy.change-failed");
                  requestContentRebuild();
                }
              },
          .openDetail =
              [this](std::string detail) {
                m_privacyStatus.clear();
                openDetail(std::move(detail));
              },
          .back =
              [this] {
                m_privacyStatus.clear();
                leavePrivacy();
                leaveDetail();
              },
          .setBool = [this](std::vector<std::string> path, bool value) { setSettingOverride(std::move(path), value); },
          // gazed asks polkit itself (com.gundulabs.gaze.manage-faces) before it stores a face.
          .addFace =
              [this, enroll] {
                m_privacyStatus.clear();
                enroll->start(BiometricEnrollment::Method::Face);
              },
          .addFinger =
              [this, enroll] {
                m_privacyStatus.clear();
                authorizeBiometrics([enroll] { enroll->start(BiometricEnrollment::Method::Finger); });
              },
          .cancelEnroll = [this, enroll] {
            enroll->stop();
            requestContentRebuild();
          },
          .removeFace =
              [enroll, done, confirmRemove](std::string name) {
                confirmRemove(name, [enroll, done, name] { enroll->removeFace(name, done); });
              },
          .renameFace =
              [this, enroll, done](std::string from, std::string to) {
                enroll->renameFace(from, to, [this, done](bool ok) {
                  done(ok);
                  if (ok && m_detail.ends_with("\trename")) {
                    leaveDetail();
                  }
                });
              },
          .removeFinger =
              [this, enroll, done, confirmRemove](std::string finger) {
                confirmRemove(BiometricEnrollment::fingerLabel(finger), [this, enroll, done, finger] {
                  authorizeBiometrics([enroll, done, finger] { enroll->removeFinger(finger, done); });
                });
              },
          .password = &m_passwordDraft,
          .access = m_access,
          .setAccess =
              [this](std::string table, std::string app, bool allowed) {
                const auto& kinds = AppPermissions::kinds();
                const auto kind = std::ranges::find(kinds, table, &AppPermissions::Kind::table);
                if (kind == kinds.end() || m_permissions == nullptr) {
                  return;
                }
                // The grant's store entries come from a fresh load: the view keeps only what the rows show.
                m_permissions->load(*kind, [this, kind = *kind, app, allowed](AppPermissions::Grants grants) {
                  const auto grant = std::ranges::find(grants, app, &AppPermissions::Grant::app);
                  if (grant == grants.end()) {
                    loadAppAccess();
                    return;
                  }
                  m_permissions->setAllowed(kind, *grant, allowed, [this](bool ok) {
                    m_privacyStatus = ok ? std::string{} : i18n::tr("settings.privacy.change-failed");
                    loadAppAccess();
                  });
                });
              },
          .shortcuts = &m_globalShortcuts,
          .shortcutEvent = [this, scale](gs::Event event) { globalShortcutsEvent(std::move(event), scale); },
          .toggledShortcutApps = &m_toggledShortcutApps,
          .toggleShortcutApp =
              [this](std::string app) {
                if (!m_toggledShortcutApps.erase(app)) {
                  m_toggledShortcutApps.insert(std::move(app));
                }
                requestContentRebuild();
              },
          .appName = [](const std::string& app) { return appEntryName(app); },
          .appIcon =
              [this](const std::string& app, float size) -> std::unique_ptr<Node> {
                const auto entries = desktopEntriesSnapshot();
                const DesktopEntry* entry = findAppEntry(*entries, app);
                return entry != nullptr ? appIcon(*entry, size) : nullptr;
              },
          .trustedApps = &m_trustedApps,
          .addTrustedApp = [this] { addTrustedApp(); },
          .removeTrustedApp = [this](std::string path) { removeTrustedApp(path); },
          .openKeyboardShortcuts = [this] { navigateTo("shortcuts"); },
          .changePassword = [this] { changePassword(); },
      }
  );
}

void SettingsWindow::globalShortcutsEvent(gs::Event event, float scale) {
  const bool wasConfirm = std::holds_alternative<gs::Confirm>(m_globalShortcuts.edit);
  for (gs::Action& action : m_globalShortcuts.handle(std::move(event))) {
    std::visit(
        Overload{
            [this, scale](gs::Capture&) {
              if (m_compositorSettings == nullptr) {
                globalShortcutsEvent(gs::Captured{.chord = {}}, scale);
                return;
              }
              m_compositorSettings->captureChord([this, scale](std::string chord) {
                globalShortcutsEvent(gs::Captured{.chord = std::move(chord)}, scale);
              });
            },
            [this](gs::CancelCapture&) {
              if (m_compositorSettings != nullptr) {
                m_compositorSettings->cancelCapture();
              }
            },
            [this, scale](gs::Assign& assign) {
              const std::string arg =
                  assign.app + " " + assign.id + " " + (assign.chord.empty() ? std::string("off") : assign.chord);
              const auto reply = m_platform == nullptr
                                     ? std::nullopt
                                     : m_platform->andromedaRuntime().requestCommand("global-shortcuts-assign", arg);
              gs::Assigned result{.result = gs::Assigned::Result::Failed, .holder = {}};
              if (reply.has_value()) {
                const nlohmann::json& ok = (*reply)["ok"];
                const std::string reason = ok.value("reason", "");
                result.result = ok.value("assigned", false) ? gs::Assigned::Result::Ok
                                : reason == "in_use"        ? gs::Assigned::Result::InUse
                                                            : gs::Assigned::Result::Reserved;
                result.holder = ok.value("holder", "");
              }
              globalShortcutsEvent(std::move(result), scale);
            },
            [this, scale](gs::Revoke& revoke) {
              const bool ok = m_platform != nullptr
                              && m_platform->andromedaRuntime()
                                     .requestCommand("global-shortcuts-revoke", revoke.app.empty() ? "all" : revoke.app)
                                     .has_value();
              globalShortcutsEvent(gs::Removed{.ok = ok}, scale);
            },
            [this, scale](gs::Reload&) { globalShortcutsEvent(gs::Loaded{.apps = fetchGlobalShortcuts()}, scale); },
        },
        action
    );
  }
  if (const auto* confirm = std::get_if<gs::Confirm>(&m_globalShortcuts.edit); confirm != nullptr && !wasConfirm) {
    const bool all = confirm->app.empty();
    const std::string name = all ? std::string{} : appEntryName(confirm->app);
    const auto answer = [this, scale](bool remove) { globalShortcutsEvent(gs::Answer{.remove = remove}, scale); };
    settings::showSettingsAlert(
        m_modalHost,
        settings::SettingsAlertRequest{
            .title = all ? i18n::tr("settings.privacy.shortcuts-remove-all-title")
                         : i18n::tr("settings.privacy.shortcuts-remove-title", "app", name),
            .body = all ? i18n::tr("settings.privacy.shortcuts-remove-all-body", "count",
                                   std::to_string(m_globalShortcuts.apps.size()))
                        : i18n::tr("settings.privacy.shortcuts-remove-body", "app", name),
            .buttons = {
                {.text = i18n::tr("common.actions.cancel"), .onClick = [answer] { answer(false); }},
                {.text = i18n::tr(all ? "settings.privacy.shortcuts-remove-all-button" : "settings.privacy.remove"),
                 .variant = ButtonVariant::Destructive,
                 .onClick = [answer] { answer(true); }},
            },
            // Cancel is the default for a destructive action, so Enter never removes.
            .defaultIndex = 0,
            .scale = scale,
            .onDismissed = [answer] { answer(false); },
        }
    );
  }
  if (isOpen()) {
    requestContentRebuild();
  }
}

std::optional<std::vector<gs::App>> SettingsWindow::fetchGlobalShortcuts() const {
  if (m_platform == nullptr) {
    return std::nullopt;
  }
  const auto reply = m_platform->andromedaRuntime().requestCommand("global-shortcuts");
  return reply.has_value() ? gs::parseApps((*reply)["ok"]) : std::nullopt;
}

void SettingsWindow::loadTrustedApps() {
  using Load = settings::TrustedAppsView::Load;
  const auto reply = m_platform == nullptr ? std::unexpected(std::string{})
                                           : m_platform->andromedaRuntime().commandResult("trusted-binaries");
  m_trustedApps.paths.clear();
  if (!reply.has_value() || !reply->is_array()) {
    m_trustedApps.load = Load::Unavailable;
    return;
  }
  for (const auto& path : *reply) {
    if (path.is_string()) {
      m_trustedApps.paths.push_back(path.get<std::string>());
    }
  }
  m_trustedApps.load = Load::Ready;
}

void SettingsWindow::addTrustedApp() {
  FileDialogOptions options;
  options.title = i18n::tr("settings.privacy.screen-input-add-title");
  options.startDirectory = "/usr/bin";
  const bool opened = FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> result) {
    if (!result.has_value() || m_platform == nullptr) {
      return;
    }
    const auto added = m_platform->andromedaRuntime().commandResult("trusted-binary-add", result->native());
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kErrors{{
        {"not-executable", "settings.privacy.screen-input-not-app"},
        {"script", "settings.privacy.screen-input-script"},
        {"runs-other-code", "settings.privacy.screen-input-runner"},
        {"too-many", "settings.privacy.screen-input-too-many"},
        {"too-long", "settings.privacy.screen-input-not-app"},
    }};
    m_privacyStatus.clear();
    if (!added.has_value()) {
      const auto it = std::ranges::find(kErrors, std::string_view(added.error()), &std::pair<std::string_view, std::string_view>::first);
      m_privacyStatus = i18n::tr(it != kErrors.end() ? it->second : "settings.privacy.change-failed");
    }
    loadTrustedApps();
    requestContentRebuild();
  });
  if (!opened) {
    m_privacyStatus = i18n::tr("settings.privacy.change-failed");
    requestContentRebuild();
  }
}

void SettingsWindow::removeTrustedApp(const std::string& path) {
  const bool ok = m_platform != nullptr
                  && m_platform->andromedaRuntime().commandResult("trusted-binary-remove", path).has_value();
  m_privacyStatus = ok ? std::string{} : i18n::tr("settings.privacy.change-failed");
  loadTrustedApps();
  requestContentRebuild();
}
