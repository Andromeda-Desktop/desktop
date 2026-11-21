#include "shell/lockscreen/lock_video.h"

#include "capture/gstreamer_api.h"
#include "core/log.h"

#include <cstring>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("lock-video");

  void wake(int fd) {
    const std::uint64_t one = 1;
    (void)::write(fd, &one, sizeof(one));
  }

  GstFlowReturn onNewSample(GstAppSink* /*sink*/, gpointer data) {
    wake(*static_cast<int*>(data));
    return GST_FLOW_OK;
  }

  // vapostproc writes dmabufs only to a sink that reads per-buffer strides and offsets.
  gboolean onProposeAllocation(GstAppSink* /*sink*/, GstQuery* query, gpointer /*data*/) {
    gstreamer().gst_query_add_allocation_meta(query, gstVideo().gst_video_meta_api_get_type(), nullptr);
    return TRUE;
  }

  GstBusSyncReply onBusMessage(GstBus* /*bus*/, GstMessage* message, gpointer data) {
    const GstMessageType type = GST_MESSAGE_TYPE(message);
    if (type == GST_MESSAGE_EOS || type == GST_MESSAGE_ERROR) {
      wake(*static_cast<int*>(data));
    }
    return GST_BUS_PASS;
  }

  void linkDecoded(GstElement* /*decoder*/, GstPad* pad, gpointer next) {
    GstPad* sinkPad = gstreamer().gst_element_get_static_pad(static_cast<GstElement*>(next), "sink");
    if (!gstreamer().gst_pad_is_linked(sinkPad)) {
      GstCaps* caps = gstreamer().gst_pad_get_current_caps(pad);
      const bool video = caps == nullptr
          || g_str_has_prefix(
                             gstreamer().gst_structure_get_name(gstreamer().gst_caps_get_structure(caps, 0)), "video/"
          );
      if (caps != nullptr) {
        gstreamer().gst_caps_unref(caps);
      }
      if (video) {
        (void)gstreamer().gst_pad_link(pad, sinkPad);
      }
    }
    gstreamer().gst_object_unref(sinkPad);
  }

  // The first single-plane RGBA DRM format (with the driver's modifier) vapostproc can write.
  std::string pickDrmFormat(GstElement* postproc) {
    GstPad* pad = gstreamer().gst_element_get_static_pad(postproc, "src");
    GstCaps* caps = gstreamer().gst_pad_query_caps(pad, nullptr);
    gstreamer().gst_object_unref(pad);
    std::string picked;
    for (guint i = 0; caps != nullptr && i < gstreamer().gst_caps_get_size(caps) && picked.empty(); ++i) {
      GstCapsFeatures* features = gstreamer().gst_caps_get_features(caps, i);
      if (!gstreamer().gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF)) {
        continue;
      }
      const GValue* formats =
          gstreamer().gst_structure_get_value(gstreamer().gst_caps_get_structure(caps, i), "drm-format");
      const auto consider = [&](const GValue* value) {
        const char* text = G_VALUE_HOLDS_STRING(value) ? g_value_get_string(value) : nullptr;
        if (picked.empty() && text != nullptr
            && (g_str_has_prefix(text, "AB24") || g_str_has_prefix(text, "XB24") || g_str_has_prefix(text, "AR24")
                || g_str_has_prefix(text, "XR24"))) {
          picked = text;
        }
      };
      if (formats != nullptr && G_VALUE_HOLDS(formats, gstreamer().gst_value_list_get_type())) {
        for (guint j = 0; j < gstreamer().gst_value_list_get_size(formats); ++j) {
          consider(gstreamer().gst_value_list_get_value(formats, j));
        }
      } else if (formats != nullptr) {
        consider(formats);
      }
    }
    if (caps != nullptr) {
      gstreamer().gst_caps_unref(caps);
    }
    return picked;
  }

} // namespace

LockVideo::LockVideo(const std::string& path, bool dmabuf) : m_path(path), m_dmabuf(dmabuf) {
  gstreamer().gst_init(nullptr, nullptr);
  m_wakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (m_wakeFd < 0) {
    throw std::runtime_error("no eventfd");
  }
  try {
    start();
  } catch (const std::exception& e) {
    if (!m_dmabuf) {
      ::close(m_wakeFd);
      throw;
    }
    kLog.info("VA decode unavailable ({}); decoding in software", e.what());
    m_dmabuf = false;
    try {
      start();
    } catch (...) {
      ::close(m_wakeFd);
      throw;
    }
  }
}

