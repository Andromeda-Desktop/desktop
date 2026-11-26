#include "net/http_client.h"

#include "core/blocking_pool.h"
#include "core/deferred_call.h"
#include "core/lazy_library.h"
#include "core/log.h"
#include "protocols/async_file_writer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>

// clang-format off
#define LIBCURL_FUNCTIONS(X) \
  X(curl_easy_cleanup) X(curl_easy_getinfo) X(curl_easy_init) X(curl_easy_pause) X(curl_easy_setopt) \
  X(curl_easy_strerror) \
  X(curl_global_cleanup) X(curl_global_init) X(curl_multi_add_handle) X(curl_multi_cleanup) X(curl_multi_fdset) \
  X(curl_multi_info_read) X(curl_multi_init) X(curl_multi_perform) X(curl_multi_remove_handle) \
  X(curl_multi_strerror) X(curl_multi_timeout) X(curl_slist_append) X(curl_slist_free_all)
// clang-format on
LAZY_LIBRARY(LibcurlApi, libcurl, LAZY_SONAME_CURL, LIBCURL_FUNCTIONS)

namespace {
  constexpr Logger kLog("http");
  constexpr float kSlowCurlOperationDebugMs = 50.0F;
  constexpr float kSlowCurlOperationWarnMs = 1000.0F;
  constexpr float kCurlServiceGapWarnMs = 5000.0F;
  // Long enough for chained requests (catalog, then its files) to reuse one connection.
  constexpr auto kIdleConnectionLinger = std::chrono::seconds(10);
  constexpr const char* kUserAgent = "noctalia/" NOCTALIA_VERSION;

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  std::string urlForLog(std::string_view url) {
    const std::size_t redactedPos = url.find_first_of("?#");
    if (redactedPos != std::string_view::npos) {
      const char suffix = url[redactedPos] == '?' ? '?' : '#';
      return std::string(url.substr(0, redactedPos))
          + suffix
          + (suffix == '?' ? "<query redacted>" : "<fragment redacted>");
    }

    return std::string(url);
  }

  void deferFailure(HttpClient::CompletionCallback cb) {
    if (!cb) {
      return;
    }
    DeferredCall::callLater([cb = std::move(cb)] { cb(false); });
  }

  void deferFailures(std::vector<HttpClient::CompletionCallback> callbacks) {
    if (callbacks.empty()) {
      return;
    }
    DeferredCall::callLater([callbacks = std::move(callbacks)] mutable {
      for (auto& callback : callbacks) {
        if (callback) {
          callback(false);
        }
      }
    });
  }

  void deferResponse(HttpClient::ResponseCallback cb) {
    if (!cb) {
      return;
    }
    DeferredCall::callLater([cb = std::move(cb)] mutable { cb(HttpResponse{}); });
  }

  void deferStreamClose(HttpClient::StreamCloseCallback cb) {
    if (!cb) {
      return;
    }
    DeferredCall::callLater([cb = std::move(cb)] mutable { cb(HttpStreamResult{}); });
  }

  // A download hands its body to the file writer in pieces of this size.
  constexpr std::size_t kDownloadChunkBytes = 256 * 1024;
  // Download bytes held in memory while the disk lags: above the first the transfer pauses, below the second it
  // resumes.
  constexpr std::size_t kDownloadPauseBytes = 4 * 1024 * 1024;
  constexpr std::size_t kDownloadResumeBytes = 1024 * 1024;

  std::size_t captureResponse(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t bytes = size * nmemb;
    if (userdata == nullptr || ptr == nullptr || bytes == 0) {
      return bytes;
    }

    auto* response = static_cast<std::string*>(userdata);
    response->append(ptr, bytes);
    return bytes;
  }

  std::size_t captureHeaders(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t bytes = size * nmemb;
    auto& headers = *static_cast<std::unordered_map<std::string, std::string>*>(userdata);
    const std::string_view line(ptr, bytes);
    if (line.starts_with("HTTP/")) {
      headers.clear();
    } else if (const auto colon = line.find(':'); colon != std::string_view::npos) {
      std::string name(line.substr(0, colon));
      for (auto& ch : name) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      }
      auto value = line.substr(colon + 1);
      const auto first = value.find_first_not_of(" \t\r\n");
      if (first == std::string_view::npos) {
        value = {};
      } else {
        value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
      }
      headers.insert_or_assign(std::move(name), std::string(value));
    }
    return bytes;
  }

  std::size_t forwardStreamChunk(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t bytes = size * nmemb;
    if (userdata == nullptr || ptr == nullptr || bytes == 0) {
      return bytes;
    }

    auto* onData = static_cast<HttpClient::StreamDataCallback*>(userdata);
    if (*onData) {
      (*onData)(std::string_view(ptr, bytes));
    }
    return bytes;
  }
} // namespace

