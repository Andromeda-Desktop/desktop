#include "render/core/shared_texture_cache.h"

#include "core/log.h"
#include "core/sys/sys.h"
#include "render/backend/render_backend.h"
#include "render/core/image_file_loader.h"
#include "render/core/image_source_log.h"
#include "render/core/texture_manager.h"
#include "render/gl_shared_context.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  constexpr Logger kLog("texcache");

  // Keeps the first channel of each RGBA pixel, packed tightly.
  void packFirstChannel(std::vector<std::uint8_t>& rgba, int width, int height) {
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
      rgba[pixel] = rgba[pixel * 4U];
    }
    rgba.resize(pixelCount);
  }

  [[nodiscard]] bool allOpaque(const std::vector<std::uint8_t>& rgba) {
    for (std::size_t index = 3; index < rgba.size(); index += 4) {
      if (rgba[index] != 0xFF) {
        return false;
      }
    }
    return true;
  }

  constexpr int kBlurredMaxSide = 1024;

  // A large image reduced for its coverage is kept on disk, so the next start (the login wallpaper of the shell and
  // the lock) reads the reduced pixels instead of decoding the source again (~500 ms for 16 MP on an HD 520 laptop).
  // Pixels are RGBA before any blur. The key holds the source's size and mtime, so an edited file misses.
  namespace pixel_cache {
    constexpr std::uint32_t kMagic = 0x31435444U;
    constexpr std::size_t kMaxFiles = 4;

    struct Header {
      std::uint32_t magic = kMagic;
      std::int32_t width = 0;
      std::int32_t height = 0;
      std::int32_t sourceWidth = 0;
      std::int32_t sourceHeight = 0;
      std::uint32_t opaque = 0;
    };

    std::filesystem::path directory() {
      if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "andromeda" / "textures";
      }
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".cache" / "andromeda" / "textures";
      }
      return {};
    }

    std::optional<std::filesystem::path> fileFor(const std::string& path, TextureCoverage coverage) {
      struct stat info {};
      if (path.empty() || path.front() != '/' || coverage.empty() || ::stat(path.c_str(), &info) != 0) {
        return std::nullopt;
      }
      const std::filesystem::path dir = directory();
      if (dir.empty()) {
        return std::nullopt;
      }
      std::uint64_t hash = 14695981039346656037ULL;
      const auto mix = [&hash](const void* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
          hash = (hash ^ static_cast<const std::uint8_t*>(data)[i]) * 1099511628211ULL;
        }
      };
      mix(path.data(), path.size());
      const std::int64_t fields[] = {
          // ctime and the inode change on every write or replacement, even when a copy keeps the old mtime.
          static_cast<std::int64_t>(info.st_dev), static_cast<std::int64_t>(info.st_ino), info.st_size,
          sys::ctimeNs(info), coverage.width, coverage.height,
      };
      mix(fields, sizeof(fields));
      return dir / std::format("{:016x}.rgba", hash);
    }

    bool read(const std::filesystem::path& file, Header& header, std::vector<std::uint8_t>& pixels) {
      std::ifstream in(file, std::ios::binary);
      if (!in.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.magic != kMagic || header.width <= 0
          || header.height <= 0 || header.width > header.sourceWidth || header.height > header.sourceHeight) {
        return false;
      }
      const std::size_t size = static_cast<std::size_t>(header.width) * static_cast<std::size_t>(header.height) * 4U;
      std::error_code ec;
      // A damaged header must not size the buffer: only a file of exactly this length is used.
      if (std::filesystem::file_size(file, ec) != sizeof(header) + size || ec) {
        return false;
      }
      pixels.resize(size);
      if (!in.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()))) {
        return false;
      }
      std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), ec);
      return true;
    }

    // Written to a per-process temporary name and renamed, since the shell and the lock may write the same key.
    void write(const std::filesystem::path& file, const Header& header, const std::vector<std::uint8_t>& pixels) {
      std::error_code ec;
      std::filesystem::create_directories(file.parent_path(), ec);
      const std::filesystem::path temporary = std::format("{}.{}.tmp", file.string(), ::getpid());
      {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        if (!out.flush()) {
          std::filesystem::remove(temporary, ec);
          return;
        }
      }
      std::filesystem::rename(temporary, file, ec);
      if (ec) {
        std::filesystem::remove(temporary, ec);
        return;
      }
      std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
      for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), ec)) {
        if (entry.path().extension() == ".rgba") {
          files.emplace_back(entry.last_write_time(ec), entry.path());
        }
      }
      if (files.size() > kMaxFiles) {
        std::ranges::sort(files, std::greater{}, &decltype(files)::value_type::first);
        for (std::size_t i = kMaxFiles; i < files.size(); ++i) {
          std::filesystem::remove(files[i].second, ec);
        }
      }
    }
  } // namespace pixel_cache

  // Number of 2x halvings that keep the image at least `coverage` on both axes.
  [[nodiscard]] int reductionLevel(int width, int height, TextureCoverage coverage) {
    if (coverage.empty()) {
      return 0;
    }
    int level = 0;
    while ((width >> (level + 1)) >= coverage.width && (height >> (level + 1)) >= coverage.height) {
      ++level;
    }
    return level;
  }

  // One mip step: each output pixel averages a 2x2 block, the box filter glGenerateMipmap uses. An odd last row or
  // column is dropped, matching the floor(size / 2) level dimensions. Runs in place: every write lands at or before
  // the block being read, and only on bytes no later block reads.
  void halveInPlace(std::vector<std::uint8_t>& pixels, int& width, int& height, int channels) {
    const int halfWidth = width / 2;
    const int halfHeight = height / 2;
    const auto channelCount = static_cast<std::size_t>(channels);
    const std::size_t stride = static_cast<std::size_t>(width) * channelCount;
    std::uint8_t* data = pixels.data();
    for (int y = 0; y < halfHeight; ++y) {
      const std::uint8_t* top = data + (static_cast<std::size_t>(y) * 2U * stride);
      const std::uint8_t* bottom = top + stride;
      std::uint8_t* out = data + (static_cast<std::size_t>(y) * static_cast<std::size_t>(halfWidth) * channelCount);
      for (int x = 0; x < halfWidth; ++x) {
        const std::size_t left = static_cast<std::size_t>(x) * 2U * channelCount;
        for (std::size_t c = 0; c < channelCount; ++c) {
          const std::size_t s = left + c;
          const unsigned sum = top[s] + top[s + channelCount] + bottom[s] + bottom[s + channelCount] + 2U;
          out[(static_cast<std::size_t>(x) * channelCount) + c] = static_cast<std::uint8_t>(sum >> 2U);
        }
      }
    }
    width = halfWidth;
    height = halfHeight;
    pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * channelCount);
  }
} // namespace

