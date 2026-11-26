#pragma once

#include "util/private_state_file.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
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
  // Every change runs in request order and answers on the event loop; durable changes are saved on the store's
  // saver thread first, and memory follows only a successful save.
  class InputCapturePermissions {
  public:
    // Returns whether the grant reached its client. One that did not is undone: an issued grant is removed, a durable
    // grant restored to persist mode 2 gets its old token back. The return value of a nullopt call is ignored.
    using GrantDone = std::function<bool(std::optional<InputCaptureGrant>)>;
    using Done = std::function<void(bool)>;
    using BinaryDone = std::function<void(std::expected<std::string, std::string>)>;

    InputCapturePermissions(wl_event_loop* loop, std::filesystem::path path);
    static std::filesystem::path defaultPath();

    // A token restores only a grant of the same kind and application, for a subset of its capabilities.
    void issue(
        std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer,
        InputPermissionKind kind, GrantDone done
    );
    void restore(
        std::string_view token, std::string_view application, uint32_t capabilities, uint32_t persistMode,
        uint64_t peer, InputPermissionKind kind, GrantDone done
    );
    [[nodiscard]] std::vector<InputCapturePermission> list();
    // Binaries the user granted screen and input access (Settings > Privacy): canonical absolute paths.
    [[nodiscard]] std::vector<std::string> binaries();
    // Stores the canonical path of an existing ELF executable. Errors: not-absolute, not-executable, script,
    // runs-other-code (an interpreter or loader), too-long, too-many (64), store-failed.
    void addBinary(std::string_view path, BinaryDone done);
    void removeBinary(std::string_view path, Done done);
    void revoke(std::string_view id, Done done);
    void expirePeer(uint64_t peer);
    // An ephemeral grant (persist mode 1) of `peer` is in memory.
    [[nodiscard]] bool hasPeer(uint64_t peer) const { return std::ranges::contains(m_ephemeral, peer, &Record::peer); }

    struct Record {
      InputCapturePermission permission;
      std::string token;
      uint64_t peer = 0;
    };
    // The file's content: durable grants in use order, and binaries.
    struct Stored {
      std::vector<Record> grants;
      std::vector<std::string> binaries;
    };

  private:
    bool load();
    void commit(Stored stored);
    // Gives the file's record restored to token `used` its token from `original` again.
    void putBack(Record original, std::string used);
    // A durable save that ran with a stale count of ephemeral grants: drop the oldest ephemeral ones over the cap.
    void trimEphemeral();

    // Mirror of the file's grants (persist mode 2), in use order: a restore moves its record last.
    std::vector<Record> m_durable;
    // Grants for one app instance (persist mode 1); never on disk.
    std::vector<Record> m_ephemeral;
    std::vector<std::string> m_binaries;
    // lstat (inode, size, mtime) of the store when binaries() last parsed it.
    std::array<int64_t, 4> m_stamp{};
    bool m_loaded = false;
    private_state::Saver m_saver;
  };

} // namespace andromeda