void HttpClient::applyCommonOptions(CURL* easy) const {
  libcurl().curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  libcurl().curl_easy_setopt(easy, CURLOPT_USERAGENT, kUserAgent);
  if (!m_caBundlePath.empty()) {
    libcurl().curl_easy_setopt(easy, CURLOPT_CAINFO, m_caBundlePath.c_str());
  }
  // With the threaded resolver, curl reports a DNS timeout while its getaddrinfo() thread is
  // still blocked. Without this, curl_easy_cleanup() joins that thread and stalls the main loop
  // for as long as the resolver takes. CURLOPT_QUICK_EXIT detaches it instead: the abandoned
  // thread frees itself once the lookup returns.
  libcurl().curl_easy_setopt(easy, CURLOPT_QUICK_EXIT, 1L);
}

void HttpClient::applyTlsClientCert(CURL* easy, const std::shared_ptr<const HttpTlsClientCert>& cert) {
  if (cert == nullptr) {
    return;
  }
  // curl copies both strings, so the request does not have to outlive the transfer.
  libcurl().curl_easy_setopt(easy, CURLOPT_SSLCERT, cert->clientCertPath.c_str());
  libcurl().curl_easy_setopt(easy, CURLOPT_SSLKEY, cert->clientKeyPath.c_str());
}

// Writer-thread state of one download's `.part` file; only file-writer operations touch it.
struct HttpClient::DownloadPart {
  std::filesystem::path path;
  int fd = -1;
  bool failed = false;

  explicit DownloadPart(std::filesystem::path partPath) : path(std::move(partPath)) {}
  DownloadPart(const DownloadPart&) = delete;
  DownloadPart& operator=(const DownloadPart&) = delete;
  ~DownloadPart() { closeFd(); }

  void closeFd() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  bool open() {
    if (fd >= 0 || failed) {
      return !failed;
    }
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    failed = fd < 0;
    return !failed;
  }

  // No size limit: a download fails instead when it would leave less than min(1 GiB, 5%) of its filesystem free.
  [[nodiscard]] bool leavesDiskRoom(std::size_t bytes) const {
    struct statvfs fs{};
    if (::fstatvfs(fd, &fs) != 0) {
      return true;
    }
    const auto unit = static_cast<std::uint64_t>(fs.f_frsize);
    const std::uint64_t floor = std::min<std::uint64_t>(1ULL << 30U, fs.f_blocks * unit / 20);
    return static_cast<std::uint64_t>(fs.f_bavail) * unit >= floor + bytes;
  }

  bool append(const std::string& bytes) {
    if (!open()) {
      return false;
    }
    if (!leavesDiskRoom(bytes.size())) {
      kLog.warn("download stopped: the disk is almost full at {}", path.string());
      failed = true;
      return false;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        kLog.warn("download failed to write {}", path.string());
        failed = true;
        return false;
      }
      done += static_cast<std::size_t>(n);
    }
    return true;
  }

  void discard() {
    closeFd();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  // Moves the finished file to `dest`; an empty body still makes an empty file.
  bool finish(const std::filesystem::path& dest) {
    if (!open()) {
      discard();
      return false;
    }
    closeFd();
    std::error_code ec;
    std::filesystem::rename(path, dest, ec);
    if (ec) {
      std::filesystem::copy_file(path, dest, std::filesystem::copy_options::overwrite_existing, ec);
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      if (ec) {
        kLog.warn("download failed to move {} to {}: {}", path.string(), dest.string(), ec.message());
        return false;
      }
    }
    return true;
  }
};

HttpClient::HttpClient(std::filesystem::path caBundlePath) : m_caBundlePath(std::move(caBundlePath)) {}