// ── Lease ────────────────────────────────────────────────────────────────────

SharedTextureCache::Lease::Lease(SharedTextureCache* cache, std::weak_ptr<void> lifetimeToken, std::uint64_t id)
    : m_cache(cache), m_lifetimeToken(std::move(lifetimeToken)), m_id(id) {}

SharedTextureCache::Lease::~Lease() { reset(); }

SharedTextureCache::Lease::Lease(Lease&& other) noexcept
    : m_cache(std::exchange(other.m_cache, nullptr)), m_lifetimeToken(std::move(other.m_lifetimeToken)),
      m_id(std::exchange(other.m_id, 0)) {}

SharedTextureCache::Lease& SharedTextureCache::Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    reset();
    m_cache = std::exchange(other.m_cache, nullptr);
    m_lifetimeToken = std::move(other.m_lifetimeToken);
    m_id = std::exchange(other.m_id, 0);
  }
  return *this;
}

SharedTextureCache* SharedTextureCache::Lease::liveCache() const {
  return m_cache != nullptr && !m_lifetimeToken.expired() ? m_cache : nullptr;
}

TextureHandle SharedTextureCache::Lease::texture() const {
  const SharedTextureCache* cache = liveCache();
  const Entry* entry = cache != nullptr ? cache->findLeaseEntry(m_id) : nullptr;
  return entry != nullptr && entry->state == EntryState::Ready ? entry->handle : TextureHandle{};
}

