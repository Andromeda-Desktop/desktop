#include "render/core/async_texture_cache.h"

#include "core/log.h"
#include "render/backend/render_backend.h"
#include "render/core/image_file_loader.h"
#include "render/core/image_source_log.h"
#include "render/core/texture_manager.h"
#include "render/gl_shared_context.h"

#include <algorithm>
#include <tuple>
#include <cerrno>
#include <cstdint>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("async-tex");
  constexpr std::size_t kMinWorkers = 2;
  constexpr std::size_t kMaxWorkers = 4;
  constexpr std::size_t kMaxUnusedEntries = 128;
  constexpr std::size_t kMaxUnusedBytes = 64 * 1024 * 1024;

} // namespace

AsyncTextureCache::ReadySubscription::ReadySubscription(
    AsyncTextureCache* cache, std::weak_ptr<void> lifetimeToken, std::uint64_t id
)
    : m_cache(cache), m_lifetimeToken(std::move(lifetimeToken)), m_id(id) {}

AsyncTextureCache::ReadySubscription::~ReadySubscription() { disconnect(); }

AsyncTextureCache::ReadySubscription::ReadySubscription(ReadySubscription&& other) noexcept
    : m_cache(other.m_cache), m_lifetimeToken(std::move(other.m_lifetimeToken)), m_id(other.m_id) {
  other.m_cache = nullptr;
  other.m_id = 0;
}

AsyncTextureCache::ReadySubscription&
AsyncTextureCache::ReadySubscription::operator=(ReadySubscription&& other) noexcept {
  if (this != &other) {
    disconnect();
    m_cache = other.m_cache;
    m_lifetimeToken = std::move(other.m_lifetimeToken);
    m_id = other.m_id;
    other.m_cache = nullptr;
    other.m_id = 0;
  }
  return *this;
}

void AsyncTextureCache::ReadySubscription::disconnect() {
  if (m_id == 0) {
    return;
  }
  if (m_cache != nullptr && !m_lifetimeToken.expired()) {
    m_cache->removeReadyListener(m_id);
  }
  m_cache = nullptr;
  m_lifetimeToken.reset();
  m_id = 0;
}

AsyncTextureCache::AsyncTextureCache()
    : m_unused(
          {.entries = kMaxUnusedEntries, .bytes = kMaxUnusedBytes}, [](const std::size_t& bytes) { return bytes; },
          [this](RequestKey& key, std::size_t&) {
            const auto it = m_entries.find(key);
            if (it->second.handle.id != 0) {
              makeCurrent();
              m_textureManager->unload(it->second.handle);
            }
            m_entries.erase(it);
          }
      ) {
  m_textureManager = createDefaultTextureManager();

  if (!m_wake.valid()) {
    kLog.warn("failed to create the wake fd; async texture cache notifications will be disabled");
  }

  const unsigned hc = std::thread::hardware_concurrency();
  const std::size_t suggested = hc == 0U ? kMinWorkers : std::max<std::size_t>(kMinWorkers, hc / 2U);
  const std::size_t workerCount = std::clamp<std::size_t>(suggested, kMinWorkers, kMaxWorkers);
  m_workers = std::make_unique<IdleWorkerSlots>(workerCount, "dsk-texture");
}

AsyncTextureCache::~AsyncTextureCache() {
  m_lifetimeToken.reset();
  m_readyListeners.clear();

  {
    std::scoped_lock lock(m_queueMutex);
    m_shutdown.store(true);
  }
  m_queueCv.notify_all();
  m_workers->joinAll();

  if (!m_entries.empty()) {
    makeCurrent();
    for (auto& [key, entry] : m_entries) {
      (void)key;
      if (entry.handle.id != 0) {
        m_textureManager->unload(entry.handle);
      }
    }
    m_entries.clear();
  }
  m_textureManager->cleanup();

}

namespace {
  std::expected<LoadedImageFile, std::string>
  decodeImage(const std::string& path, int targetSize, AsyncTextureCache::DecodeOptions options) {
    auto loaded = loadImageFile(path, targetSize, options.centerSquareCrop);
    if (loaded && options.blurRadius > 0) {
      blurRgba(loaded->rgba, loaded->width, loaded->height, options.blurRadius);
    }
    return loaded;
  }
} // namespace

void AsyncTextureCache::initialize(GlSharedContext* sharedGl) { m_sharedGl = sharedGl; }

AsyncTextureCache::ReadySubscription
AsyncTextureCache::subscribeReady(
    const std::string& path, int targetSize, bool mipmap, TextureReadyCallback callback, DecodeOptions options
) {
  auto key = makeKey(path, targetSize, mipmap, options);
  if (key.path.empty() || !callback) {
    return {};
  }

  if (const auto handle = peek(key.path, key.targetSize, key.mipmap, key.options); handle.id != 0) {
    callback(handle);
    return {};
  }

  const std::uint64_t id = ++m_nextReadyListenerId;
  m_readyListeners.emplace(id, ReadyListener{.key = std::move(key), .callback = std::move(callback)});
  return ReadySubscription{this, m_lifetimeToken, id};
}

