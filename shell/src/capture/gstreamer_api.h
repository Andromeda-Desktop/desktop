#pragma once

// GStreamer loads on first use (screen recording, lock screen video); see core/lazy_library.h. The inline unref
// helpers become exported calls, and callers use the *_CAST macros and *_get_type() calls, so no header code
// references a GStreamer symbol directly.
#define GST_DISABLE_MINIOBJECT_INLINE_FUNCTIONS

#include "core/lazy_library.h"

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

// clang-format off
#define GSTREAMER_FUNCTIONS(X) \
  X(gst_bin_add) X(gst_bin_add_many) X(gst_bin_get_by_name) X(gst_buffer_n_memory) X(gst_buffer_peek_memory) \
  X(gst_bus_get_pollfd) X(gst_bus_pop) X(gst_bus_pop_filtered) X(gst_bus_set_sync_handler) \
  X(gst_caps_features_contains) X(gst_caps_features_new_single_static_str) X(gst_caps_get_features) \
  X(gst_caps_get_size) X(gst_caps_get_structure) X(gst_caps_new_simple) X(gst_caps_set_features) \
  X(gst_caps_unref) X(gst_element_factory_make) X(gst_element_get_bus) X(gst_element_get_static_pad) \
  X(gst_element_link) X(gst_element_link_many) X(gst_element_seek_simple) X(gst_element_send_event) \
  X(gst_element_set_state) X(gst_event_new_eos) X(gst_event_parse_caps) X(gst_fraction_get_type) X(gst_init) \
  X(gst_is_initialized) X(gst_message_parse_error) X(gst_message_unref) X(gst_object_unref) X(gst_pad_add_probe) \
  X(gst_pad_get_current_caps) X(gst_pad_get_parent_element) X(gst_pad_is_linked) X(gst_pad_link) \
  X(gst_pad_query_caps) X(gst_pipeline_new) X(gst_query_add_allocation_meta) X(gst_sample_get_buffer) \
  X(gst_sample_get_caps) X(gst_sample_unref) X(gst_structure_free) X(gst_structure_get_int) \
  X(gst_structure_get_name) X(gst_structure_get_value) X(gst_structure_new) X(gst_util_set_object_arg) \
  X(gst_value_list_get_size) X(gst_value_list_get_type) X(gst_value_list_get_value)
// clang-format on
LAZY_LIBRARY(GstreamerApi, gstreamer, LAZY_SONAME_GSTREAMER, GSTREAMER_FUNCTIONS)

// clang-format off
#define GSTAPP_FUNCTIONS(X) \
  X(gst_app_sink_set_callbacks) X(gst_app_sink_try_pull_sample)
// clang-format on
LAZY_LIBRARY(GstAppApi, gstApp, LAZY_SONAME_GSTAPP, GSTAPP_FUNCTIONS)

// clang-format off
#define GSTVIDEO_FUNCTIONS(X) \
  X(gst_buffer_get_video_meta) X(gst_video_dma_drm_fourcc_from_string) X(gst_video_frame_map) \
  X(gst_video_frame_unmap) X(gst_video_info_dma_drm_from_caps) X(gst_video_info_from_caps) \
  X(gst_video_meta_api_get_type)
// clang-format on
LAZY_LIBRARY(GstVideoApi, gstVideo, LAZY_SONAME_GSTVIDEO, GSTVIDEO_FUNCTIONS)

// clang-format off
#define GSTALLOCATORS_FUNCTIONS(X) \
  X(gst_dmabuf_memory_get_fd) X(gst_is_dmabuf_memory)
// clang-format on
LAZY_LIBRARY(GstAllocatorsApi, gstAllocators, LAZY_SONAME_GSTALLOCATORS, GSTALLOCATORS_FUNCTIONS)