bool SharedTextureCache::Lease::failed() const {
  const SharedTextureCache* cache = liveCache();
  const Entry* entry = cache != nullptr ? cache->findLeaseEntry(m_id) : nullptr;
  return entry != nullptr && entry->state == EntryState::Failed;
}

void SharedTextureCache::Lease::setCoverage(TextureCoverage coverage) {
  if (SharedTextureCache* cache = liveCache(); cache != nullptr) {
    cache->setLeaseCoverage(m_id, coverage);
  }
}

void SharedTextureCache::Lease::reset() {
  if (SharedTextureCache* cache = liveCache(); cache != nullptr) {
    cache->releaseLease(m_id);
  }
  m_cache = nullptr;
  m_lifetimeToken.reset();
  m_id = 0;
}

// ── Cache ────────────────────────────────────────────────────────────────────

SharedTextureCache::SharedTextureCache() {
  if (!m_wake.valid()) {
    kLog.warn("failed to create the wake fd; wallpaper textures will never finish loading");
  }
}

SharedTextureCache::~SharedTextureCache() {
  m_lifetimeToken.reset();
  m_leases.clear();

  {
    std::scoped_lock lock(m_queueMutex);
    m_shutdown.store(true);
  }
  m_queueCv.notify_all();
  m_workers.joinAll();

  // Textures uploaded into consumer backends die with those backends' contexts. Best-effort for the shared ones: if
  // the context can't be bound (lost on resume), the GL textures are already gone.
  if (m_textureManager != nullptr && makeCurrentFor(EntryKey{})) {
    m_textureManager->cleanup();
  }

}

void SharedTextureCache::initialize(GlSharedContext* sharedGl) {
  m_sharedGl = sharedGl;
  if (m_sharedGl != nullptr) {
    m_textureManager = createDefaultTextureManager();
  }
}

SharedTextureCache::Lease
SharedTextureCache::acquire(SharedTextureRequest request, RenderBackend* backend, std::function<void()> onChange) {
  if (request.path.empty()) {
    return {};
  }
  request.coverage = effectiveCoverage(request.coverage);
  EntryKey key{.request = std::move(request), .backend = m_textureManager != nullptr ? nullptr : backend};
  if (m_textureManager == nullptr && key.backend == nullptr) {
    return {};
  }

  retainEntry(key);
  const std::uint64_t id = ++m_nextLeaseId;
  m_leases.emplace(id, LeaseRecord{.key = std::move(key), .onChange = std::move(onChange)});
  return Lease{this, m_lifetimeToken, id};
}

void SharedTextureCache::abandonGpuResources() noexcept {
  if (m_textureManager != nullptr) {
    m_textureManager->abandonGpuResources();
  }
  // A reset loses every context, including the per-backend ones.
  for (auto& [key, entry] : m_entries) {
    (void)key;
    if (entry.state == EntryState::Ready) {
      entry.handle = {};
      entry.state = EntryState::Loading;
    }
  }
}

void SharedTextureCache::reloadResidentTextures() {
  for (const auto& [key, entry] : m_entries) {
    if (entry.state == EntryState::Loading) {
      requestDecode(key.request);
    }
  }
}

void SharedTextureCache::doAddPollFds(std::vector<pollfd>& fds) {
  if (!m_wake.valid()) {
    return;
  }
  fds.push_back({.fd = m_wake.pollFd(), .events = POLLIN, .revents = 0});
}

