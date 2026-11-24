#include "system/desktop_entry_launch.h"
#include "protocols/bus_names.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/shared_worker.h"
#include "dbus/name_owner_watch.h"
#include "core/process/process.h"
#include "system/desktop_entry.h"
#include "system/terminal_launch.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <gio/gio.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sdbus-c++/sdbus-c++.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("desktop_entry_launch");

  std::string decodeDesktopStringValue(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '\\' && i + 1 < value.size()) {
        const char next = value[i + 1];
        if (next == '\\' || next == 's' || next == 'n' || next == 't' || next == 'r') {
          // https://specifications.freedesktop.org/desktop-entry/latest/value-types.html
          // https://specifications.freedesktop.org/desktop-entry-spec/latest/exec-variables.html
          result += (next == 's') ? ' ' : (next == 'n') ? '\n' : (next == 't') ? '\t' : (next == 'r') ? '\r' : '\\';
          ++i;
          continue;
        }
        // Any other \X: remove the backslash, keep X
        result += next;
        ++i;
        continue;
      }
      result += value[i];
    }
    return result;
  }

  std::string stripFieldCodes(std::string_view exec) {
    std::string result;
    result.reserve(exec.size());
    for (std::size_t i = 0; i < exec.size(); ++i) {
      if (exec[i] == '%' && i + 1 < exec.size()) {
        const char next = exec[i + 1];
        if (next == 'f'
            || next == 'F'
            || next == 'u'
            || next == 'U'
            || next == 'd'
            || next == 'D'
            || next == 'n'
            || next == 'N'
            || next == 'i'
            || next == 'c'
            || next == 'k') {
          ++i;
          if (i + 1 < exec.size() && exec[i + 1] == ' ') {
            ++i;
          }
          continue;
        }
        if (next == '%') {
          result += '%';
          ++i;
          continue;
        }
      }
      result += exec[i];
    }

    while (!result.empty() && result.back() == ' ') {
      result.pop_back();
    }

    // Strip orphaned Flatpak @@ file-forwarding markers.
    for (std::size_t pos = 0;;) {
      pos = result.find("@@u", pos);
      if (pos == std::string::npos)
        break;
      std::size_t end = result.find("@@", pos + 3);
      if (end == std::string::npos)
        break;
      bool onlyWhitespace = true;
      for (std::size_t j = pos + 3; j < end; ++j) {
        if (result[j] != ' ') {
          onlyWhitespace = false;
          break;
        }
      }
      if (onlyWhitespace) {
        result.erase(pos, end + 2 - pos);
      } else {
        pos = end + 2;
      }
    }

    return result;
  }

  std::vector<std::string> tokenize(std::string_view cmd) {
    std::vector<std::string> args;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;

    for (std::size_t i = 0; i < cmd.size(); ++i) {
      const char c = cmd[i];
      if (c == '\\' && inDouble && i + 1 < cmd.size()) {
        const char next = cmd[i + 1];
        if (next == '"' || next == '`' || next == '$' || next == '\\') {
          current += next;
          ++i;
          continue;
        }
      }
      if (c == '\'' && !inDouble) {
        inSingle = !inSingle;
        continue;
      }
      if (c == '"' && !inSingle) {
        inDouble = !inDouble;
        continue;
      }
      if (c == ' ' && !inSingle && !inDouble) {
        if (!current.empty()) {
          args.push_back(std::move(current));
          current.clear();
        }
        continue;
      }
      current += c;
    }
    if (!current.empty()) {
      args.push_back(std::move(current));
    }
    return args;
  }

  bool isEnvAssignment(std::string_view arg) {
    const auto eq = arg.find('=');
    if (eq == std::string_view::npos || eq == 0 || std::isdigit(static_cast<unsigned char>(arg.front())) != 0) {
      return false;
    }
    return std::ranges::all_of(arg.substr(0, eq), [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; });
  }

  std::string expandExecutablePath(std::string_view binary) {
    if (binary.empty() || binary.front() != '~') {
      return std::string(binary);
    }
    return FileUtils::expandUserPath(std::string(binary)).string();
  }

  std::string appNameOrDefault(std::string_view appName) {
    return appName.empty() ? "desktop-entry" : std::string(appName);
  }

  std::string parseCustomCommand(const std::string& exec, const std::string& customCommand) {
    if (customCommand.empty()) {
      return exec;
    }
    constexpr std::string_view kPlaceholder = "$CMD";
    if (!customCommand.contains(kPlaceholder)) {
      kLog.warn("Custom command does not contain '$CMD': '{}'", customCommand);
    }
    std::string command = customCommand;
    for (std::size_t pos = 0; (pos = command.find(kPlaceholder, pos)) != std::string::npos; pos += exec.length()) {
      command.replace(pos, kPlaceholder.size(), exec);
    }
    return command;
  }

  // Resolves whether this launch really goes through systemd, warning once when the option is set
  // but the shell is not itself a systemd user unit: apps would then be moved out of the login
  // session the shell lives in.
  bool effectiveRunAsSystemdService(const desktop_entry_launch::LaunchOptions& options) {
    // A custom command is the user's own launch wrapper; the systemd launch is only the default.
    if (!options.runAsSystemdService || !options.customCommand.empty()) {
      return false;
    }
    if (process::runningUnderSystemdUserManager()) {
      return true;
    }
    static std::once_flag warnOnce;
    std::call_once(warnOnce, [] {
      kLog.warn(
          "launch_apps_as_systemd_services is enabled but Noctalia is not running under the systemd user "
          "manager (no uwsm or systemd user service); launching apps directly"
      );
    });
    return false;
  }

  std::string dbusObjectPathForAppId(std::string_view appId) {
    std::string path;
    path.reserve(appId.size() + 1);
    path.push_back('/');
    for (const char c : appId) {
      path.push_back(c == '.' ? '/' : c);
    }
    return path;
  }

  std::map<std::string, sdbus::Variant> dbusPlatformData(const std::string& activationToken) {
    std::map<std::string, sdbus::Variant> platformData;
    if (!activationToken.empty()) {
      platformData.emplace("desktop-startup-id", sdbus::Variant{activationToken});
      platformData.emplace("activation-token", sdbus::Variant{activationToken});
    }
    return platformData;
  }

  bool dbusCandidate(const desktop_entry_launch::LaunchOptions& options) {
    return options.dbusActivatable && !options.dbusAppId.empty() && options.customCommand.empty();
  }

  // The app is not there to take the call: the launch falls back to Exec. A timeout is not one of them: the app runs
  // but is slow, and a second Exec would open a second instance.
  bool targetMissing(const sdbus::Error& error) {
    const std::string name = error.getName();
    return name == "org.freedesktop.DBus.Error.ServiceUnknown" || name == "org.freedesktop.DBus.Error.NameHasNoOwner"
        || name == "org.freedesktop.DBus.Error.UnknownObject" || name == "org.freedesktop.DBus.Error.UnknownInterface"
        || name == "org.freedesktop.DBus.Error.UnknownMethod";
  }

  // One session-bus connection for warm launches, made on the SharedWorker at the first launch. Its event loop
  // thread keeps the set of owned well-known names (ListNames once, then NameOwnerChanged), so a launch needs no
  // connect and no NameHasOwner round trip.
  class LaunchBus {
  public:
    // Null when the session bus is unreachable. SharedWorker thread only.
    static LaunchBus* instance() {
      static const std::unique_ptr<LaunchBus> bus = create();
      return bus.get();
    }

    bool owned(const std::string& name) {
      const std::lock_guard lock(m_mutex);
      return m_owned.contains(name);
    }

    // SharedWorker thread only. `missing` runs on the bus thread when the app did not take the launch.
    void activate(const desktop_entry_launch::LaunchOptions& options, std::function<void()> missing) {
      constexpr auto kIface = "org.freedesktop.Application";
      // As the forward in libdesk-app: under load a running app can take seconds to open its window.
      constexpr auto kActivateTimeout = std::chrono::seconds(5);
      auto& proxy = m_apps[options.dbusAppId];
      if (proxy == nullptr) {
        proxy = sdbus::createProxy(
            *m_connection, sdbus::ServiceName{options.dbusAppId},
            sdbus::ObjectPath{dbusObjectPathForAppId(options.dbusAppId)}
        );
      }
      auto onReply = [appId = options.dbusAppId, missing = std::move(missing)](std::optional<sdbus::Error> error) {
        if (!error) {
          return;
        }
        if (targetMissing(*error)) {
          kLog.debug("dbus activation of '{}' found no app ({}); falling back to Exec", appId, error->getName());
          missing();
        } else {
          kLog.warn("dbus activation of '{}' did not finish: {}", appId, error->what());
        }
      };
      const auto platformData = dbusPlatformData(options.activationToken);
      if (options.desktopActionId.empty()) {
        proxy->callMethodAsync("Activate")
            .onInterface(kIface)
            .withTimeout(kActivateTimeout)
            .withArguments(platformData)
            .uponReplyInvoke(std::move(onReply));
      } else {
        proxy->callMethodAsync("ActivateAction")
            .onInterface(kIface)
            .withTimeout(kActivateTimeout)
            .withArguments(options.desktopActionId, std::vector<sdbus::Variant>{}, platformData)
            .uponReplyInvoke(std::move(onReply));
      }
    }

  private:
    static std::unique_ptr<LaunchBus> create() {
      try {
        auto bus = std::unique_ptr<LaunchBus>(new LaunchBus());
        bus->start();
        return bus;
      } catch (const sdbus::Error& error) {
        kLog.warn("no session bus for D-Bus launches: {}", error.what());
        return nullptr;
      }
    }

    LaunchBus() = default;

    void start() {
      m_connection = sdbus::createSessionBusConnection();
      m_dbus = sdbus::createProxy(
          *m_connection, sdbus::ServiceName{busname::kDaemon}, sdbus::ObjectPath{busname::kDaemonPath}
      );
      m_ownerWatch = NameOwnerWatch::all(
          *m_connection,
          [this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
            if (name.starts_with(':')) {
              return;
            }
            const std::lock_guard lock(m_mutex);
            if (newOwner.empty()) {
              m_owned.erase(name);
            } else {
              m_owned.insert(name);
            }
          }
      );
      m_connection->enterEventLoopAsync();
      std::vector<std::string> names;
      m_dbus->callMethod("ListNames").onInterface(busname::kDaemon).storeResultsTo(names);
      const std::lock_guard lock(m_mutex);
      for (auto& name : names) {
        if (!name.starts_with(':')) {
          m_owned.insert(std::move(name));
        }
      }
    }

    std::unique_ptr<sdbus::IConnection> m_connection;
    std::unique_ptr<sdbus::IProxy> m_dbus;
    std::map<std::string, std::unique_ptr<sdbus::IProxy>> m_apps;
    std::mutex m_mutex;
    std::set<std::string> m_owned;
    NameOwnerWatch m_ownerWatch;
  };

  // A running D-Bus-activatable app takes the launch over the bus, with no new process; otherwise `exec` runs on the
  // main loop. True means the launch started.
  bool activateOrExec(const desktop_entry_launch::LaunchOptions& options, std::function<bool()> exec) {
    if (!dbusCandidate(options)) {
      return exec();
    }
    auto execLater = [exec, appId = options.dbusAppId] {
      DeferredCall::callLater([exec, appId] {
        // The caller already saw "started"; a failed fallback can only be logged.
        if (!exec()) {
          kLog.warn("launch of '{}' failed after D-Bus activation was not possible", appId);
        }
      });
    };
    SharedWorker::instance().post([options, execLater = std::move(execLater)] {
      LaunchBus* bus = LaunchBus::instance();
      if (bus == nullptr || !bus->owned(options.dbusAppId)) {
        execLater();
        return;
      }
      bus->activate(options, execLater);
    });
    return true;
  }

  // The double fork copies the shell's page tables twice and waits for the exec (several ms for a large shell), so it
  // runs on the SharedWorker; the caller sees "started" and a failure is logged.
  bool launchPrepared(
      const std::vector<std::string>& args, std::string_view appName, std::string_view workingDir,
      const desktop_entry_launch::LaunchOptions& options, bool runAsSystemdService
  ) {
    SharedWorker::instance().post(
        [args, name = appNameOrDefault(appName), workingDir = std::string(workingDir),
         token = options.activationToken, runAsSystemdService] {
          const bool started = runAsSystemdService ? process::runAsyncAsSystemdService(args, name, token, workingDir)
                                                   : process::runAsync(args, token, workingDir);
          if (!started) {
            kLog.warn("failed to start '{}'", args.front());
          }
        }
    );
    return true;
  }

} // namespace