HttpClient::~HttpClient() {
  if (m_multi == nullptr) {
    return;
  }
  for (auto& [easy, transfer] : m_transfers) {
    libcurl().curl_multi_remove_handle(m_multi, easy);
    libcurl().curl_easy_cleanup(easy);
    andromeda::runFileOpAsync([part = transfer.part] {
      part->discard();
      return true;
    });
  }
  for (auto& [easy, post] : m_postTransfers) {
    libcurl().curl_multi_remove_handle(m_multi, easy);
    libcurl().curl_easy_cleanup(easy);
    if (post.headers != nullptr) {
      libcurl().curl_slist_free_all(post.headers);
    }
  }
  for (auto& [easy, req] : m_requestTransfers) {
    libcurl().curl_multi_remove_handle(m_multi, easy);
    libcurl().curl_easy_cleanup(easy);
    if (req.headers != nullptr) {
      libcurl().curl_slist_free_all(req.headers);
    }
  }
  for (auto& [easy, stream] : m_streamTransfers) {
    libcurl().curl_multi_remove_handle(m_multi, easy);
    libcurl().curl_easy_cleanup(easy);
    if (stream.headers != nullptr) {
      libcurl().curl_slist_free_all(stream.headers);
    }
  }
  // In-flight handles are closed without waiting for their resolver threads (CURLOPT_QUICK_EXIT).
  // curl_global_cleanup() does not wait for them either; an abandoned thread only frees its own
  // resolver state, so it has nothing left to race here.
  libcurl().curl_multi_cleanup(m_multi);
  libcurl().curl_global_cleanup();
}

CURL* HttpClient::newEasy() {
  if (m_multi == nullptr) {
    libcurl().curl_global_init(CURL_GLOBAL_DEFAULT);
    m_multi = libcurl().curl_multi_init();
  }
  return libcurl().curl_easy_init();
}

bool HttpClient::hasActiveTransfers() const {
  return !m_transfers.empty() || !m_postTransfers.empty() || !m_requestTransfers.empty() || !m_streamTransfers.empty();
}

void HttpClient::download(std::string_view url, const std::filesystem::path& destPath, CompletionCallback cb) {
  if (m_offlineMode) {
    kLog.warn("download skipped in offline mode url={}", urlForLog(url));
    deferFailure(std::move(cb));
    return;
  }

  const std::string destKey = destPath.string();
  if (auto activeIt = m_activeByDest.find(destKey); activeIt != m_activeByDest.end()) {
    if (auto transferIt = m_transfers.find(activeIt->second); transferIt != m_transfers.end()) {
      transferIt->second.callbacks.push_back(std::move(cb));
      return;
    }
    m_activeByDest.erase(activeIt);
  }
  if (auto writing = m_writing.find(destKey); writing != m_writing.end()) {
    writing->second.push_back(std::move(cb));
    return;
  }

  CURL* easy = newEasy();
  if (easy == nullptr) {
    kLog.warn("download failed to create curl handle url={}", urlForLog(url));
    deferFailure(std::move(cb));
    return;
  }

  const std::string urlStr(url);
  applyCommonOptions(easy);
  libcurl().curl_easy_setopt(easy, CURLOPT_URL, urlStr.c_str());
  libcurl().curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, captureDownload);
  libcurl().curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
  libcurl().curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L);
  // A stalled server fails the download, not a large file: under 1 KiB/s for 30 s aborts. Paused time is excluded.
  libcurl().curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  libcurl().curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 30L);
  libcurl().curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);

  Transfer transfer{};
  transfer.client = this;
  transfer.easy = easy;
  transfer.id = m_nextTransferId++;
  transfer.destPath = destPath;
  transfer.part = std::make_shared<DownloadPart>(destPath.parent_path() / (destPath.filename().string() + ".part"));
  transfer.callbacks.push_back(std::move(cb));
  transfer.destKey = destKey;
  transfer.url = urlStr;
  m_transfers[easy] = std::move(transfer);
  libcurl().curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, m_transfers[easy].errorBuffer.data());
  libcurl().curl_easy_setopt(easy, CURLOPT_WRITEDATA, &m_transfers[easy]);
  const CURLMcode addResult = libcurl().curl_multi_add_handle(m_multi, easy);
  if (addResult != CURLM_OK) {
    Transfer failedTransfer = std::move(m_transfers[easy]);
    m_transfers.erase(easy);
    libcurl().curl_easy_cleanup(easy);
    kLog.warn(
        "download failed to add curl handle url={} error={}", urlForLog(urlStr),
        libcurl().curl_multi_strerror(addResult)
    );
    deferFailures(std::move(failedTransfer.callbacks));
    return;
  }
  m_activeByDest[destKey] = easy;
  performMulti("download start");
}

void HttpClient::download(std::vector<std::string> urls, const std::filesystem::path& destPath, CompletionCallback cb) {
  if (urls.empty()) {
    deferFailure(std::move(cb));
    return;
  }
  auto state = std::make_shared<SequentialDownload>();
  state->urls = std::move(urls);
  state->destPath = destPath;
  state->callback = std::move(cb);
  downloadSequential(state);
}