void SharedTextureCache::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (!m_wake.valid() || startIdx >= fds.size() || (fds[startIdx].revents & POLLIN) == 0) {
    return;
  }

  m_wake.drain();

  std::deque<DecodedImage> results;
  {
    std::scoped_lock lock(m_resultMutex);
    results = std::move(m_results);
    m_results.clear();
  }

  std::vector<EntryKey> changed;
  for (const DecodedImage& image : results) {
    m_pendingDecodes.erase(image.request);
    for (auto& [key, entry] : m_entries) {
      if (key.request != image.request || entry.state != EntryState::Loading) {
        continue;
      }
      if (image.failed) {
        entry.state = EntryState::Failed;
      } else {
        upload(key, entry, image);
      }
      if (entry.state != EntryState::Loading) {
        changed.push_back(key);
      }
    }
  }

  std::vector<std::uint64_t> notify;
  for (auto& [id, record] : m_leases) {
    const bool pendingChanged =
        record.pending.has_value() && std::ranges::find(changed, *record.pending) != changed.end();
    const bool switched = pendingChanged && resolvePending(record);
    if (record.onChange && (switched || std::ranges::find(changed, record.key) != changed.end())) {
      notify.push_back(id);
    }
  }
  // Coverage changes whose target was already resident switch here, so onChange stays the only way texture() changes.
  for (const std::uint64_t id : std::exchange(m_deferredSwitches, {})) {
    const auto it = m_leases.find(id);
    if (it != m_leases.end() && it->second.pending.has_value() && resolvePending(it->second) && it->second.onChange) {
      notify.push_back(id);
    }
  }
  // Callbacks may acquire or release leases, so look each one up again and call a copy.
  for (const std::uint64_t id : notify) {
    const auto it = m_leases.find(id);
    if (it == m_leases.end()) {
      continue;
    }
    const auto onChange = it->second.onChange;
    onChange();
  }
}

void SharedTextureCache::retainEntry(const EntryKey& key) {
  auto [it, inserted] = m_entries.try_emplace(key);
  ++it->second.refCount;
  if (inserted) {
    requestDecode(key.request);
  }
}

void SharedTextureCache::releaseEntry(const EntryKey& key) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return;
  }
  if (--it->second.refCount > 0) {
    return;
  }
  unloadEntry(it->first, it->second);
  m_entries.erase(it);
  kLog.info("evicted {}", key.request.path);
  cancelDecodeIfUnused(key.request);
}

void SharedTextureCache::releaseLease(std::uint64_t id) {
  const auto it = m_leases.find(id);
  if (it == m_leases.end()) {
    return;
  }
  const EntryKey key = std::move(it->second.key);
  const std::optional<EntryKey> pending = std::move(it->second.pending);
  m_leases.erase(it);
  releaseEntry(key);
  if (pending.has_value()) {
    releaseEntry(*pending);
  }
}

const SharedTextureCache::Entry* SharedTextureCache::findLeaseEntry(std::uint64_t id) const {
  const auto lease = m_leases.find(id);
  if (lease == m_leases.end()) {
    return nullptr;
  }
  const auto entry = m_entries.find(lease->second.key);
  return entry != m_entries.end() ? &entry->second : nullptr;
}

