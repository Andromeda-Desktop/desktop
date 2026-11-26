#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

class CompositorPlatform;
class ConfigService;
class IpcService;
struct ShellGreeterSyncConfig;

namespace process {
  struct RunResult;
}

namespace greeter {

  namespace detail {

    enum class ApplyHelperProtocol : std::uint8_t {
      Unknown,
      Legacy,
      SecureSyncV1,
    };

    [[nodiscard]] ApplyHelperProtocol classifyApplyHelperProtocol(const process::RunResult& result);

  } // namespace detail

  enum class GreeterSyncLaunch {
    Failed,
    LaunchedConstrained,
    LaunchedLegacy,
    StagedOnly,
  };

  using SyncLaunched = std::function<void(GreeterSyncLaunch)>;
  using SyncCompletion = std::function<void(bool success)>;

  // True when noctalia-greeter and the privileged apply helper are installed.
  [[nodiscard]] bool appearanceSyncAvailable(const ShellGreeterSyncConfig& greeterSync) noexcept;

  // Copies the inputs now and stages them on SharedWorker; false (nothing posted) while another sync runs. The
  // staging job negotiates the helper protocol: current helpers receive an appearance-only payload through pkexec
  // and --sync; older helpers keep the administrator-authenticated positional mode and its session payload, and give
  // StagedOnly when no login session can host a Polkit prompt. onLaunched runs on the main loop after staging, with
  // the helper started when the result is Launched*. onComplete runs (on a process thread) after that helper exits.
  [[nodiscard]] bool syncAppearanceToGreeterAsync(
      const ConfigService& config, std::string_view resolvedThemeMode, SyncLaunched onLaunched = {},
      SyncCompletion onComplete = {}, const CompositorPlatform* platform = nullptr, bool logindOnSystemBus = false
  );

  // The lock clock's gradient ink (packed 0xRRGGBBAA, LockScreen::setClockGradient); synced as [lockscreen] clock_ink_*.
  void setLockClockGradient(std::uint32_t from, std::uint32_t to);

  // Runs on the main loop when a staged wallpaper's lock look (blur, dim) finished baking; the caller syncs again.
  void setLockWallpaperBakeReady(std::function<void()> callback);

  // No-op when appearanceSyncAvailable() is false (handler is not registered).
  void registerIpc(
      IpcService& ipc, const ConfigService& config, std::function<std::string_view()> resolvedThemeMode,
      const CompositorPlatform* platform, std::function<bool()> logindOnSystemBus = {}
  );

} // namespace greeter
