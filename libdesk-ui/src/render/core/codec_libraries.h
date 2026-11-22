#pragma once

// WebP and JPEG XL load on first use: most sessions decode neither (core/lazy_library.h). Callers use the exported
// *Internal functions instead of the header's inline wrappers, and own their JXL handles with these deleters.
#include "core/lazy_library.h"

#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/resizable_parallel_runner.h>
#include <memory>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>

// clang-format off
#define WEBP_FUNCTIONS(X) \
  X(WebPConfigInitInternal) X(WebPDecodeRGBA) X(WebPEncode) X(WebPEncodeLosslessRGBA) X(WebPEncodeRGB) \
  X(WebPEncodeRGBA) X(WebPFree) X(WebPGetFeaturesInternal) X(WebPMemoryWrite) X(WebPMemoryWriterClear) \
  X(WebPMemoryWriterInit) X(WebPPictureFree) X(WebPPictureImportRGBA) X(WebPPictureInitInternal) \
  X(WebPValidateConfig)
// clang-format on
LAZY_LIBRARY(WebpApi, webp, LAZY_SONAME_WEBP, WEBP_FUNCTIONS)

// clang-format off
#define WEBPDEMUX_FUNCTIONS(X) \
  X(WebPAnimDecoderDelete) X(WebPAnimDecoderGetInfo) X(WebPAnimDecoderGetNext) X(WebPAnimDecoderNewInternal) \
  X(WebPAnimDecoderOptionsInitInternal)
// clang-format on
LAZY_LIBRARY(WebpDemuxApi, webpDemux, LAZY_SONAME_WEBPDEMUX, WEBPDEMUX_FUNCTIONS)

// clang-format off
#define JXL_FUNCTIONS(X) \
  X(JxlColorEncodingSetToSRGB) X(JxlDecoderCloseInput) X(JxlDecoderCreate) X(JxlDecoderDestroy) \
  X(JxlDecoderGetBasicInfo) X(JxlDecoderImageOutBufferSize) X(JxlDecoderProcessInput) \
  X(JxlDecoderSetImageOutBuffer) X(JxlDecoderSetInput) X(JxlDecoderSetParallelRunner) \
  X(JxlDecoderSetUnpremultiplyAlpha) X(JxlDecoderSubscribeEvents) X(JxlEncoderAddImageFrame) \
  X(JxlEncoderCloseInput) X(JxlEncoderCreate) X(JxlEncoderDestroy) X(JxlEncoderFrameSettingsCreate) \
  X(JxlEncoderInitBasicInfo) X(JxlEncoderProcessOutput) X(JxlEncoderSetBasicInfo) X(JxlEncoderSetColorEncoding) \
  X(JxlEncoderSetFrameDistance) X(JxlEncoderSetFrameLossless) X(JxlEncoderSetParallelRunner)
// clang-format on
LAZY_LIBRARY(JxlApi, jxl, LAZY_SONAME_JXL, JXL_FUNCTIONS)

// clang-format off
#define JXLTHREADS_FUNCTIONS(X) \
  X(JxlResizableParallelRunner) X(JxlResizableParallelRunnerCreate) X(JxlResizableParallelRunnerDestroy) \
  X(JxlResizableParallelRunnerSetThreads) X(JxlResizableParallelRunnerSuggestThreads)
// clang-format on
LAZY_LIBRARY(JxlThreadsApi, jxlThreads, LAZY_SONAME_JXL_THREADS, JXLTHREADS_FUNCTIONS)

namespace codec_libraries {

  struct JxlDecoderDeleter {
    void operator()(JxlDecoder* decoder) const { jxl().JxlDecoderDestroy(decoder); }
  };
  struct JxlEncoderDeleter {
    void operator()(JxlEncoder* encoder) const { jxl().JxlEncoderDestroy(encoder); }
  };
  struct JxlRunnerDeleter {
    void operator()(void* runner) const { jxlThreads().JxlResizableParallelRunnerDestroy(runner); }
  };
  using JxlDecoderHandle = std::unique_ptr<JxlDecoder, JxlDecoderDeleter>;
  using JxlEncoderHandle = std::unique_ptr<JxlEncoder, JxlEncoderDeleter>;
  using JxlRunnerHandle = std::unique_ptr<void, JxlRunnerDeleter>;

} // namespace codec_libraries
