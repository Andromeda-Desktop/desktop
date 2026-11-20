#include "mirror_decoder.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace linkapp {

  namespace {

    constexpr Logger kLog("link-mirror");
    constexpr std::size_t kHeader = 13;          // u32 length, u64 time, u8 flags
    constexpr std::uint32_t kMaxUnit = 4U << 20; // link-mirror's MAX_UNIT
    constexpr std::uint8_t kAudio = 4;
    constexpr std::uint8_t kEnd = 8;

    // Reads exactly `size` bytes; false at the end of the stream or on an error.
    bool readAll(int fd, std::uint8_t* data, std::size_t size) {
      while (size > 0) {
        const ssize_t n = ::read(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= static_cast<std::size_t>(n);
      }
      return true;
    }

    AVPixelFormat pickFormat(AVCodecContext* ctx, const AVPixelFormat* formats) {
      for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_VAAPI && ctx->hw_device_ctx != nullptr) return *f;
      }
      for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if ((av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) return *f;
      }
      return AV_PIX_FMT_NONE;
    }

  } // namespace

  MirrorDecoder::MirrorDecoder(int fd, std::function<void(Frame)> frame, std::function<void(std::string)> ended)
      : m_fd(fd), m_frame(std::move(frame)), m_ended(std::move(ended)), m_thread([this] { run(); }) {}

  MirrorDecoder::~MirrorDecoder() {
    ::shutdown(m_fd, SHUT_RDWR); // wakes the worker's read
    m_thread.join();
    ::close(m_fd);
  }

  void MirrorDecoder::setTarget(int width, int height) {
    m_targetW = width;
    m_targetH = height;
  }

  void MirrorDecoder::run() {
    const auto end = [this](std::string why) {
      DeferredCall::callLater([this, why = std::move(why), alive = std::weak_ptr<bool>(m_alive)]() mutable {
        if (!alive.expired()) m_ended(std::move(why));
      });
    };
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* ctx = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
    if (ctx == nullptr) {
      end("This desktop has no H.264 decoder.");
      return;
    }
    if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) {
      kLog.info("mirror: no VA-API device, decoding in software");
    }
    ctx->get_format = pickFormat;
    ctx->thread_count = 0;
    AVPacket* packet = av_packet_alloc();
    AVFrame* decoded = av_frame_alloc();
    AVFrame* mapped = av_frame_alloc();
    SwsContext* scale = nullptr;
    std::string why;
    if (avcodec_open2(ctx, codec, nullptr) < 0) why = "The H.264 decoder did not start.";
    std::vector<std::uint8_t> unit;
    int frames = 0;
    auto second = std::chrono::steady_clock::now();
    while (why.empty()) {
      std::array<std::uint8_t, kHeader> header{};
      if (!readAll(m_fd, header.data(), header.size())) break;
      const std::uint32_t length = (std::uint32_t{header[0]} << 24) | (std::uint32_t{header[1]} << 16) |
                                   (std::uint32_t{header[2]} << 8) | header[3];
      const std::uint8_t flags = header[12];
      if (length > kMaxUnit) {
        why = "The phone sent a broken video stream.";
        break;
      }
      unit.resize(length + AV_INPUT_BUFFER_PADDING_SIZE);
      if (!readAll(m_fd, unit.data(), length)) break;
      if ((flags & kEnd) != 0) break;
      if ((flags & kAudio) != 0) continue;
      packet->data = unit.data();
      packet->size = static_cast<int>(length);
      if (avcodec_send_packet(ctx, packet) < 0) continue;
      while (avcodec_receive_frame(ctx, decoded) == 0) {
        ++frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - second >= std::chrono::seconds(1)) {
          m_fps = frames;
          frames = 0;
          second = now;
        }
        if (m_posted.load()) continue; // the UI has not taken the last frame: drop this one
        AVFrame* source = decoded;
        if (decoded->format == AV_PIX_FMT_VAAPI) {
          av_frame_unref(mapped);
          if (av_hwframe_transfer_data(mapped, decoded, 0) < 0) continue;
          source = mapped;
        }
        int w = m_targetW.load();
        int h = m_targetH.load();
        if (w <= 0 || h <= 0) {
          w = source->width;
          h = source->height;
        }
        scale = sws_getCachedContext(scale, source->width, source->height, static_cast<AVPixelFormat>(source->format), w, h,
                                     AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (scale == nullptr) continue;
        Frame frame{.rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4), .width = w,
                    .height = h};
        std::array<std::uint8_t*, 4> planes{frame.rgba.data(), nullptr, nullptr, nullptr};
        std::array<int, 4> strides{w * 4, 0, 0, 0};
        sws_scale(scale, source->data, source->linesize, 0, source->height, planes.data(), strides.data());
        m_posted = true;
        DeferredCall::callLater([this, frame = std::move(frame), alive = std::weak_ptr<bool>(m_alive)]() mutable {
          if (alive.expired()) return;
          m_posted = false;
          m_frame(std::move(frame));
        });
      }
    }
    sws_freeContext(scale);
    av_frame_free(&mapped);
    av_frame_free(&decoded);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    end(std::move(why));
  }

} // namespace linkapp