void SharedTextureCache::setLeaseCoverage(std::uint64_t id, TextureCoverage coverage) {
  const auto it = m_leases.find(id);
  if (it == m_leases.end()) {
    return;
  }
  LeaseRecord& record = it->second;
  EntryKey target = record.key;
  target.request.coverage = effectiveCoverage(coverage);

  if (target == record.key) {
    if (record.pending.has_value()) {
      const EntryKey pending = *std::exchange(record.pending, std::nullopt);
      releaseEntry(pending);
    }
    return;
  }
  if (record.pending == target) {
    return;
  }

  retainEntry(target);
  if (record.pending.has_value()) {
    const EntryKey pending = *std::exchange(record.pending, std::nullopt);
    releaseEntry(pending);
  }

  const EntryState targetState = m_entries.find(target)->second.state;
  const auto active = m_entries.find(record.key);
  const bool activeReady = active != m_entries.end() && active->second.state == EntryState::Ready;
  if (targetState == EntryState::Failed && activeReady) {
    releaseEntry(target);
    return;
  }
  if (targetState == EntryState::Loading && !activeReady) {
    // Nothing is shown either way, so retarget the lease now.
    const EntryKey previous = std::exchange(record.key, std::move(target));
    releaseEntry(previous);
    return;
  }
  record.pending = std::move(target);
  if (targetState != EntryState::Loading) {
    m_deferredSwitches.push_back(id);
    signalMain();
  }
}

bool SharedTextureCache::resolvePending(LeaseRecord& record) {
  const auto it = m_entries.find(*record.pending);
  if (it == m_entries.end()) {
    record.pending.reset();
    return false;
  }
  if (it->second.state == EntryState::Loading) {
    return false;
  }
  const EntryKey pending = *std::exchange(record.pending, std::nullopt);
  if (it->second.state == EntryState::Failed) {
    releaseEntry(pending);
    return false;
  }
  const EntryKey previous = std::exchange(record.key, pending);
  releaseEntry(previous);
  return true;
}

TextureCoverage SharedTextureCache::effectiveCoverage(TextureCoverage coverage) {
  return coverage.empty() || !TextureManager::globalMipmapsEnabled() ? TextureCoverage{} : coverage;
}

void SharedTextureCache::requestDecode(const SharedTextureRequest& request) {
  if (!m_pendingDecodes.insert(request).second) {
    return;
  }
  {
    std::scoped_lock lock(m_queueMutex);
    m_jobQueue.push_back(request);
    // One worker: wallpapers decode to hundreds of MB each, and they are requested one at a time anyway.
    m_workers.spawn([this](std::size_t slot) { workerLoop(slot); });
  }
  m_queueCv.notify_one();
}

void SharedTextureCache::cancelDecodeIfUnused(const SharedTextureRequest& request) {
  if (!m_pendingDecodes.contains(request) || hasLoadingEntry(request)) {
    return;
  }
  // A decode already running finishes and its result is dropped in dispatch(); only queued work can be skipped.
  bool removed = false;
  {
    std::scoped_lock lock(m_queueMutex);
    if (const auto it = std::ranges::find(m_jobQueue, request); it != m_jobQueue.end()) {
      m_jobQueue.erase(it);
      removed = true;
    }
  }
  if (removed) {
    m_pendingDecodes.erase(request);
  }
}

bool SharedTextureCache::hasLoadingEntry(const SharedTextureRequest& request) const {
  return std::ranges::any_of(m_entries, [&request](const auto& item) {
    return item.first.request == request && item.second.state == EntryState::Loading;
  });
}

void SharedTextureCache::upload(const EntryKey& key, Entry& entry, const DecodedImage& image) {
  if (!makeCurrentFor(key)) {
    return;
  }
  TextureManager* manager = textureManagerFor(key);
  TextureHandle handle = key.request.kind == SharedTextureKind::AlphaMask
      ? manager->loadFromPixels(
            image.pixels.data(), image.width, image.height, TextureDataFormat::Alpha, TextureFilter::Linear, true
        )
      : manager->loadFromRgba(image.pixels.data(), image.width, image.height, true);
  if (!handle.valid()) {
    kLog.warn("failed to upload {} ({}x{})", key.request.path, image.width, image.height);
    entry.state = EntryState::Failed;
    return;
  }
  handle.sourceWidth = image.sourceWidth;
  handle.sourceHeight = image.sourceHeight;
  handle.opaque = image.opaque;
  handle.brightLuminance = image.brightLuminance;
  entry.handle = handle;
  entry.state = EntryState::Ready;
  kLog.info(
      "uploaded {} ({}x{} from {}x{})", key.request.path, image.width, image.height, image.sourceWidth,
      image.sourceHeight
  );
}