TextureHandle AsyncTextureCache::acquire(const std::string& path, int targetSize, bool mipmap, DecodeOptions options) {
  const auto key = makeKey(path, targetSize, mipmap, options);
  if (key.path.empty()) {
    return {};
  }

  auto [it, inserted] = m_entries.try_emplace(key);
  Entry& entry = it->second;
  if (inserted) {
    entry.refCount = 1;
  } else {
    ++entry.refCount;
  }
  std::ignore = m_unused.extract(key);

  if (entry.handle.id != 0 || entry.failed) {
    return entry.handle;
  }

  {
    std::scoped_lock lock(m_queueMutex);
    m_canceled.erase(key);
    if (!m_inFlight.contains(key)) {
      m_inFlight.insert(key);
      m_jobQueue.push_back(key);
      m_workers->spawn([this](std::size_t slot) { workerLoop(slot); });
    }
  }
  m_queueCv.notify_one();
  return {};
}

bool AsyncTextureCache::failed(const std::string& path, int targetSize, bool mipmap, DecodeOptions options) const {
  const auto it = m_entries.find(makeKey(path, targetSize, mipmap, options));
  return it != m_entries.end() && it->second.failed;
}

TextureHandle AsyncTextureCache::peek(const std::string& path, int targetSize, bool mipmap, DecodeOptions options) const {
  const auto key = makeKey(path, targetSize, mipmap, options);
  if (key.path.empty()) {
    return {};
  }

  auto it = m_entries.find(key);
  return it != m_entries.end() ? it->second.handle : TextureHandle{};
}

void AsyncTextureCache::release(const std::string& path, int targetSize, bool mipmap, DecodeOptions options) {
  const auto key = makeKey(path, targetSize, mipmap, options);
  if (key.path.empty()) {
    return;
  }

  auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return;
  }

  if (it->second.refCount > 0) {
    --it->second.refCount;
  }
  if (it->second.refCount > 0) {
    return;
  }

  if (it->second.handle.id != 0 || it->second.failed) {
    // Recently used zero-ref textures stay resident so virtualized views rebind them at once while users scrub.
    if (m_unused.find(key) == nullptr) {
      const TextureHandle& handle = it->second.handle;
      m_unused.put(key, static_cast<std::size_t>(handle.width) * static_cast<std::size_t>(handle.height) * 4U);
    }
    return;
  }

  {
    std::scoped_lock lock(m_queueMutex);
    if (m_inFlight.contains(key)) {
      m_canceled.insert(key);
    }
  }

  m_entries.erase(it);
}

void AsyncTextureCache::doAddPollFds(std::vector<pollfd>& fds) {
  if (!m_wake.valid()) {
    return;
  }
  fds.push_back({.fd = m_wake.pollFd(), .events = POLLIN, .revents = 0});
}

void AsyncTextureCache::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (!m_wake.valid() || startIdx >= fds.size()) {
    return;
  }
  if ((fds[startIdx].revents & POLLIN) == 0) {
    return;
  }

  m_wake.drain();

  std::deque<DecodedJob> jobs;
  {
    std::scoped_lock lock(m_resultMutex);
    jobs = std::move(m_results);
    m_results.clear();
  }
  if (jobs.empty()) {
    return;
  }

  bool madeCurrent = false;

  for (auto& job : jobs) {
    bool dropped = false;
    {
      std::scoped_lock lock(m_queueMutex);
      m_inFlight.erase(job.key);
      if (auto canceled = m_canceled.find(job.key); canceled != m_canceled.end()) {
        m_canceled.erase(canceled);
        dropped = true;
      }
    }
    if (dropped) {
      continue;
    }

    auto entryIt = m_entries.find(job.key);
    if (entryIt == m_entries.end()) {
      continue;
    }

    if (job.failed || job.rgba.empty() || job.width <= 0 || job.height <= 0) {
      entryIt->second.failed = true;
      notifyReady(job.key, TextureHandle{});
      continue;
    }

    if (!madeCurrent) {
      makeCurrent();
      madeCurrent = true;
    }

    entryIt->second.handle = m_textureManager->loadFromRgba(job.rgba.data(), job.width, job.height, job.key.mipmap);
    if (entryIt->second.handle.id == 0) {
      entryIt->second.failed = true;
      notifyReady(job.key, TextureHandle{});
      continue;
    }
    entryIt->second.handle.sourceWidth = job.sourceWidth;
    entryIt->second.handle.sourceHeight = job.sourceHeight;
    notifyReady(job.key, entryIt->second.handle);
  }
}

void AsyncTextureCache::trimUnused(std::size_t maxUnusedEntries) { m_unused.trim({.entries = maxUnusedEntries}); }

void AsyncTextureCache::abandonGpuResources() noexcept {
  if (m_textureManager != nullptr) {
    m_textureManager->abandonGpuResources();
  }
  // An unused entry has no consumer to reload it.
  m_unused.clear([this](RequestKey& key, std::size_t&) { m_entries.erase(key); });
  for (auto& [key, entry] : m_entries) {
    (void)key;
    entry.handle = {};
    entry.failed = false;
  }
}