void HttpClient::downloadSequential(const std::shared_ptr<SequentialDownload>& state) {
  const std::size_t index = state->index;
  // The captured shared state keeps urls/dest/callback alive across async hops;
  // advance to the next candidate only when the current one fails.
  download(state->urls[index], state->destPath, [this, state](bool success) {
    if (success || state->index + 1 >= state->urls.size()) {
      state->callback(success);
      return;
    }
    state->index += 1;
    downloadSequential(state);
  });
}

void HttpClient::post(std::string_view url, std::string body, std::string_view contentType, CompletionCallback cb) {
  if (m_offlineMode) {
    kLog.warn("post skipped in offline mode url={}", urlForLog(url));
    deferFailure(std::move(cb));
    return;
  }

  CURL* easy = newEasy();
  if (easy == nullptr) {
    kLog.warn("post failed to create curl handle url={}", urlForLog(url));
    deferFailure(std::move(cb));
    return;
  }

  PostTransfer post{};
  post.body = std::move(body);
  post.callback = std::move(cb);

  const std::string header = "Content-Type: " + std::string(contentType);
  post.headers = libcurl().curl_slist_append(nullptr, header.c_str());

  const std::string urlStr(url);
  post.url = urlStr;
  applyCommonOptions(easy);
  libcurl().curl_easy_setopt(easy, CURLOPT_URL, urlStr.c_str());
  libcurl().curl_easy_setopt(easy, CURLOPT_POST, 1L);
  libcurl().curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(post.body.size()));
  libcurl().curl_easy_setopt(easy, CURLOPT_POSTFIELDS, post.body.c_str());
  libcurl().curl_easy_setopt(easy, CURLOPT_HTTPHEADER, post.headers);
  libcurl().curl_easy_setopt(easy, CURLOPT_TIMEOUT, 10L);
  libcurl().curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
  libcurl().curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L);

  m_postTransfers[easy] = std::move(post);
  libcurl().curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, m_postTransfers[easy].errorBuffer.data());
  libcurl().curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, captureResponse);
  libcurl().curl_easy_setopt(easy, CURLOPT_WRITEDATA, &m_postTransfers[easy].response);
  const CURLMcode addResult = libcurl().curl_multi_add_handle(m_multi, easy);
  if (addResult != CURLM_OK) {
    PostTransfer failedPost = std::move(m_postTransfers[easy]);
    m_postTransfers.erase(easy);
    libcurl().curl_easy_cleanup(easy);
    if (failedPost.headers != nullptr) {
      libcurl().curl_slist_free_all(failedPost.headers);
    }
    kLog.warn(
        "post failed to add curl handle url={} error={}", urlForLog(urlStr), libcurl().curl_multi_strerror(addResult)
    );
    deferFailure(std::move(failedPost.callback));
    return;
  }
  performMulti("post start");
}

void HttpClient::request(HttpRequest req, ResponseCallback cb) {
  if (m_offlineMode) {
    kLog.warn("request skipped in offline mode url={}", urlForLog(req.url));
    deferResponse(std::move(cb));
    return;
  }

  CURL* easy = newEasy();
  if (easy == nullptr) {
    kLog.warn("request failed to create curl handle url={}", urlForLog(req.url));
    deferResponse(std::move(cb));
    return;
  }

  RequestTransfer transfer{};
  transfer.callback = std::move(cb);
  transfer.url = req.url;
  transfer.body = std::move(req.body);
  transfer.basicUsername = std::move(req.basicUsername);
  transfer.basicPassword = std::move(req.basicPassword);
  for (const auto& header : req.headers) {
    transfer.headers = libcurl().curl_slist_append(transfer.headers, header.c_str());
  }

  m_requestTransfers[easy] = std::move(transfer);
  RequestTransfer& stored = m_requestTransfers[easy];

  applyCommonOptions(easy);
  libcurl().curl_easy_setopt(easy, CURLOPT_URL, stored.url.c_str());
  libcurl().curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
  libcurl().curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
  if (req.allowInsecureTls) {
    libcurl().curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    libcurl().curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  applyTlsClientCert(easy, req.tlsClientCert);
  if (req.followRedirects) {
    libcurl().curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    if (req.allowRedirectAuth) {
      libcurl().curl_easy_setopt(easy, CURLOPT_UNRESTRICTED_AUTH, 1L);
    }
  }
  if (req.freshConnection) {
    libcurl().curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, 1L);
    libcurl().curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);
  }
  if (!stored.basicUsername.empty() || !stored.basicPassword.empty()) {
    libcurl().curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    libcurl().curl_easy_setopt(easy, CURLOPT_USERNAME, stored.basicUsername.c_str());
    libcurl().curl_easy_setopt(easy, CURLOPT_PASSWORD, stored.basicPassword.c_str());
  }
  // No CURLOPT_FAILONERROR: callers need the status and body for non-2xx responses.
  if (req.method == "POST") {
    libcurl().curl_easy_setopt(easy, CURLOPT_POST, 1L);
  } else if (req.method != "GET") {
    libcurl().curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, req.method.c_str());
  }
  if (stored.headers != nullptr) {
    libcurl().curl_easy_setopt(easy, CURLOPT_HTTPHEADER, stored.headers);
  }
  if (!stored.body.empty()) {
    libcurl().curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(stored.body.size()));
    libcurl().curl_easy_setopt(easy, CURLOPT_POSTFIELDS, stored.body.c_str());
  }
  libcurl().curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, stored.errorBuffer.data());
  libcurl().curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, captureResponse);
  libcurl().curl_easy_setopt(easy, CURLOPT_WRITEDATA, &stored.response);
  libcurl().curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, captureHeaders);
  libcurl().curl_easy_setopt(easy, CURLOPT_HEADERDATA, &stored.responseHeaders);

  const CURLMcode addResult = libcurl().curl_multi_add_handle(m_multi, easy);
  if (addResult != CURLM_OK) {
    RequestTransfer failed = std::move(m_requestTransfers[easy]);
    m_requestTransfers.erase(easy);
    libcurl().curl_easy_cleanup(easy);
    if (failed.headers != nullptr) {
      libcurl().curl_slist_free_all(failed.headers);
    }
    kLog.warn(
        "request failed to add curl handle url={} error={}", urlForLog(failed.url),
        libcurl().curl_multi_strerror(addResult)
    );
    deferResponse(std::move(failed.callback));
    return;
  }
  performMulti("request start");
}

