#include "holder_client.h"
#include "core/sys/sys.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace term::holder {

namespace {

constexpr std::size_t kMaxFrame = 64u << 20;
constexpr int kHandshakeTimeoutMs = 2000;

enum Tag : std::uint8_t {
  kSpawn = 1,
  kAttach = 2,
  kSnapshot = 3,
  kClose = 4,
  kList = 5,
  kWatch = 6,
  kPutWorkspace = 7,
  kCloseWorkspace = 8,
  kSetWorkspace = 9,
  kDetach = 20,
  kHello = 21,
  kVersion = 22,
  kSpawned = 10,
  kSpawnFailed = 11,
  kAttached = 12,
  kAttachFailed = 13,
  kOutput = 14,
  kWantSnapshot = 15,
  kExited = 16,
  kOrphans = 17,
  kWorkspacePut = 18,
  kWorkspaces = 19,
};

class Writer {
public:
  explicit Writer(std::uint8_t tag) : m_bytes{0, 0, 0, 0, tag} {}
  Writer& u16(std::uint16_t v) { return put(v, 2); }
  Writer& u32(std::uint32_t v) { return put(v, 4); }
  Writer& u64(std::uint64_t v) { return put(v, 8); }
  Writer& bytes(std::span<const std::uint8_t> v) {
    u32(static_cast<std::uint32_t>(v.size()));
    m_bytes.insert(m_bytes.end(), v.begin(), v.end());
    return *this;
  }
  Writer& str(const std::string& s) { return bytes({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}); }
  Writer& strs(const std::vector<std::string>& v) {
    u32(static_cast<std::uint32_t>(v.size()));
    for (const auto& s : v) str(s);
    return *this;
  }
  std::vector<std::uint8_t> finish() {
    const auto len = static_cast<std::uint32_t>(m_bytes.size() - 4);
    for (int i = 0; i < 4; ++i) m_bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(len >> (8 * i));
    return std::move(m_bytes);
  }

private:
  Writer& put(std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) m_bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    return *this;
  }
  std::vector<std::uint8_t> m_bytes;
};

class Reader {
public:
  explicit Reader(std::span<const std::uint8_t> s) : m_s(s) {}
  bool ok() const { return m_ok; }
  std::uint8_t u8() { return static_cast<std::uint8_t>(get(1)); }
  std::uint32_t u32() { return static_cast<std::uint32_t>(get(4)); }
  std::uint64_t u64() { return get(8); }
  std::vector<std::uint8_t> bytes() {
    const std::size_t n = u32();
    if (!m_ok || m_s.size() < n) {
      m_ok = false;
      return {};
    }
    std::vector<std::uint8_t> v(m_s.begin(), m_s.begin() + static_cast<std::ptrdiff_t>(n));
    m_s = m_s.subspan(n);
    return v;
  }
  std::string str() {
    auto b = bytes();
    return {b.begin(), b.end()};
  }

private:
  std::uint64_t get(int n) {
    if (!m_ok || m_s.size() < static_cast<std::size_t>(n)) {
      m_ok = false;
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(m_s[static_cast<std::size_t>(i)]) << (8 * i);
    m_s = m_s.subspan(static_cast<std::size_t>(n));
    return v;
  }
  std::span<const std::uint8_t> m_s;
  bool m_ok = true;
};

} // namespace

std::vector<std::uint8_t> encodeSpawn(std::uint32_t req, WinSize size, const std::string& cwd,
                                      const std::vector<std::string>& argv, const std::vector<std::string>& env) {
  return Writer(kSpawn)
      .u32(req)
      .u16(size.cols)
      .u16(size.rows)
      .u16(size.xpixel)
      .u16(size.ypixel)
      .str(cwd)
      .strs(argv)
      .strs(env)
      .finish();
}

std::vector<std::uint8_t> encodeAttach(std::uint64_t id) { return Writer(kAttach).u64(id).finish(); }

std::vector<std::uint8_t> encodeSnapshot(std::uint64_t id, std::uint64_t offset, std::span<const std::uint8_t> blob) {
  return Writer(kSnapshot).u64(id).u64(offset).bytes(blob).finish();
}

std::vector<std::uint8_t> encodeClose(std::uint64_t id) { return Writer(kClose).u64(id).finish(); }

std::vector<std::uint8_t> encodeList() { return Writer(kList).finish(); }

std::vector<std::uint8_t> encodeWatch() { return Writer(kWatch).finish(); }

std::vector<std::uint8_t> encodeDetach(std::uint64_t id) { return Writer(kDetach).u64(id).finish(); }

std::vector<std::uint8_t> encodePutWorkspace(std::uint32_t req, std::uint64_t id, const std::string& name,
                                             const std::string& root, const std::string& symbol) {
  return Writer(kPutWorkspace).u32(req).u64(id).str(name).str(root).str(symbol).finish();
}

std::vector<std::uint8_t> encodeCloseWorkspace(std::uint64_t id) { return Writer(kCloseWorkspace).u64(id).finish(); }

std::vector<std::uint8_t> encodeSetWorkspace(std::uint64_t session, std::uint64_t workspace) {
  return Writer(kSetWorkspace).u64(session).u64(workspace).finish();
}

