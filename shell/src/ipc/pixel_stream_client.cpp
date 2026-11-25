#include "ipc/pixel_stream_client.h"

#include "core/log.h"
#include "protocols/ipc_channel.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("pixel-stream");

  enum : std::uint32_t {
    kHello = 1,
    kRequest = 2,
    kNoDmabuf = 3,
    kPause = 4,
  };

  enum : std::uint32_t {
    kBuffer = 1,
    kFrame = 2,
    kDmabuf = 3,
  };

  constexpr std::size_t kBufferMessageSize = 16;
  constexpr std::size_t kDmabufMessageSize = 36;
  constexpr std::size_t kFrameMessageSize = 8;
  // Bounds one dispatch so a flooding companion cannot starve the main loop.
  constexpr int kMaxMessagesPerDispatch = 16;
  constexpr std::size_t kMaxFdsPerMessage = 4;

  std::uint32_t readLe32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
  }

  void writeLe32(std::uint8_t* bytes, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
  }

} // namespace

PixelStreamClient::~PixelStreamClient() { disconnect(); }

bool PixelStreamClient::setPath(std::string path) {
  if (path == m_path) {
    return false;
  }
  disconnect();
  m_path = std::move(path);
  m_retryAt = {};
  return true;
}

void PixelStreamClient::adopt(int fd) {
  disconnect();
  m_path.clear();
  m_fd = fd;
  if (send(kHello, kDmabufProtocolVersion) && send(kRequest, 0)) {
    m_requested = true;
  }
  if (m_paused) {
    (void)send(kPause, 1);
  }
}

std::optional<std::chrono::steady_clock::time_point> PixelStreamClient::retryAt() const {
  if (connected() || m_path.empty()) {
    return std::nullopt;
  }
  return m_retryAt;
}

void PixelStreamClient::tick(std::chrono::steady_clock::time_point now) {
  if (!connected() && !m_path.empty() && now >= m_retryAt) {
    connect();
  }
}

void PixelStreamClient::connect() {
  m_retryAt = std::chrono::steady_clock::now() + kRetryInterval;
  if (m_path.front() != '/' || m_path.contains('\0')) {
    // Retried like any other failure, so log at debug level to avoid repeating a warning.
    kLog.debug("'{}' is not a usable socket path", m_path);
    return;
  }
  ipc::Fd fd = ipc::connectTo(m_path, ipc::Deadline{}, SOCK_SEQPACKET | SOCK_NONBLOCK);
  if (!fd) {
    // The companion not running (yet) is the normal case; retry quietly.
    kLog.debug("connect '{}' failed: {}", m_path, std::strerror(errno));
    return;
  }
  auto peer = ipc::checkPeer(std::move(fd));
  if (!peer) {
    kLog.warn("refusing '{}': companion runs as another user", m_path);
    return;
  }
  m_fd = peer->conn.release();
  if (send(kHello, kProtocolVersion) && send(kRequest, 0)) {
    m_requested = true;
    kLog.debug("connected '{}'", m_path);
  }
}

void PixelStreamClient::disconnect() {
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
  for (Buffer& buffer : m_buffers) {
    unmap(buffer);
  }
  m_frameBuffer.reset();
  closeReleaseFence();
  m_requested = false;
  m_retryAt = std::chrono::steady_clock::now() + kRetryInterval;
}

void PixelStreamClient::fail(std::string_view reason) {
  kLog.warn("'{}': {}; reconnecting", m_path, reason);
  disconnect();
  notify();
}

void PixelStreamClient::reconnect() {
  if (m_path.empty()) {
    // An adopted socket cannot reconnect; a second HELLO asks for the current frame again.
    if (connected() && send(kHello, kDmabufProtocolVersion)) {
      m_frameBuffer.reset();
      closeReleaseFence();
      if (!m_requested && send(kRequest, 0)) {
        m_requested = true;
      }
    }
    return;
  }
  disconnect();
  m_retryAt = {};
}

bool PixelStreamClient::send(std::uint32_t type, std::uint32_t argument) {
  std::array<std::uint8_t, 8> message{};
  writeLe32(message.data(), type);
  writeLe32(message.data() + 4, argument);
  if (::send(m_fd, message.data(), message.size(), MSG_DONTWAIT | MSG_NOSIGNAL)
      != static_cast<ssize_t>(message.size())) {
    // One outstanding request never fills the socket buffer; any failure means the peer is gone.
    fail("write failed");
    return false;
  }
  return true;
}