HttpClient::StreamId HttpClient::startStream(HttpRequest req, StreamDataCallback onData, StreamCloseCallback onClose) {
  if (m_offlineMode) {
    kLog.warn("stream skipped in offline mode url={}", urlForLog(req.url));
    deferStreamClose(std::move(onClose));
    return 0;
  }

  CURL* easy = newEasy();
  if (easy == nullptr) {
    kLog.warn("stream failed to create curl handle url={}", urlForLog(req.url));
    deferStreamClose(std::move(onClose));
    return 0;
  }

  StreamTransfer transfer{};
  transfer.id = m_nextStreamId++;
  transfer.onData = std::move(onData);
  transfer.onClose = std::move(onClose);
  transfer.url = req.url;
  transfer.body = std::move(req.body);
  transfer.basicUsername = std::move(req.basicUsername);
  transfer.basicPassword = std::move(req.basicPassword);
  for (const auto& header : req.headers) {
    transfer.headers = libcurl().curl_slist_append(transfer.headers, header.c_str());
  }

  m_streamTransfers[easy] = std::move(transfer);
  StreamTransfer& stored = m_streamTransfers[easy];
  m_streamsById[stored.id] = easy;

  applyCommonOptions(easy);
  libcurl().curl_easy_setopt(easy, CURLOPT_URL, stored.url.c_str());
  // No CURLOPT_TIMEOUT: the transfer is expected to stay open indefinitely.
  libcurl().curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
  if (req.allowInsecureTls) {
    libcurl().curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    libcurl().curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  applyTlsClientCert(easy, req.tlsClientCert);
  if (req.followRedirects) {
    libcurl().curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    if (req.allowRedirectAuth) {
      libcurl().curl_easy_setopt(easy, CURLOPT_UNRESTRICTED_AUTH, 1L);
    }
  }
  if (req.freshConnection) {
    libcurl().curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, 1L);
    libcurl().curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);
  }
  if (!stored.basicUsername.empty() || !stored.basicPassword.empty()) {
    libcurl().curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    libcurl().curl_easy_setopt(easy, CURLOPT_USERNAME, stored.basicUsername.c_str());
    libcurl().curl_easy_setopt(easy, CURLOPT_PASSWORD, stored.basicPassword.c_str());
  }
  // No CURLOPT_FAILONERROR: error responses stream their body and report status on close.
  if (req.method == "POST") {
    libcurl().curl_easy_setopt(easy, CURLOPT_POST, 1L);
  } else if (req.method != "GET") {
    libcurl().curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, req.method.c_str());
  }
  if (stored.headers != nullptr) {
    libcurl().curl_easy_setopt(easy, CURLOPT_HTTPHEADER, stored.headers);
  }
  if (!stored.body.empty()) {
    libcurl().curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(stored.body.size()));
    libcurl().curl_easy_setopt(easy, CURLOPT_POSTFIELDS, stored.body.c_str());
  }
  libcurl().curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, stored.errorBuffer.data());
  libcurl().curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, forwardStreamChunk);
  libcurl().curl_easy_setopt(easy, CURLOPT_WRITEDATA, &stored.onData);

  const CURLMcode addResult = libcurl().curl_multi_add_handle(m_multi, easy);
  if (addResult != CURLM_OK) {
    StreamTransfer failed = std::move(m_streamTransfers[easy]);
    m_streamsById.erase(failed.id);
    m_streamTransfers.erase(easy);
    libcurl().curl_easy_cleanup(easy);
    if (failed.headers != nullptr) {
      libcurl().curl_slist_free_all(failed.headers);
    }
    kLog.warn(
        "stream failed to add curl handle url={} error={}", urlForLog(failed.url),
        libcurl().curl_multi_strerror(addResult)
    );
    deferStreamClose(std::move(failed.onClose));
    return 0;
  }
  const StreamId id = stored.id;
  performMulti("stream start");
  return id;
}

