#pragma once

#include "wlr.h"

namespace andromeda {

  // Defers the scene visibility pass and output damage of every node change in this scope to one pass at its end, so
  // moving N views walks the scene once instead of N times. Scopes nest.
  class SceneUpdateBatch {
  public:
    explicit SceneUpdateBatch(wlr_scene* scene) : m_scene(scene) { wlr_scene_update_batch_begin(m_scene); }
    ~SceneUpdateBatch() { wlr_scene_update_batch_end(m_scene); }
    SceneUpdateBatch(const SceneUpdateBatch&) = delete;
    SceneUpdateBatch& operator=(const SceneUpdateBatch&) = delete;

  private:
    wlr_scene* m_scene;
  };

} // namespace andromeda
