#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace andromeda {

  // Capture sends local input to the application; Emulate lets it inject input (RemoteDesktop).
  enum class InputPermissionKind : uint8_t { Capture, Emulate };

  struct InputCapturePermission {
    std::string id;
    std::string application;
    uint32_t capabilities = 0;
    uint32_t persistMode = 0;
    InputPermissionKind kind = InputPermissionKind::Capture;
  };

  struct InputCaptureGrant {
    std::string id;
    std::string token;
    uint32_t persistMode = 0;
  };

  // The token is a bearer credential. Display names and application identifiers
  // alone never authorize capture. Only durable grants are written to disk.
  class InputCapturePermissions {
  public:
    explicit InputCapturePermissions(std::filesystem::path path);
    static std::filesystem::path defaultPath();

    // A token restores only a grant of the same kind and application, for a subset of its capabilities.
    [[nodiscard]] std::optional<InputCaptureGrant> issue(
        std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer,
        InputPermissionKind kind = InputPermissionKind::Capture
    );
    [[nodiscard]] std::optional<InputCaptureGrant> restore(
        std::string_view token, std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer,
        InputPermissionKind kind = InputPermissionKind::Capture
    );
    [[nodiscard]] std::vector<InputCapturePermission> list();
    // Binaries the user granted screen and input access (Settings > Privacy): canonical absolute paths.
    [[nodiscard]] std::vector<std::string> binaries();
    // Stores the canonical path of an existing ELF executable. Errors: not-absolute, not-executable, script,
    // runs-other-code (an interpreter or loader), too-long, too-many (64), store-failed.
    [[nodiscard]] std::expected<std::string, std::string> addBinary(std::string_view path);
    [[nodiscard]] bool removeBinary(std::string_view path);
    [[nodiscard]] bool revoke(std::string_view id);
    void expirePeer(uint64_t peer);

  private:
    struct Record {
      InputCapturePermission permission;
      std::string token;
      uint64_t peer = 0;
    };

    bool load();
    bool save(const std::vector<Record>& records, const std::vector<std::string>& binaries) const;

    std::filesystem::path m_path;
    std::vector<Record> m_records;
    std::vector<std::string> m_binaries;
    // lstat (inode, size, mtime) of the store when binaries() last parsed it.
    std::array<int64_t, 4> m_stamp{};
    bool m_loaded = false;
  };

} // namespace andromeda