void HttpClient::cancelStream(StreamId id) {
  const auto byId = m_streamsById.find(id);
  if (byId == m_streamsById.end()) {
    return;
  }
  CURL* easy = byId->second;
  m_streamsById.erase(byId);

  const auto it = m_streamTransfers.find(easy);
  if (it != m_streamTransfers.end()) {
    if (it->second.headers != nullptr) {
      libcurl().curl_slist_free_all(it->second.headers);
    }
    m_streamTransfers.erase(it);
  }
  libcurl().curl_multi_remove_handle(m_multi, easy);
  libcurl().curl_easy_cleanup(easy);
}

void HttpClient::addPollFds(std::vector<pollfd>& fds) {
  if (!hasActiveTransfers()) {
    return;
  }

  fd_set readfds, writefds, errfds;
  FD_ZERO(&readfds);
  FD_ZERO(&writefds);
  FD_ZERO(&errfds);
  int maxfd = -1;
  libcurl().curl_multi_fdset(m_multi, &readfds, &writefds, &errfds, &maxfd);

  for (int fd = 0; fd <= maxfd; ++fd) {
    short events = 0;
    if (FD_ISSET(fd, &readfds)) {
      events |= POLLIN;
    }
    if (FD_ISSET(fd, &writefds)) {
      events |= POLLOUT;
    }
    if (events != 0) {
      fds.push_back({fd, events, 0});
    }
  }
}

int HttpClient::timeoutMs() const {
  if (!hasActiveTransfers()) {
    if (m_idleSince == std::chrono::steady_clock::time_point{}) {
      return -1;
    }
    const auto left = kIdleConnectionLinger - (std::chrono::steady_clock::now() - m_idleSince);
    return static_cast<int>(std::max<std::int64_t>(0, std::chrono::ceil<std::chrono::milliseconds>(left).count()));
  }
  long timeout = -1;
  libcurl().curl_multi_timeout(m_multi, &timeout);
  // curl_multi_timeout can report a deadline up to the per-transfer timeout, but
  // curl needs servicing at least once per second to advance its timeout
  // handling, so cap the wait while transfers are active.
  if (timeout < 0 || timeout > 1000) {
    return 1000;
  }
  return static_cast<int>(timeout);
}

void HttpClient::dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) {
  if (!hasActiveTransfers()) {
    // Idle cached connections are never polled, so a server's close leaves them in CLOSE-WAIT and keep-alives
    // keep the radio busy: a fresh multi handle closes them all.
    if (m_idleSince != std::chrono::steady_clock::time_point{}
        && std::chrono::steady_clock::now() - m_idleSince >= kIdleConnectionLinger) {
      m_idleSince = {};
      libcurl().curl_multi_cleanup(m_multi);
      m_multi = libcurl().curl_multi_init();
    }
    return;
  }

  performMulti("poll dispatch");

  CURLMsg* msg = nullptr;
  int msgsLeft = 0;
  while ((msg = libcurl().curl_multi_info_read(m_multi, &msgsLeft)) != nullptr) {
    if (msg->msg == CURLMSG_DONE) {
      CURL* easy = msg->easy_handle;
      if (m_postTransfers.contains(easy)) {
        finishPostTransfer(easy, msg->data.result);
      } else if (m_requestTransfers.contains(easy)) {
        finishRequestTransfer(easy, msg->data.result);
      } else if (m_streamTransfers.contains(easy)) {
        finishStreamTransfer(easy, msg->data.result);
      } else {
        finishTransfer(easy, msg->data.result);
      }
    }
  }

  if (!hasActiveTransfers()) {
    m_lastServiceAt = {};
    m_idleSince = std::chrono::steady_clock::now();
  }
}

