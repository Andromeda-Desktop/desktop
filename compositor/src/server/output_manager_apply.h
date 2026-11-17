#pragma once

#include "output/display_store.h"
#include "wlr.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace andromeda {

  class Output;

  // One output-management apply between its draw and its commit: the draw runs on the render thread, and the commit,
  // rollback and reply run from its kick.
  struct OutputManagerApply {
    struct RequestedHead {
      wlr_output_configuration_head_v1* head = nullptr;
      Output* output = nullptr;
      bool wasDesktopEnabled = false;
      bool wasMirror = false;
      SavedOutput live;
    };
    struct BackendSnapshot {
      wlr_output* output = nullptr;
      bool enabled = false;
      wlr_output_mode* mode = nullptr;
      int width = 0;
      int height = 0;
      int refresh = 0;
      float scale = 1.0F;
      wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
      bool adaptiveSyncSupported = false;
      bool adaptiveSyncEnabled = false;
    };
    struct SceneRectDestroy {
      void operator()(wlr_scene_rect* rect) const {
        if (rect != nullptr) {
          wlr_scene_node_destroy(&rect->node);
        }
      }
    };
    using FrameMasks = std::vector<std::unique_ptr<wlr_scene_rect, SceneRectDestroy>>;

    wlr_output_configuration_v1* config = nullptr;
    bool testOnly = false;
    wlr_backend_output_state* states = nullptr;
    size_t statesLen = 0;
    std::vector<RequestedHead> requested;
    wlr_output_swapchain_manager swapchainManager{};
    bool swapchainManagerFinished = false;
    std::vector<BackendSnapshot> snapshots;
    std::vector<wlr_backend_output_state> rollbackStates;
    FrameMasks configuredFrameMasks;
    // The states are built and their draw was submitted: the commit is due.
    bool built = false;
    bool ok = false;
  };

} // namespace andromeda