void SharedTextureCache::unloadEntry(const EntryKey& key, Entry& entry) {
  if (entry.state != EntryState::Ready || !entry.handle.valid()) {
    return;
  }
  // If the context can't be bound (lost on resume), the GPU object is already gone.
  if (makeCurrentFor(key)) {
    textureManagerFor(key)->unload(entry.handle);
  }
  entry.handle = {};
}

TextureManager* SharedTextureCache::textureManagerFor(const EntryKey& key) const {
  return key.backend != nullptr ? &key.backend->textureManager() : m_textureManager.get();
}

bool SharedTextureCache::makeCurrentFor(const EntryKey& key) {
  if (key.backend != nullptr) {
    return key.backend->makeCurrentNoSurface();
  }
  if (m_sharedGl == nullptr) {
    return false;
  }
  // Backend contexts share the root context's share-list, so uploads/deletes work on whichever context is bound. If
  // a backend already owns the thread's context (mid-frame), switching away would drop its draw surface and break its
  // trailing eglSwapBuffers.
  if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
    return true;
  }
  return m_sharedGl->makeCurrentSurfaceless();
}

void SharedTextureCache::workerLoop(std::size_t slot) {
  while (true) {
    // Every queued request for the same file shares one decode; each coverage gets its own reduction.
    std::vector<SharedTextureRequest> batch;
    {
      std::unique_lock lock(m_queueMutex);
      if (!m_queueCv.wait_for(lock, IdleWorkerSlots::kIdleExit, [this] {
            return m_shutdown.load() || !m_jobQueue.empty();
          })) {
        m_workers.exited(slot);
        return;
      }
      if (m_shutdown.load()) {
        return;
      }
      batch.push_back(std::move(m_jobQueue.front()));
      m_jobQueue.pop_front();
      for (auto it = m_jobQueue.begin(); it != m_jobQueue.end();) {
        if (it->path == batch.front().path && it->kind == batch.front().kind) {
          batch.push_back(std::move(*it));
          it = m_jobQueue.erase(it);
        } else {
          ++it;
        }
      }
    }

    // A copy: reduce() moves each request out of the batch before store() needs the path.
    const std::string path = batch.front().path;
    const bool alphaMask = batch.front().kind == SharedTextureKind::AlphaMask;
    const auto blur = [alphaMask](DecodedImage& image) {
      if (image.request.blur <= 0.0F) {
        if (!alphaMask) {
          image.brightLuminance = rgbaBrightLuminance(image.pixels);
        }
        return;
      }
      // A blurred image keeps no fine detail, so a small copy looks the same and blurs many times faster.
      while (std::max(image.width, image.height) > kBlurredMaxSide) {
        halveInPlace(image.pixels, image.width, image.height, 4);
      }
      const float radius = image.request.blur * static_cast<float>(std::max(image.width, image.height));
      blurRgba(image.pixels, image.width, image.height, static_cast<int>(std::lround(radius)));
      if (!alphaMask) {
        image.brightLuminance = rgbaBrightLuminance(image.pixels);
      }
    };

    if (!alphaMask) {
      std::vector<DecodedImage> cached;
      for (const SharedTextureRequest& request : batch) {
        const auto file = pixel_cache::fileFor(path, request.coverage);
        DecodedImage image{.request = request};
        pixel_cache::Header header;
        if (!file || !pixel_cache::read(*file, header, image.pixels)) {
          break;
        }
        image.width = header.width;
        image.height = header.height;
        image.sourceWidth = header.sourceWidth;
        image.sourceHeight = header.sourceHeight;
        image.opaque = header.opaque != 0;
        cached.push_back(std::move(image));
      }
      if (cached.size() == batch.size()) {
        for (DecodedImage& image : cached) {
          blur(image);
          pushResult(std::move(image));
        }
        continue;
      }
    }

    auto loaded = loadImageFile(path);
    if (!loaded) {
      kLog.warn("failed to decode {} ({})", ImageSourceLog::describe(path), loaded.error());
      for (SharedTextureRequest& request : batch) {
        pushResult(DecodedImage{.request = std::move(request), .failed = true});
      }
      continue;
    }

    const int sourceWidth = loaded->width;
    const int sourceHeight = loaded->height;
    if (alphaMask) {
      packFirstChannel(loaded->rgba, sourceWidth, sourceHeight);
    }
    // Averaging fully opaque pixels stays fully opaque, so the source answer holds for every reduction.
    const bool opaque = !alphaMask && allOpaque(loaded->rgba);

    // Smallest reduction first, so each result is copied out before the buffer is halved further.
    std::ranges::sort(batch, {}, [sourceWidth, sourceHeight](const SharedTextureRequest& request) {
      return reductionLevel(sourceWidth, sourceHeight, request.coverage);
    });
    std::vector<std::uint8_t> pixels = std::move(loaded->rgba);
    int width = sourceWidth;
    int height = sourceHeight;
    int level = 0;
    const auto reduce = [&](SharedTextureRequest& request) {
      const int target = reductionLevel(sourceWidth, sourceHeight, request.coverage);
      for (; level < target; ++level) {
        halveInPlace(pixels, width, height, alphaMask ? 1 : 4);
      }
      return DecodedImage{
          .request = std::move(request),
          .width = width,
          .height = height,
          .sourceWidth = sourceWidth,
          .sourceHeight = sourceHeight,
          .opaque = opaque,
      };
    };
    // Only a reduced image: a full-size copy would cost as much disk as it saves decode time. Runs after the result is
    // pushed, so a cache miss does not wait for the disk write.
    const auto store = [&](TextureCoverage coverage) {
      if (alphaMask || level == 0) {
        return;
      }
      if (const auto file = pixel_cache::fileFor(path, coverage)) {
        pixel_cache::write(
            *file,
            {.width = width,
             .height = height,
             .sourceWidth = sourceWidth,
             .sourceHeight = sourceHeight,
             .opaque = opaque ? 1U : 0U},
            pixels
        );
      }
    };
    for (std::size_t i = 0; i + 1 < batch.size(); ++i) {
      DecodedImage image = reduce(batch[i]);
      const TextureCoverage coverage = image.request.coverage;
      image.pixels = pixels;
      blur(image);
      pushResult(std::move(image));
      store(coverage);
    }
    DecodedImage last = reduce(batch.back());
    const TextureCoverage coverage = last.request.coverage;
    const bool keep = !alphaMask && level > 0;
    if (keep) {
      last.pixels = pixels;
    } else {
      last.pixels = std::move(pixels);
    }
    blur(last);
    pushResult(std::move(last));
    store(coverage);
  }
}

void SharedTextureCache::pushResult(DecodedImage image) {
  {
    std::scoped_lock lock(m_resultMutex);
    m_results.push_back(std::move(image));
  }
  signalMain();
}

void SharedTextureCache::signalMain() {
  if (!m_wake.valid()) {
    return;
  }
  if (!m_wake.signal()) {
    kLog.warn("failed to signal texture cache wake: errno={}", errno);
  }
}

std::size_t SharedTextureCache::RequestHash::operator()(const SharedTextureRequest& request) const noexcept {
  return std::hash<std::string>{}(request.path) ^ (static_cast<std::size_t>(request.kind) << 1U);
}

std::size_t SharedTextureCache::EntryKeyHash::operator()(const EntryKey& key) const noexcept {
  return RequestHash{}(key.request) ^ (std::hash<const void*>{}(key.backend) << 2U);
}