void HttpClient::performMulti(const char* reason) {
  const auto now = std::chrono::steady_clock::now();
  if (m_lastServiceAt != std::chrono::steady_clock::time_point{}) {
    const float serviceGapMs = std::chrono::duration<float, std::milli>(now - m_lastServiceAt).count();
    if (serviceGapMs >= kCurlServiceGapWarnMs) {
      kLog.warn(
          "http client was not serviced for {:.1F}ms before {} (downloads={} posts={} running={})", serviceGapMs,
          reason, m_transfers.size(), m_postTransfers.size(), m_running
      );
    }
  }

  const auto opStart = std::chrono::steady_clock::now();
  libcurl().curl_multi_perform(m_multi, &m_running);
  const float ms = elapsedSince(opStart);
  if (ms >= kSlowCurlOperationWarnMs) {
    kLog.warn("curl_multi_perform took {:.1F}ms during {}", ms, reason);
  } else if (ms >= kSlowCurlOperationDebugMs) {
    kLog.debug("curl_multi_perform took {:.1F}ms during {}", ms, reason);
  }
  m_lastServiceAt = std::chrono::steady_clock::now();
}

void HttpClient::finishTransfer(CURL* easy, CURLcode result) {
  auto it = m_transfers.find(easy);
  if (it == m_transfers.end()) {
    libcurl().curl_multi_remove_handle(m_multi, easy);
    libcurl().curl_easy_cleanup(easy);
    return;
  }

  long responseCode = 0;
  libcurl().curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &responseCode);

  // Destroy the handle before the state it still points at (error buffer, write target).
  libcurl().curl_multi_remove_handle(m_multi, easy);
  libcurl().curl_easy_cleanup(easy);

  Transfer transfer = std::move(it->second);
  m_transfers.erase(it);
  m_activeByDest.erase(transfer.destKey);

  if (result != CURLE_OK) {
    const char* detail =
        transfer.errorBuffer[0] != '\0' ? transfer.errorBuffer.data() : libcurl().curl_easy_strerror(result);
    kLog.warn(
        "download failed url={} curl={} http={} error={}", urlForLog(transfer.url), static_cast<int>(result),
        responseCode, detail
    );
    andromeda::runFileOpAsync([part = std::move(transfer.part)] {
      part->discard();
      return true;
    });
    for (auto& callback : transfer.callbacks) {
      callback(false);
    }
    return;
  }

  // The rename runs after every queued piece; under disk load each write blocks for 100s of ms.
  flushDownloadChunk(transfer);
  m_writing[transfer.destKey] = std::move(transfer.callbacks);
  andromeda::runFileOpAsync(
      [part = std::move(transfer.part), destPath = std::move(transfer.destPath)] { return part->finish(destPath); },
      [this, alive = std::weak_ptr<int>(m_alive), destKey = std::move(transfer.destKey)](bool written) {
        if (alive.expired()) {
          return;
        }
        auto node = m_writing.extract(destKey);
        for (auto& callback : node.mapped()) {
          callback(written);
        }
      }
  );
}

std::size_t HttpClient::captureDownload(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  const std::size_t bytes = size * nmemb;
  auto* transfer = static_cast<Transfer*>(userdata);
  if (transfer->writeFailed) {
    return CURL_WRITEFUNC_ERROR;
  }
  if (transfer->queuedBytes >= kDownloadPauseBytes) {
    transfer->paused = true;
    return CURL_WRITEFUNC_PAUSE;
  }
  transfer->chunk.append(ptr, bytes);
  if (transfer->chunk.size() >= kDownloadChunkBytes) {
    transfer->client->flushDownloadChunk(*transfer);
  }
  return bytes;
}

void HttpClient::flushDownloadChunk(Transfer& transfer) {
  if (transfer.chunk.empty()) {
    return;
  }
  const std::size_t bytes = transfer.chunk.size();
  transfer.queuedBytes += bytes;
  andromeda::runFileOpAsync(
      [part = transfer.part, chunk = std::exchange(transfer.chunk, {})] { return part->append(chunk); },
      [this, alive = std::weak_ptr<int>(m_alive), id = transfer.id, bytes](bool ok) {
        if (!alive.expired()) {
          onDownloadChunkWritten(id, bytes, ok);
        }
      }
  );
}