namespace desktop_entry_launch {

  std::optional<PreparedCommand> prepareCommand(std::string_view exec, bool terminal, const PrepareOptions& options) {
    const std::string decodedExec = decodeDesktopStringValue(exec);
    const std::string cleanExec = stripFieldCodes(decodedExec);
    std::vector<std::string> args;
    if (terminal) {
      auto prepared = terminal_launch::prepareCommand(
          cleanExec,
          terminal_launch::Options{
              .terminalCandidates = options.terminalCandidates,
              .useSystemTerminalDiscovery = options.useSystemTerminalDiscovery,
          }
      );
      if (!prepared.has_value()) {
        return std::nullopt;
      }
      args = std::move(*prepared);
    } else {
      args = tokenize(cleanExec);
    }

    const auto program = std::ranges::find_if_not(args, isEnvAssignment);
    if (program == args.end()) {
      return std::nullopt;
    }
    if (program->contains('/')) {
      *program = expandExecutablePath(*program);
    }
    if (program != args.begin()) {
      args.insert(args.begin(), "env");
    }
    return PreparedCommand{std::move(args)};
  }

  bool launchDefaultForMimeType(std::string_view mimeType) {
    const std::string mimeTypeText(mimeType);
    GAppInfo* appInfo = g_app_info_get_default_for_type(mimeTypeText.c_str(), FALSE);
    if (appInfo == nullptr) {
      kLog.warn("no default application set for MIME type '{}'", mimeType);
      return false;
    }

    GError* error = nullptr;
    const bool launched = g_app_info_launch(appInfo, nullptr, nullptr, &error) == TRUE;
    if (!launched) {
      if (error != nullptr) {
        kLog.warn("failed to launch default application for MIME type '{}': {}", mimeType, error->message);
      } else {
        kLog.warn("failed to launch default application for MIME type '{}'", mimeType);
      }
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    g_object_unref(appInfo);
    return launched;
  }

  bool launchEntry(const DesktopEntry& entry, const LaunchOptions& options) {
    LaunchOptions resolved = options;
    if (resolved.dbusAppId.empty()) {
      resolved.dbusAppId = entry.id;
    }
    if (!resolved.dbusActivatable) {
      resolved.dbusActivatable = entry.dbusActivatable;
    }
    resolved.desktopActionId.clear();

    return activateOrExec(resolved, [entry, resolved] {
      const bool runAsSystemdService = effectiveRunAsSystemdService(resolved);
      const std::string command = parseCustomCommand(entry.exec, resolved.customCommand);
      auto prepared = prepareCommand(command, entry.terminal);

      if (!prepared.has_value()) {
        kLog.warn("Failed to prepare launch command for desktop entry '{}'", entry.id.empty() ? entry.name : entry.id);
        return false;
      }

      const std::string appName = !entry.id.empty() ? entry.id : appNameOrDefault(entry.name);
      return launchPrepared(prepared->args, appName, entry.workingDir, resolved, runAsSystemdService);
    });
  }

  bool launchAction(
      const DesktopAction& action, std::string_view appName, std::string_view workingDir, bool terminal,
      const LaunchOptions& options
  ) {
    LaunchOptions resolved = options;
    if (resolved.dbusAppId.empty()) {
      resolved.dbusAppId = std::string(appName);
    }
    resolved.desktopActionId = action.id;

    return activateOrExec(
        resolved, [action, appName = std::string(appName), workingDir = std::string(workingDir), terminal, resolved] {
          const bool runAsSystemdService = effectiveRunAsSystemdService(resolved);
          const std::string command = parseCustomCommand(action.exec, resolved.customCommand);
          auto prepared = prepareCommand(command, terminal);
          if (!prepared.has_value()) {
            kLog.warn(
                "Failed to prepare launch command for desktop action '{}'",
                action.id.empty() ? action.name : action.id
            );
            return false;
          }
          return launchPrepared(prepared->args, appName, workingDir, resolved, runAsSystemdService);
        }
    );
  }

} // namespace desktop_entry_launch