LockVideo::~LockVideo() {
  stop();
  ::close(m_wakeFd);
}

void LockVideo::start() {
  GstElement* pipeline = gstreamer().gst_pipeline_new("lock-video");
  GstElement* source = gstreamer().gst_element_factory_make("filesrc", nullptr);
  GstElement* decoder = gstreamer().gst_element_factory_make("decodebin3", nullptr);
  GstElement* convert = gstreamer().gst_element_factory_make(m_dmabuf ? "vapostproc" : "videoconvert", nullptr);
  GstElement* filter = gstreamer().gst_element_factory_make("capsfilter", nullptr);
  GstElement* sink = gstreamer().gst_element_factory_make("appsink", nullptr);
  if (pipeline == nullptr || source == nullptr || decoder == nullptr || convert == nullptr || filter == nullptr
      || sink == nullptr) {
    for (GstElement* element : {pipeline, source, decoder, convert, filter, sink}) {
      if (element != nullptr) {
        gstreamer().gst_object_unref(element);
      }
    }
    throw std::runtime_error("missing GStreamer elements");
  }
  gstreamer().gst_bin_add_many(GST_BIN_CAST(pipeline), source, decoder, convert, filter, sink, nullptr);
  m_pipeline = pipeline;
  m_sink = sink;
  g_object_set(source, "location", m_path.c_str(), nullptr);
  g_signal_connect(decoder, "pad-added", G_CALLBACK(linkDecoded), convert);

  GstCaps* caps = nullptr;
  if (m_dmabuf) {
    // vapostproc lists its driver formats once it has a VA display (READY).
    (void)gstreamer().gst_element_set_state(convert, GST_STATE_READY);
    const std::string format = pickDrmFormat(convert);
    m_fourcc = format.empty() ? 0 : gstVideo().gst_video_dma_drm_fourcc_from_string(format.c_str(), &m_modifier);
    if (m_fourcc == 0) {
      stop();
      throw std::runtime_error("vapostproc writes no RGBA dmabuf");
    }
    caps = gstreamer().gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "DMA_DRM", "drm-format", G_TYPE_STRING, format.c_str(), nullptr
    );
    gstreamer().gst_caps_set_features(
        caps, 0, gstreamer().gst_caps_features_new_single_static_str(GST_CAPS_FEATURE_MEMORY_DMABUF)
    );
  } else {
    caps = gstreamer().gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", nullptr);
  }
  g_object_set(filter, "caps", caps, nullptr);
  gstreamer().gst_caps_unref(caps);

  // Newest frame only, paced by the pipeline clock.
  g_object_set(sink, "max-buffers", 1U, "drop", TRUE, "sync", TRUE, "enable-last-sample", FALSE, nullptr);
  GstAppSinkCallbacks callbacks{};
  callbacks.new_sample = onNewSample;
  callbacks.propose_allocation = onProposeAllocation;
  gstApp().gst_app_sink_set_callbacks(GST_APP_SINK_CAST(sink), &callbacks, &m_wakeFd, nullptr);
  GstBus* bus = gstreamer().gst_element_get_bus(pipeline);
  gstreamer().gst_bus_set_sync_handler(bus, onBusMessage, &m_wakeFd, nullptr);
  gstreamer().gst_object_unref(bus);

  if (!gstreamer().gst_element_link(source, decoder)
      || !gstreamer().gst_element_link_many(convert, filter, sink, nullptr)) {
    stop();
    throw std::runtime_error("cannot link the video pipeline");
  }
  if (gstreamer().gst_element_set_state(pipeline, m_paused ? GST_STATE_PAUSED : GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE) {
    stop();
    throw std::runtime_error("the video pipeline does not start");
  }
}

void LockVideo::stop() noexcept {
  releaseSample();
  if (m_pipeline != nullptr) {
    (void)gstreamer().gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gstreamer().gst_object_unref(m_pipeline);
  }
  m_pipeline = nullptr;
  m_sink = nullptr;
}

void LockVideo::releaseSample() noexcept {
  if (m_sample != nullptr) {
    gstreamer().gst_sample_unref(m_sample);
    m_sample = nullptr;
  }
}

bool LockVideo::fallBack() {
  if (!m_dmabuf) {
    return false;
  }
  stop();
  m_dmabuf = false;
  m_hadFrame = false;
  try {
    start();
  } catch (const std::exception& e) {
    kLog.warn("'{}': {}", m_path, e.what());
    return false;
  }
  return true;
}