void PixelStreamClient::dispatch(short revents) {
  if (!connected()) {
    return;
  }
  if ((revents & (POLLERR | POLLNVAL)) != 0) {
    fail("socket error");
    return;
  }
  if ((revents & (POLLIN | POLLHUP)) == 0) {
    return;
  }
  for (int i = 0; i < kMaxMessagesPerDispatch && readMessage(); ++i) {
  }
}

bool PixelStreamClient::readMessage() {
  std::array<std::uint8_t, kDmabufMessageSize + 1> payload{};
  ipc::Received received = ipc::recvFds(m_fd, std::as_writable_bytes(std::span(payload)), kMaxFdsPerMessage);
  switch (received.kind) {
  case ipc::Received::Kind::WouldBlock:
    return false;
  case ipc::Received::Kind::Failed:
    fail("read failed");
    return false;
  case ipc::Received::Kind::Closed:
    kLog.debug("'{}' closed by companion", m_path);
    disconnect();
    notify();
    return false;
  case ipc::Received::Kind::Truncated:
    fail("oversized message");
    return false;
  case ipc::Received::Kind::Data:
    break;
  }

  auto& fds = received.fds;
  const std::size_t size = received.size;
  const std::uint32_t type = size >= 4 ? readLe32(payload.data()) : 0;
  if (type == kBuffer && size == kBufferMessageSize && fds.size() == 1) {
    attachBuffer(readLe32(payload.data() + 4), readLe32(payload.data() + 8), readLe32(payload.data() + 12), fds[0].get());
    return connected();
  }
  if (type == kDmabuf && !m_path.empty()) {
    fail("DMABUF on a version 1 stream");
    return false;
  }
  if (type == kDmabuf && size == kDmabufMessageSize && fds.size() == 1) {
    const std::uint8_t* p = payload.data();
    const std::uint64_t modifier = readLe32(p + 20) | (std::uint64_t{readLe32(p + 24)} << 32U);
    attachDmabuf(
        readLe32(p + 4), readLe32(p + 8), readLe32(p + 12), readLe32(p + 16), modifier, readLe32(p + 28),
        readLe32(p + 32), fds[0].release()
    );
    return connected();
  }
  if (type == kFrame && size == kFrameMessageSize && fds.empty()) {
    const std::uint32_t id = readLe32(payload.data() + 4);
    if (!m_requested) {
      fail("FRAME without a pending REQUEST");
      return false;
    }
    if (id >= kMaxBuffers || (m_buffers[id].data == nullptr && m_buffers[id].dmabufFd < 0)) {
      fail("FRAME names an unattached buffer");
      return false;
    }
    m_requested = false;
    m_frameBuffer = id;
    notify();
    return connected();
  }
  fail("malformed message");
  return false;
}

void PixelStreamClient::attachBuffer(std::uint32_t id, std::uint32_t width, std::uint32_t height, int fd) {
  if (id >= kMaxBuffers) {
    fail("BUFFER id out of range");
    return;
  }
  if (m_frameBuffer == id) {
    fail("BUFFER replaces the buffer the shell is reading");
    return;
  }
  if (width == 0
      || height == 0
      || width > kMaxDimension
      || height > kMaxDimension
      || std::size_t{width} * height > kMaxPixels) {
    fail("BUFFER dimensions out of range");
    return;
  }
  const std::size_t bytes = std::size_t{width} * height * 4;
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || static_cast<std::size_t>(info.st_size) < bytes) {
    fail("BUFFER file is smaller than its pixels");
    return;
  }
  // A shrinkable mapping could SIGBUS the shell; require the companion to seal it.
  const int seals = ::fcntl(fd, F_GET_SEALS);
  if (seals < 0 || (seals & F_SEAL_SHRINK) == 0) {
    fail("BUFFER memfd is not sealed against shrinking");
    return;
  }
  void* data = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    fail("BUFFER mmap failed");
    return;
  }
  Buffer& buffer = m_buffers[id];
  unmap(buffer);
  buffer = Buffer{.data = data, .size = bytes, .width = static_cast<int>(width), .height = static_cast<int>(height)};
}

