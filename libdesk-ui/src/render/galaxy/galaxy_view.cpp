#include "render/galaxy/galaxy_view.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

using Vec3 = std::array<float, 3>;

constexpr float kFov = 40.0F * std::numbers::pi_v<float> / 180.0F;
constexpr float kFar = 400.0F;
// Below this the disc is nearly edge-on: the turn stops squashing further, or it would sample far outside the image.
constexpr float kMinTilt = 0.2F;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 normalize(const Vec3& a) {
  const float l = std::sqrt(dot(a, a));
  return {a[0] / l, a[1] / l, a[2] / l};
}

std::optional<std::array<float, 2>> project(const GalaxyMat4& m, float width, float height, const Vec3& p) {
  const float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
  const float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
  const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
  if (w <= 0.0F) return std::nullopt;
  return std::array<float, 2>{(x / w * 0.5F + 0.5F) * width, (0.5F - y / w * 0.5F) * height};
}

}  // namespace

std::array<float, 3> galaxyEye(const GalaxyCamera& c) {
  const float cp = std::cos(c.pitch);
  return {c.dist * cp * std::sin(c.yaw), c.dist * std::sin(c.pitch), c.dist * cp * std::cos(c.yaw)};
}

GalaxyMat4 galaxyViewProjection(const GalaxyCamera& c, float width, float height) {
  const Vec3 e = galaxyEye(c);
  const Vec3 f = normalize(sub({0, 0, 0}, e));
  const Vec3 r = normalize(cross(f, {0, 1, 0}));
  const Vec3 u = cross(r, f);
  const float n = std::max(c.dist * 0.004F, 0.0005F);
  const float focal = 1.0F / std::tan(kFov / 2.0F);
  const float aspect = height > 0.0F ? width / height : 1.0F;
  const GalaxyMat4 v = {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0, -dot(r, e), -dot(u, e), dot(f, e), 1};
  const GalaxyMat4 p = {focal / aspect, 0, 0, 0, 0, focal, 0, 0, -c.sx, -c.sy, (kFar + n) / (n - kFar), -1,
                        0, 0, 2 * kFar * n / (n - kFar), 0};
  GalaxyMat4 out{};
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += p[k * 4 + row] * v[col * 4 + k];
      out[col * 4 + row] = s;
    }
  }
  return out;
}

GalaxySpin galaxySpin(const GalaxyCamera& c, float width, float height, float angle) {
  GalaxySpin spin;
  if (const auto core = project(galaxyViewProjection(c, width, height), width, height, {0, 0, 0})) spin.centre = *core;
  // The disc seen from pitch p is a circle squashed by sin p on screen: turn = squash . rotate . unsquash.
  const float k = std::max(std::abs(std::sin(c.pitch)), kMinTilt);
  const float co = std::cos(angle);
  const float si = std::sin(angle);
  spin.m = {co, k * si, -si / k, co};
  return spin;
}

std::optional<std::array<float, 2>>
galaxyProject(const GalaxyCamera& c, float width, float height, const std::array<float, 3>& point, float angle) {
  const auto p = project(galaxyViewProjection(c, width, height), width, height, point);
  if (!p) return std::nullopt;
  const GalaxySpin s = galaxySpin(c, width, height, angle);
  const float dx = (*p)[0] - s.centre[0];
  const float dy = (*p)[1] - s.centre[1];
  return std::array<float, 2>{s.centre[0] + s.m[0] * dx + s.m[2] * dy, s.centre[1] + s.m[1] * dx + s.m[3] * dy};
}