bool LockVideo::dispatch() {
  std::uint64_t count = 0;
  (void)::read(m_wakeFd, &count, sizeof(count));
  if (m_pipeline == nullptr) {
    return false;
  }
  GstBus* bus = gstreamer().gst_element_get_bus(m_pipeline);
  bool ok = true;
  while (GstMessage* message =
             gstreamer().gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR))) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
      (void)gstreamer().gst_element_seek_simple(
          m_pipeline, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0
      );
    } else {
      GError* error = nullptr;
      gstreamer().gst_message_parse_error(message, &error, nullptr);
      kLog.warn("'{}': {}", m_path, error != nullptr ? error->message : "pipeline error");
      g_clear_error(&error);
      ok = false;
    }
    gstreamer().gst_message_unref(message);
  }
  gstreamer().gst_object_unref(bus);
  if (!ok) {
    // A stream VA cannot decode fails before its first frame; software may still play it.
    ok = m_dmabuf && !m_hadFrame && fallBack();
    if (!ok) {
      stop();
    }
  }
  return ok;
}

std::optional<LockVideo::Frame> LockVideo::takeFrame() {
  if (m_sink == nullptr) {
    return std::nullopt;
  }
  GstSample* sample = gstApp().gst_app_sink_try_pull_sample(GST_APP_SINK_CAST(m_sink), 0);
  if (sample == nullptr) {
    return std::nullopt;
  }
  // The previous sample backs the image the shader still shows; keep it until this one is usable.
  const auto reject = [sample] {
    gstreamer().gst_sample_unref(sample);
    return std::nullopt;
  };
  GstBuffer* buffer = gstreamer().gst_sample_get_buffer(sample);
  if (buffer == nullptr) {
    return reject();
  }

  Frame frame;
  if (m_dmabuf) {
    GstVideoInfoDmaDrm drm;
    GstMemory* memory = gstreamer().gst_buffer_peek_memory(buffer, 0);
    if (!gstVideo().gst_video_info_dma_drm_from_caps(&drm, gstreamer().gst_sample_get_caps(sample))
        || gstreamer().gst_buffer_n_memory(buffer) != 1
        || !gstAllocators().gst_is_dmabuf_memory(memory)) {
      return reject();
    }
    const GstVideoMeta* meta = gstVideo().gst_buffer_get_video_meta(buffer);
    frame.width = static_cast<std::uint32_t>(GST_VIDEO_INFO_WIDTH(&drm.vinfo));
    frame.height = static_cast<std::uint32_t>(GST_VIDEO_INFO_HEIGHT(&drm.vinfo));
    frame.dmabuf = PixelStreamServer::Dmabuf{
        .fd = gstAllocators().gst_dmabuf_memory_get_fd(memory),
        .width = frame.width,
        .height = frame.height,
        .fourcc = m_fourcc,
        .modifier = m_modifier,
        .stride =
            static_cast<std::uint32_t>(meta != nullptr ? meta->stride[0] : GST_VIDEO_INFO_PLANE_STRIDE(&drm.vinfo, 0)),
        .offset = static_cast<std::uint32_t>(memory->offset + (meta != nullptr ? meta->offset[0] : 0)),
    };
  } else {
    GstVideoInfo info;
    GstVideoFrame mapped;
    if (!gstVideo().gst_video_info_from_caps(&info, gstreamer().gst_sample_get_caps(sample))
        || !gstVideo().gst_video_frame_map(&mapped, &info, buffer, GST_MAP_READ)) {
      return reject();
    }
    frame.width = static_cast<std::uint32_t>(GST_VIDEO_FRAME_WIDTH(&mapped));
    frame.height = static_cast<std::uint32_t>(GST_VIDEO_FRAME_HEIGHT(&mapped));
    const std::size_t row = std::size_t{frame.width} * 4;
    const auto stride = static_cast<std::size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0));
    const auto* data = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0));
    m_pixels.resize(row * frame.height);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
      std::memcpy(m_pixels.data() + row * y, data + stride * y, row);
    }
    gstVideo().gst_video_frame_unmap(&mapped);
    frame.pixels = m_pixels;
  }
  releaseSample();
  m_sample = sample;
  m_hadFrame = true;
  return frame;
}

void LockVideo::setPaused(bool paused) {
  if (std::exchange(m_paused, paused) != paused && m_pipeline != nullptr) {
    (void)gstreamer().gst_element_set_state(m_pipeline, paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);
  }
}
