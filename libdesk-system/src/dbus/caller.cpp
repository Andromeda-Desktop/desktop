#include "dbus/caller.h"

#include "core/log.h"
#include "protocols/bus_names.h"
#include "protocols/ipc_channel.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <unistd.h>
#include <utility>

namespace {
  constexpr Logger kLog("bus-caller");
  constexpr std::size_t kMaxFlatpakInfo = 64 * 1024;

  struct Fd {
    int fd = -1;
    explicit Fd(int value) : fd(value) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  };

  // The value of `key` in `[group]` of a GLib key file.
  std::optional<std::string> keyFileValue(std::string_view text, std::string_view group, std::string_view key) {
    bool inGroup = false;
    while (!text.empty()) {
      const auto end = text.find('\n');
      std::string_view line = text.substr(0, end);
      text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
      if (line.starts_with('[')) {
        inGroup = line.size() == group.size() + 2 && line.substr(1, group.size()) == group && line.back() == ']';
      } else if (inGroup && line.starts_with(key) && line.substr(key.size()).starts_with('=')) {
        return std::string{line.substr(key.size() + 1)};
      }
    }
    return std::nullopt;
  }

  // Host or the Flatpak app id of `pid`, read through its root; nullopt when the root or the file cannot be read.
  std::optional<BusCaller> sandboxOf(std::uint32_t pid) {
    const Fd root{::open(("/proc/" + std::to_string(pid) + "/root").c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
    if (root.fd < 0) {
      kLog.debug("pid {}: root not readable: {}", pid, std::strerror(errno));
      return std::nullopt;
    }
    const Fd info{::openat(root.fd, ".flatpak-info", O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    if (info.fd < 0) {
      if (errno == ENOENT) {
        return BusCaller{};
      }
      kLog.debug("pid {}: .flatpak-info not readable: {}", pid, std::strerror(errno));
      return std::nullopt;
    }
    std::string text(kMaxFlatpakInfo, '\0');
    const ssize_t got = ::read(info.fd, text.data(), text.size());
    if (got <= 0) {
      return std::nullopt;
    }
    text.resize(static_cast<std::size_t>(got));
    auto appId = keyFileValue(text, "Application", "name");
    if (!appId.has_value() || appId->empty()) {
      return std::nullopt;
    }
    return BusCaller{.kind = BusCaller::Kind::Sandboxed, .appId = std::move(*appId)};
  }

  std::unique_ptr<sdbus::IProxy> daemonProxy(sdbus::IConnection& bus) {
    return sdbus::createProxy(bus, sdbus::ServiceName{busname::kDaemon}, sdbus::ObjectPath{busname::kDaemonPath});
  }
} // namespace

std::optional<BusCaller> busCaller(sdbus::IConnection& bus, std::string_view sender) {
  if (sender.empty()) {
    return std::nullopt;
  }
  std::map<std::string, sdbus::Variant> credentials;
  try {
    daemonProxy(bus)
        ->callMethod("GetConnectionCredentials")
        .onInterface(busname::kDaemon)
        .withArguments(std::string{sender})
        .storeResultsTo(credentials);
  } catch (const sdbus::Error& e) {
    kLog.debug("GetConnectionCredentials({}) failed: {}", sender, e.what());
    return std::nullopt;
  }
  const auto uid = credentials.find("UnixUserID");
  const auto pid = credentials.find("ProcessID");
  const auto pidfd = credentials.find("ProcessFD");
  if (uid == credentials.end() || pid == credentials.end() || pidfd == credentials.end()) {
    kLog.warn("bus gives no pidfd for {}: caller refused", sender);
    return std::nullopt;
  }
  if (uid->second.get<std::uint32_t>() != ::getuid()) {
    return std::nullopt;
  }
  const auto pidfdHandle = pidfd->second.get<sdbus::UnixFd>();
  auto caller = sandboxOf(pid->second.get<std::uint32_t>());
  // The pid names the caller only while its pidfd shows the process still runs: after an exit the pid can be reused.
  if (!caller.has_value() || !ipc::running(pidfdHandle.get())) {
    return std::nullopt;
  }
  return caller;
}

bool isNameOwner(sdbus::IConnection& bus, std::string_view name, std::string_view sender) {
  if (sender.empty()) {
    return false;
  }
  try {
    std::string owner;
    daemonProxy(bus)
        ->callMethod("GetNameOwner")
        .onInterface(busname::kDaemon)
        .withArguments(std::string{name})
        .storeResultsTo(owner);
    return owner == sender;
  } catch (const sdbus::Error& e) {
    kLog.debug("GetNameOwner({}) failed: {}", name, e.what());
    return false;
  }
}

namespace {
  void requireOwner(sdbus::IConnection& bus, std::string_view name, const std::string& refusal) {
    const char* sender = bus.getCurrentlyProcessedMessage().getSender();
    if (!isNameOwner(bus, name, std::string{sender != nullptr ? sender : ""})) {
      throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}, refusal);
    }
  }
} // namespace

void requireNameOwner(sdbus::IConnection& bus, std::string_view name) {
  requireOwner(bus, name, "Only the owner of " + std::string{name} + " may call");
}

void requirePortalFrontend(sdbus::IConnection& bus) {
  requireOwner(bus, busname::kPortalFrontend, "Only the portal frontend may call");
}