void HttpClient::onDownloadChunkWritten(std::uint64_t id, std::size_t bytes, bool ok) {
  const auto it = std::ranges::find_if(m_transfers, [id](const auto& entry) { return entry.second.id == id; });
  if (it == m_transfers.end()) {
    return;
  }
  Transfer& transfer = it->second;
  transfer.queuedBytes -= bytes;
  transfer.writeFailed = transfer.writeFailed || !ok;
  if (transfer.paused && (transfer.writeFailed || transfer.queuedBytes < kDownloadResumeBytes)) {
    transfer.paused = false;
    // An unpause can deliver held data to captureDownload before it returns; a failed write then aborts the transfer.
    libcurl().curl_easy_pause(transfer.easy, CURLPAUSE_CONT);
    performMulti("download resume");
  }
}

void HttpClient::finishPostTransfer(CURL* easy, CURLcode result) {
  auto it = m_postTransfers.find(easy);
  if (it == m_postTransfers.end()) {
    libcurl().curl_multi_remove_handle(m_multi, easy);
    libcurl().curl_easy_cleanup(easy);
    return;
  }

  long responseCode = 0;
  libcurl().curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &responseCode);

  libcurl().curl_multi_remove_handle(m_multi, easy);
  libcurl().curl_easy_cleanup(easy);

  PostTransfer post = std::move(it->second);
  m_postTransfers.erase(it);

  if (post.headers != nullptr) {
    libcurl().curl_slist_free_all(post.headers);
  }

  const bool success = result == CURLE_OK;
  if (!success) {
    const char* detail = post.errorBuffer[0] != '\0' ? post.errorBuffer.data() : libcurl().curl_easy_strerror(result);
    kLog.warn(
        "post failed url={} curl={} http={} error={}", urlForLog(post.url), static_cast<int>(result), responseCode,
        detail
    );
  }

  if (post.callback) {
    post.callback(success);
  }
}

void HttpClient::finishRequestTransfer(CURL* easy, CURLcode result) {
  auto it = m_requestTransfers.find(easy);
  if (it == m_requestTransfers.end()) {
    libcurl().curl_multi_remove_handle(m_multi, easy);
    libcurl().curl_easy_cleanup(easy);
    return;
  }

  long responseCode = 0;
  libcurl().curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &responseCode);
  std::string effectiveUrl;
  if (char* value = nullptr; libcurl().curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &value) == CURLE_OK
      && value != nullptr
      && value[0] != '\0') {
    effectiveUrl = value;
  }

  libcurl().curl_multi_remove_handle(m_multi, easy);
  libcurl().curl_easy_cleanup(easy);

  RequestTransfer transfer = std::move(it->second);
  m_requestTransfers.erase(it);

  if (transfer.headers != nullptr) {
    libcurl().curl_slist_free_all(transfer.headers);
  }

  HttpResponse response;
  response.transportOk = result == CURLE_OK;
  response.timedOut = result == CURLE_OPERATION_TIMEDOUT;
  response.status = responseCode;
  response.body = std::move(transfer.response);
  response.headers = std::move(transfer.responseHeaders);
  response.effectiveUrl = std::move(effectiveUrl);
  if (!response.transportOk) {
    const char* detail =
        transfer.errorBuffer[0] != '\0' ? transfer.errorBuffer.data() : libcurl().curl_easy_strerror(result);
    kLog.warn(
        "request failed url={} curl={} http={} error={}", urlForLog(transfer.url), static_cast<int>(result),
        responseCode, detail
    );
  }

  if (transfer.callback) {
    transfer.callback(std::move(response));
  }
}

void HttpClient::finishStreamTransfer(CURL* easy, CURLcode result) {
  auto it = m_streamTransfers.find(easy);
  if (it == m_streamTransfers.end()) {
    libcurl().curl_multi_remove_handle(m_multi, easy);
    libcurl().curl_easy_cleanup(easy);
    return;
  }

  long responseCode = 0;
  libcurl().curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &responseCode);

  libcurl().curl_multi_remove_handle(m_multi, easy);
  libcurl().curl_easy_cleanup(easy);

  StreamTransfer transfer = std::move(it->second);
  m_streamTransfers.erase(it);
  m_streamsById.erase(transfer.id);

  if (transfer.headers != nullptr) {
    libcurl().curl_slist_free_all(transfer.headers);
  }

  HttpStreamResult streamResult;
  streamResult.transportOk = result == CURLE_OK;
  streamResult.status = responseCode;
  if (!streamResult.transportOk) {
    const char* detail =
        transfer.errorBuffer[0] != '\0' ? transfer.errorBuffer.data() : libcurl().curl_easy_strerror(result);
    kLog.warn(
        "stream ended url={} curl={} http={} error={}", urlForLog(transfer.url), static_cast<int>(result), responseCode,
        detail
    );
  }

  if (transfer.onClose) {
    transfer.onClose(streamResult);
  }
}