void PixelStreamClient::attachDmabuf(
    std::uint32_t id, std::uint32_t width, std::uint32_t height, std::uint32_t fourcc, std::uint64_t modifier,
    std::uint32_t stride, std::uint32_t offset, int fd
) {
  if (id >= kMaxBuffers || m_frameBuffer == id || width == 0 || height == 0 || width > kMaxDimension
      || height > kMaxDimension) {
    ::close(fd);
    fail("DMABUF out of range or replaces the buffer the shell is reading");
    return;
  }
  constexpr auto code = [](char a, char b, char c, char d) {
    return std::uint32_t(a) | std::uint32_t(b) << 8U | std::uint32_t(c) << 16U | std::uint32_t(d) << 24U;
  };
  const bool rgba8 = fourcc == code('A', 'B', '2', '4') || fourcc == code('X', 'B', '2', '4')
      || fourcc == code('A', 'R', '2', '4') || fourcc == code('X', 'R', '2', '4');
  const off_t fdSize = ::lseek(fd, 0, SEEK_END);
  if (!rgba8 || std::size_t{width} * height > kMaxPixels || stride < std::uint64_t{width} * 4
      || stride > INT32_MAX || offset > INT32_MAX || fdSize < 0
      || static_cast<std::uint64_t>(fdSize) < offset + std::uint64_t{stride} * height) {
    ::close(fd);
    fail("DMABUF layout does not fit its buffer");
    return;
  }
  Buffer& buffer = m_buffers[id];
  unmap(buffer);
  buffer = Buffer{
      .width = static_cast<int>(width),
      .height = static_cast<int>(height),
      .dmabufFd = fd,
      .fourcc = fourcc,
      .modifier = modifier,
      .stride = stride,
      .offset = offset,
      .serial = ++m_serial,
  };
}

void PixelStreamClient::refuseDmabuf() {
  if (connected()) {
    (void)send(kNoDmabuf, 0);
  }
}

void PixelStreamClient::setPaused(bool paused) {
  if (std::exchange(m_paused, paused) != paused && m_path.empty() && connected()) {
    (void)send(kPause, paused ? 1 : 0);
  }
}

void PixelStreamClient::unmap(Buffer& buffer) {
  if (buffer.data != nullptr) {
    ::munmap(buffer.data, buffer.size);
  }
  if (buffer.dmabufFd >= 0) {
    ::close(buffer.dmabufFd);
  }
  buffer = Buffer{};
}

std::optional<PixelStreamClient::Frame> PixelStreamClient::frame() const {
  if (!m_frameBuffer.has_value()) {
    return std::nullopt;
  }
  const Buffer& buffer = m_buffers[*m_frameBuffer];
  return Frame{
      .pixels = static_cast<const std::uint8_t*>(buffer.data),
      .width = buffer.width,
      .height = buffer.height,
      .dmabufFd = buffer.dmabufFd,
      .fourcc = buffer.fourcc,
      .modifier = buffer.modifier,
      .stride = buffer.stride,
      .offset = buffer.offset,
      .serial = buffer.serial,
  };
}

void PixelStreamClient::releaseFrame(int fenceFd) {
  if (!m_frameBuffer.has_value()) {
    if (fenceFd >= 0) {
      ::close(fenceFd);
    }
    return;
  }
  m_frameBuffer.reset();
  if (fenceFd >= 0 && connected()) {
    closeReleaseFence();
    m_releaseFence = fenceFd;
    return;
  }
  if (fenceFd >= 0) {
    ::close(fenceFd);
  }
  if (connected() && send(kRequest, 0)) {
    m_requested = true;
  }
}

void PixelStreamClient::dispatchReleaseFence() {
  if (m_releaseFence < 0) {
    return;
  }
  closeReleaseFence();
  if (connected() && !m_requested && send(kRequest, 0)) {
    m_requested = true;
  }
}

void PixelStreamClient::closeReleaseFence() {
  if (m_releaseFence >= 0) {
    ::close(m_releaseFence);
    m_releaseFence = -1;
  }
}

void PixelStreamClient::notify() {
  if (m_onChange) {
    m_onChange();
  }
}
