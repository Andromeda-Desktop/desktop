#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class AccountsService;
class ConfigService;
struct Config;

namespace shell {

  enum class AvatarApplyError {
    None,
    InvalidPath,
    LoadFailed,
    WriteFailed,
    AccountsFailed,
    ConfigFailed,
  };

  struct AvatarApplyResult {
    AvatarApplyError error = AvatarApplyError::None;

    [[nodiscard]] bool success() const noexcept { return error == AvatarApplyError::None; }
  };

  [[nodiscard]] std::string resolvedAvatarPath(const AccountsService* accounts, const Config& config);

  // Prefer shell.avatar_path for rendering when set so UI updates immediately after
  // applyAvatarPath; AccountsService IconFile keeps a stable path and updates async.
  [[nodiscard]] std::string avatarDisplayPath(const AccountsService* accounts, const Config& config);

  // Persists shell.avatar_path and updates AccountsService when it is connected.
  // Large images are center-cropped and downscaled before AccountsService accepts them. The file check, decode and
  // cache write run on BlockingPool; `done` runs on the main loop while `alive` lives.
  void applyAvatarPath(
      AccountsService* accounts, ConfigService* config, std::string_view path, std::weak_ptr<const void> alive,
      std::function<void(AvatarApplyResult)> done
  );

  [[nodiscard]] const char* avatarApplyErrorTranslationKey(AvatarApplyError error) noexcept;

} // namespace shell