void AsyncTextureCache::reloadResidentTextures() {
  if (m_textureManager == nullptr || m_entries.empty()) {
    return;
  }

  makeCurrent();

  std::vector<RequestKey> resident;
  resident.reserve(m_entries.size());
  for (const auto& [key, entry] : m_entries) {
    if (entry.handle.id != 0 || entry.refCount > 0) {
      resident.push_back(key);
    }
  }

  for (const RequestKey& key : resident) {
    auto entryIt = m_entries.find(key);
    if (entryIt == m_entries.end()) {
      continue;
    }

    Entry& entry = entryIt->second;
    if (entry.handle.id != 0) {
      m_textureManager->unload(entry.handle);
    }
    entry.handle = {};
    entry.failed = false;

    auto loaded = decodeImage(key.path, key.targetSize, key.options);
    if (!loaded) {
      entry.failed = true;
      kLog.warn("failed to reload image after GPU reset: {} ({})", ImageSourceLog::describe(key.path), loaded.error());
      continue;
    }

    entry.handle = m_textureManager->loadFromRgba(loaded->rgba.data(), loaded->width, loaded->height, key.mipmap);
    if (entry.handle.id == 0) {
      entry.failed = true;
      continue;
    }
    entry.handle.sourceWidth = loaded->sourceWidth;
    entry.handle.sourceHeight = loaded->sourceHeight;
    notifyReady(key, entry.handle);
  }
}

std::size_t AsyncTextureCache::RequestKeyHash::operator()(const RequestKey& key) const noexcept {
  std::size_t seed = std::hash<std::string>{}(key.path);
  seed ^= std::hash<int>{}(key.targetSize) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
  seed ^= std::hash<bool>{}(key.mipmap) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
  seed ^= std::hash<int>{}(key.options.blurRadius * 2 + (key.options.centerSquareCrop ? 1 : 0)) + 0x9e3779b9U
      + (seed << 6U) + (seed >> 2U);
  return seed;
}

void AsyncTextureCache::workerLoop(std::size_t slot) {
  while (true) {
    RequestKey key;
    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      if (!m_queueCv.wait_for(lock, IdleWorkerSlots::kIdleExit, [this]() {
            return m_shutdown.load() || !m_jobQueue.empty();
          })) {
        m_workers->exited(slot);
        return;
      }
      if (m_shutdown.load()) {
        return;
      }
      key = std::move(m_jobQueue.front());
      m_jobQueue.pop_front();
    }

    DecodedJob result;
    result.key = key;

    if (auto loaded = decodeImage(key.path, key.targetSize, key.options)) {
      result.rgba = std::move(loaded->rgba);
      result.width = loaded->width;
      result.height = loaded->height;
      result.sourceWidth = loaded->sourceWidth;
      result.sourceHeight = loaded->sourceHeight;
    } else {
      result.failed = true;
      kLog.warn("failed to decode image: {} ({})", ImageSourceLog::describe(key.path), loaded.error());
    }

    pushResult(std::move(result));
  }
}

void AsyncTextureCache::signalMain() {
  if (!m_wake.valid()) {
    return;
  }

  if (!m_wake.signal()) {
    kLog.warn("failed to signal async texture wake: errno={}", errno);
  }
}

void AsyncTextureCache::pushResult(DecodedJob job) {
  {
    std::scoped_lock lock(m_resultMutex);
    m_results.push_back(std::move(job));
  }
  signalMain();
}

void AsyncTextureCache::makeCurrent() {
  if (m_sharedGl != nullptr) {
    // Uploads are valid on any context in the share group, so if a backend already owns the thread's context
    // (mid-frame), don't switch away — that would drop its draw surface and break its trailing eglSwapBuffers.
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
      return;
    }
    m_sharedGl->makeCurrentSurfaceless();
  } else if (m_makeCurrentCallback) {
    // Non-shared mode: no share group, so uploads must bind RenderContext's own context even if another isolated
    // context is currently bound on the thread.
    m_makeCurrentCallback();
  }
}

void AsyncTextureCache::removeReadyListener(std::uint64_t id) { m_readyListeners.erase(id); }

void AsyncTextureCache::notifyReady(const RequestKey& key, TextureHandle handle) {
  std::vector<std::pair<std::uint64_t, TextureReadyCallback>> callbacks;
  callbacks.reserve(m_readyListeners.size());
  for (const auto& [id, listener] : m_readyListeners) {
    if (listener.key == key && listener.callback) {
      callbacks.emplace_back(id, listener.callback);
    }
  }

  for (auto& [id, callback] : callbacks) {
    if (m_readyListeners.contains(id)) {
      callback(handle);
    }
  }
}

AsyncTextureCache::RequestKey
AsyncTextureCache::makeKey(const std::string& path, int targetSize, bool mipmap, DecodeOptions options) {
  return RequestKey{.path = path, .targetSize = std::max(0, targetSize), .mipmap = mipmap, .options = options};
}