std::optional<std::size_t> decode(std::span<const std::uint8_t> buf, std::optional<Message>& out) {
  out.reset();
  if (buf.size() < 4) return 0;
  const std::size_t len = std::size_t{buf[0]} | std::size_t{buf[1]} << 8 | std::size_t{buf[2]} << 16 | std::size_t{buf[3]} << 24;
  if (len == 0 || len > kMaxFrame) return std::nullopt;
  if (buf.size() < 4 + len) return 0;
  const std::uint8_t tag = buf[4];
  Reader r(buf.subspan(5, len - 1));
  switch (tag) {
  case kSpawned: {
    auto req = r.u32();
    out = Spawned{req, r.u64()};
    break;
  }
  case kSpawnFailed: {
    auto req = r.u32();
    out = SpawnFailed{req, r.str()};
    break;
  }
  case kAttached: {
    Attached a;
    a.id = r.u64();
    a.offset = r.u64();
    a.snapshot = r.bytes();
    a.tail = r.bytes();
    out = std::move(a);
    break;
  }
  case kAttachFailed: out = AttachFailed{r.u64()}; break;
  case kOutput: {
    Output o;
    o.id = r.u64();
    o.offset = r.u64();
    o.bytes = r.bytes();
    out = std::move(o);
    break;
  }
  case kWantSnapshot: out = WantSnapshot{r.u64()}; break;
  case kExited: {
    auto id = r.u64();
    out = Exited{id, static_cast<int>(r.u32())};
    break;
  }
  case kVersion:
    out = Version{r.u32()};
    break;
  case kOrphans: {
    Orphans o;
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) o.ids.push_back(r.u64());
    out = std::move(o);
    break;
  }
  case kWorkspacePut: {
    auto req = r.u32();
    out = WorkspacePut{req, r.u64()};
    break;
  }
  case kWorkspaces: {
    Workspaces w;
    w.seq = r.u64();
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
      WorkspaceInfo ws;
      ws.id = r.u64();
      ws.name = r.str();
      ws.root = r.str();
      ws.symbol = r.str();
      w.workspaces.push_back(std::move(ws));
    }
    const std::uint32_t m = r.u32();
    for (std::uint32_t i = 0; i < m && r.ok(); ++i) {
      SessionInfo si;
      si.id = r.u64();
      si.workspace = r.u64();
      si.attached = r.u8() != 0;
      si.program = r.str();
      si.cwd = r.str();
      const std::uint8_t kind = r.u8();
      si.exitCode = static_cast<int>(r.u32());
      if (kind > 2) return std::nullopt;
      si.command = static_cast<CommandState>(kind);
      w.sessions.push_back(std::move(si));
    }
    out = std::move(w);
    break;
  }
  default: return std::nullopt;
  }
  if (!r.ok()) return std::nullopt;
  return 4 + len;
}

std::string socketPath() {
  if (const char* p = std::getenv("ANDROMEDA_PTYD_SOCKET"); p != nullptr && *p != '\0') return p;
  const std::string run = sys::userRuntimeDir();
  if (run.empty()) return {};
  return run + "/andromeda/ptyd-1.sock";
}

int connectSocket() {
  const std::string path = socketPath();
  sockaddr_un addr{};
  if (path.empty() || path.size() >= sizeof addr.sun_path) return -1;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  const int fd = sys::socketCloexec(AF_UNIX, SOCK_STREAM);
  if (fd < 0) return -1;
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return -1;
  }
  timeval tv{kHandshakeTimeoutMs / 1000, (kHandshakeTimeoutMs % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

bool sendAll(int fd, std::span<const std::uint8_t> bytes) {
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = sys::sendNoSignal(fd, bytes.data() + done, bytes.size() - done, 0);
    if (n > 0) {
      done += static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

long receive(int fd, std::vector<std::uint8_t>& buf, std::vector<int>& fds, bool nonblocking) {
  std::uint8_t chunk[64 * 1024];
  iovec iov{chunk, sizeof chunk};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof control;
  ssize_t n;
  do {
    n = sys::recvmsgCloexec(fd, &msg, nonblocking ? MSG_DONTWAIT : 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0) return -1;
  for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int received;
      std::memcpy(&received, CMSG_DATA(c), sizeof received);
      fds.push_back(received);
    }
  }
  buf.insert(buf.end(), chunk, chunk + n);
  return n;
}

std::optional<Message> receiveOne(int sock, std::vector<std::uint8_t>& buf, int& fd) {
  std::vector<int> fds;
  std::optional<Message> msg;
  for (;;) {
    const auto used = decode(buf, msg);
    if (!used) break;
    if (*used > 0) {
      buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*used));
      break;
    }
    if (receive(sock, buf, fds, false) <= 0) break;
  }
  fd = -1;
  for (std::size_t i = 0; i < fds.size(); ++i) {
    const bool carries = msg && (std::holds_alternative<Spawned>(*msg) || std::holds_alternative<Attached>(*msg));
    if (i == 0 && carries) {
      fd = fds[i];
    } else {
      ::close(fds[i]);
    }
  }
  return msg;
}

std::vector<std::uint64_t> listOrphans() {
  const int sock = connectSocket();
  if (sock < 0) return {};
  std::vector<std::uint64_t> ids;
  std::vector<std::uint8_t> buf;
  int fd = -1;
  if (sendAll(sock, encodeList())) {
    if (auto msg = receiveOne(sock, buf, fd); msg && std::holds_alternative<Orphans>(*msg)) {
      ids = std::get<Orphans>(*msg).ids;
    }
  }
  ::close(sock);
  return ids;
}

std::uint32_t holderVersion() {
  const int sock = connectSocket();
  if (sock < 0) return 0;
  std::uint32_t version = 1;
  std::vector<std::uint8_t> buf;
  int fd = -1;
  if (sendAll(sock, Writer(kHello).finish())) {
    if (auto msg = receiveOne(sock, buf, fd); msg && std::holds_alternative<Version>(*msg)) {
      version = std::get<Version>(*msg).version;
    }
  }
  ::close(sock);
  return version;
}

} // namespace term::holder
