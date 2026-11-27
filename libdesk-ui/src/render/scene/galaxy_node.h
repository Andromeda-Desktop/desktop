#pragma once

#include "render/galaxy/galaxy_model.h"
#include "render/galaxy/galaxy_view.h"
#include "render/scene/node.h"

#include <array>
#include <memory>
#include <optional>

// The phone's galaxy over the node rect (Link). The backend caches the rendered points: a change of points, camera,
// dim, moving or size re-renders them; life and the sky only re-composite.
class GalaxyNode final : public Node {
public:
  GalaxyNode() : Node(NodeType::Galaxy) {}

  void setPoints(std::shared_ptr<const GalaxyPoints> points) {
    if (points != m_points) {
      m_points = std::move(points);
      markPaintDirty();
    }
  }
  void setCamera(const GalaxyCamera& camera) { assign(m_camera, camera); }
  void setDim(float dim) { assign(m_dim, dim); }
  void setLightSky(const std::optional<GalaxyLightSky>& sky) { assign(m_lightSky, sky); }
  void setLife(const GalaxyLife& life) { assign(m_life, life); }
  // True while a drag, zoom or morph changes the camera each frame; false again redraws the points in full.
  void setMoving(bool moving) { assign(m_moving, moving); }
  // The render cache belongs to this owner, so a node rebuilt for the same view keeps the rendered points. The owner
  // outlives every node it is set on; nullptr = the node itself.
  void setCacheOwner(const void* owner) { m_cacheOwner = owner; }
  [[nodiscard]] const void* cacheOwner() const noexcept { return m_cacheOwner != nullptr ? m_cacheOwner : this; }

  [[nodiscard]] const std::shared_ptr<const GalaxyPoints>& points() const noexcept { return m_points; }
  [[nodiscard]] const GalaxyCamera& camera() const noexcept { return m_camera; }
  [[nodiscard]] float dim() const noexcept { return m_dim; }
  [[nodiscard]] const std::optional<GalaxyLightSky>& lightSky() const noexcept { return m_lightSky; }
  [[nodiscard]] const GalaxyLife& life() const noexcept { return m_life; }
  [[nodiscard]] bool moving() const noexcept { return m_moving; }

  // Where a galaxy point shows in node coordinates now (with the life turn); empty behind the eye.
  [[nodiscard]] std::optional<std::array<float, 2>> project(const std::array<float, 3>& point) const {
    return galaxyProject(m_camera, width(), height(), point, m_life.angle);
  }

private:
  template <typename T> void assign(T& field, const T& value) {
    if (!(field == value)) {
      field = value;
      markPaintDirty();
    }
  }

  std::shared_ptr<const GalaxyPoints> m_points;
  GalaxyCamera m_camera;
  float m_dim = 1.0F;
  std::optional<GalaxyLightSky> m_lightSky;
  GalaxyLife m_life;
  bool m_moving = false;
  const void* m_cacheOwner = nullptr;
};
