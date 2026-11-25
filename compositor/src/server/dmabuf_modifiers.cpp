#include "server/dmabuf_modifiers.h"

#include "core/log.h"
#include "wlr.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <drm_fourcc.h>
#include <gbm.h>
#include <unistd.h>
#include <vector>

namespace andromeda {

  namespace {

    constexpr Logger kLog("dmabuf");

    // Mesa 26.3 isl trims the empty tiles at the end of the last tile row and puts the aux plane right after them
    // (mesa issue 16984, MR 46707). 390x255 leaves 3 empty Tile4 columns in that row, so the trim shows at this size.
    constexpr std::uint32_t kProbeWidth = 390;
    constexpr std::uint32_t kProbeHeight = 255;

    // False when a buffer allocated with this modifier would fail the linux-dmabuf out_of_bounds check: plane 0
    // must hold offset + stride * height bytes before the buffer ends and before any later plane starts.
    bool layoutFitsProtocol(gbm_device* gbm, std::uint64_t modifier) {
      gbm_bo* bo = gbm_bo_create_with_modifiers2(
          gbm, kProbeWidth, kProbeHeight, GBM_FORMAT_ARGB8888, &modifier, 1, GBM_BO_USE_RENDERING
      );
      if (bo == nullptr) {
        return true;
      }
      const std::uint64_t start = gbm_bo_get_offset(bo, 0);
      const std::uint64_t end = start + std::uint64_t{gbm_bo_get_stride_for_plane(bo, 0)} * kProbeHeight;
      std::uint64_t limit = UINT64_MAX;
      const int fd = gbm_bo_get_fd(bo);
      if (fd >= 0) {
        const off_t size = lseek(fd, 0, SEEK_END);
        if (size >= 0) {
          limit = static_cast<std::uint64_t>(size);
        }
        close(fd);
      }
      for (int plane = 1; plane < gbm_bo_get_plane_count(bo); ++plane) {
        const std::uint64_t offset = gbm_bo_get_offset(bo, plane);
        if (offset > start) {
          limit = std::min(limit, offset);
        }
      }
      gbm_bo_destroy(bo);
      return end <= limit;
    }

    std::vector<std::uint64_t> brokenIntelModifiers(int drmFd, const wlr_drm_format_set& formats) {
      std::vector<std::uint64_t> broken;
      const wlr_drm_format* argb = wlr_drm_format_set_get(&formats, DRM_FORMAT_ARGB8888);
      if (argb == nullptr) {
        return broken;
      }
      gbm_device* gbm = nullptr;
      for (std::size_t i = 0; i < argb->len; ++i) {
        const std::uint64_t modifier = argb->modifiers[i];
        if (fourcc_mod_get_vendor(modifier) != DRM_FORMAT_MOD_VENDOR_INTEL) {
          continue;
        }
        if (gbm == nullptr && (gbm = gbm_create_device(drmFd)) == nullptr) {
          kLog.warn("gbm device for the modifier probe failed");
          return broken;
        }
        if (!layoutFitsProtocol(gbm, modifier)) {
          broken.push_back(modifier);
        }
      }
      if (gbm != nullptr) {
        gbm_device_destroy(gbm);
      }
      return broken;
    }

  } // namespace

  void dropUnsafeDmabufModifiers(wlr_renderer* renderer) {
    const int drmFd = wlr_renderer_get_drm_fd(renderer);
    // wlroots returns the EGL-owned set as const; feedback, wl_drm and the scene all read this one set.
    auto* formats = const_cast<wlr_drm_format_set*>(wlr_renderer_get_texture_formats(renderer, WLR_BUFFER_CAP_DMABUF));
    if (drmFd < 0 || formats == nullptr) {
      return;
    }
    const auto started = std::chrono::steady_clock::now();
    const std::vector<std::uint64_t> broken = brokenIntelModifiers(drmFd, *formats);
    for (const std::uint64_t modifier : broken) {
      for (std::size_t i = 0; i < formats->len; ++i) {
        wlr_drm_format_set_remove(formats, formats->formats[i].format, modifier);
      }
      kLog.warn("modifier {:#018x} not offered: the driver allocates it smaller than the protocol bound", modifier);
    }
    kLog.debug(
        "modifier probe {} us",
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()
    );
  }

} // namespace andromeda
