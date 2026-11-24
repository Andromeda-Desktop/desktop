#include "shell/lockscreen/lockscreen_transition.h"

#include "render/scene/texture_shader_node.h"

#include <algorithm>
#include <memory>

namespace {

  constexpr char kFragment[] = R"(
precision highp float;
uniform sampler2D u_texture;
uniform float u_transition;
uniform float u_progress;
uniform float u_opacity;
uniform float u_direction;
uniform float u_smoothness;
uniform float u_center_x;
uniform float u_center_y;
#define u_center vec2(u_center_x, u_center_y)
uniform float u_aspect_ratio;
uniform float u_stripe_count;
uniform float u_angle;
uniform float u_cell_size;
varying vec2 v_texcoord;

vec2 hex_round(float q, float r) {
    float x = q;
    float z = r;
    float y = -x - z;
    float rx = floor(x + 0.5);
    float ry = floor(y + 0.5);
    float rz = floor(z + 0.5);
    float dx = abs(rx - x);
    float dy = abs(ry - y);
    float dz = abs(rz - z);
    if (dx > dy && dx > dz) {
        rx = -ry - rz;
    } else if (dy > dz) {
        ry = -rx - rz;
    } else {
        rz = -rx - ry;
    }
    return vec2(rx, rz);
}

float max_distance_to_corners(vec2 center, float aspect) {
    vec2 corner_delta = max(center, vec2(aspect, 1.0) - center);
    return length(corner_delta);
}

float wipe_coverage(vec2 uv, float progress, float smoothness) {
    float extended = progress * (1.0 + 2.0 * smoothness) - smoothness;
    float factor;
    if (u_direction < 0.5) {
        float edge = 1.0 - extended;
        factor = smoothstep(edge - smoothness, edge + smoothness, uv.x);
    } else if (u_direction < 1.5) {
        float edge = extended;
        factor = smoothstep(edge - smoothness, edge + smoothness, uv.x);
        factor = 1.0 - factor;
    } else if (u_direction < 2.5) {
        float edge = 1.0 - extended;
        factor = smoothstep(edge - smoothness, edge + smoothness, uv.y);
    } else {
        float edge = extended;
        factor = smoothstep(edge - smoothness, edge + smoothness, uv.y);
        factor = 1.0 - factor;
    }
    return 1.0 - factor;
}

float disc_coverage(vec2 uv, float progress, float smoothness) {
    vec2 aspect_uv = vec2(uv.x * u_aspect_ratio, uv.y);
    vec2 center = vec2(u_center.x * u_aspect_ratio, u_center.y);
    float max_dist = max_distance_to_corners(center, u_aspect_ratio);
    float radius = progress * (max_dist + 2.0 * smoothness) - smoothness;
    return smoothstep(radius - smoothness, radius + smoothness, distance(aspect_uv, center));
}

float stripe_coverage(vec2 uv, float progress, float smoothness) {
    float rad = radians(u_angle);
    float cos_a = cos(rad);
    float sin_a = sin(rad);
    vec2 aspect_uv = vec2(uv.x * u_aspect_ratio, uv.y);
    float stripe_coord = aspect_uv.x * cos_a + aspect_uv.y * sin_a;
    float perp_coord = -aspect_uv.x * sin_a + aspect_uv.y * cos_a;
    float max_stripe_coord = u_aspect_ratio * abs(cos_a) + abs(sin_a);
    float stripe_width = max_stripe_coord / max(u_stripe_count, 1.0);
    float stripe_pos = stripe_coord / max(stripe_width, 0.0001);
    float odd = mod(floor(stripe_pos), 2.0);
    float max_perp = u_aspect_ratio * abs(sin_a) + abs(cos_a);
    float delay = abs(perp_coord / max(max_perp, 0.0001)) * 0.3;
    float local_progress = clamp((progress - delay) / max(1.0 - delay, 0.0001), 0.0, 1.0);
    float local_fraction = fract(stripe_pos);
    if (odd > 0.5) {
        float edge = mix(1.0 + smoothness, -smoothness, local_progress);
        return 1.0 - smoothstep(edge - smoothness, edge + smoothness, local_fraction);
    }
    float edge = mix(-smoothness, 1.0 + smoothness, local_progress);
    return smoothstep(edge - smoothness, edge + smoothness, local_fraction);
}

float honeycomb_coverage(vec2 uv, float progress, float smoothness) {
    float size = max(u_cell_size, 0.0001);
    vec2 aspect_uv = vec2(uv.x * u_aspect_ratio, uv.y);
    float q = (aspect_uv.x * (2.0 / 3.0)) / size;
    float r = ((-aspect_uv.x / 3.0) + (sqrt(3.0) / 3.0) * aspect_uv.y) / size;
    vec2 hex = hex_round(q, r);
    vec2 cell_center = vec2(
        hex.x * 1.5 * size,
        (hex.x * sqrt(3.0) / 2.0 + hex.y * sqrt(3.0)) * size
    );
    vec2 center = vec2(u_center.x * u_aspect_ratio, u_center.y);
    float max_dist = max_distance_to_corners(center, u_aspect_ratio);
    float radius = progress * (max_dist + 2.0 * smoothness) - smoothness;
    return smoothstep(radius - smoothness, radius + smoothness, distance(cell_center, center));
}

float vortex_coverage(vec2 uv, float progress, float smoothness) {
    vec2 aspect_uv = vec2(uv.x * u_aspect_ratio, uv.y);
    vec2 center = vec2(u_center.x * u_aspect_ratio, u_center.y);
    vec2 delta = aspect_uv - center;
    float dist = length(delta);
    float angle = atan(delta.y, delta.x) + (1.0 - progress) * 3.14159265;
    float norm_angle = (angle + 3.14159265) / 6.2831853;
    float arms = 3.0;
    float spiral = dist + fract(norm_angle * arms) * 0.25;
    float max_dist = max_distance_to_corners(center, u_aspect_ratio);
    float feather = max(0.015, smoothness * 1.5);
    float edge = progress * (max_dist + 0.25 + 2.0 * feather) - feather;
    return smoothstep(edge - feather, edge + feather, spiral);
}

float pixelate_coverage(vec2 uv, float progress, float smoothness) {
    vec2 aspect_uv = vec2(uv.x * u_aspect_ratio, uv.y);
    vec2 center = vec2(u_center.x * u_aspect_ratio, u_center.y);
    float size = max(u_cell_size * 0.75, 0.02);
    vec2 cell = floor(aspect_uv / size);
    float noise = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
    float max_dist = max_distance_to_corners(center, u_aspect_ratio);
    float dist_norm = distance(aspect_uv, center) / max(max_dist, 0.001);
    float threshold = mix(dist_norm, noise, 0.35);
    float feather = max(0.02, smoothness * 1.5);
    float edge = progress * (1.0 + 2.0 * feather) - feather;
    return smoothstep(edge - feather, edge + feather, threshold);
}

float diamond_coverage(vec2 uv, float progress, float smoothness) {
    vec2 aspect_uv = vec2(uv.x * u_aspect_ratio, uv.y);
    vec2 center = vec2(u_center.x * u_aspect_ratio, u_center.y);
    float size = max(u_cell_size * 1.15, 0.0345);
    vec2 cell_index = floor(aspect_uv / size);
    vec2 cell_center = (cell_index + 0.5) * size;
    vec2 in_cell = abs(aspect_uv - cell_center) / (size * 0.5);
    float diamond_metric = in_cell.x + in_cell.y;
    float max_dist = max_distance_to_corners(center, u_aspect_ratio);
    float delay = (distance(cell_center, center) / max(max_dist, 0.001)) * 0.4;
    float local_progress = clamp((progress - delay) / max(1.0 - delay, 0.0001), 0.0, 1.0);
    float feather = max(0.05, smoothness * 2.5);
    float radius = local_progress * (2.0 + 2.0 * feather) - feather;
    return smoothstep(radius - feather, radius + feather, diamond_metric);
}

float golden_coverage(vec2 uv, float progress, float smoothness) {
    vec2 aspect_uv = vec2(uv.x * u_aspect_ratio, uv.y);
    vec2 center = vec2(u_center.x * u_aspect_ratio, u_center.y);
    vec2 delta = aspect_uv - center;
    float dist = length(delta);
    float angle = atan(delta.y, delta.x);
    float max_dist = max_distance_to_corners(center, u_aspect_ratio);

    // Golden ratio logarithmic spiral (growth rate b = ln(1.6180339887) / (pi / 2) ≈ 0.306349)
    float b = 0.306349;
    float phase = radians(u_angle) + progress * 6.2831853;
    float delta_theta = mod(angle - phase, 6.2831853);
    float golden_dist = (dist / max(max_dist, 0.001)) * exp(-b * delta_theta);

    float feather = max(0.02, smoothness * 1.6);
    float edge = progress * (1.0 + 2.0 * feather) - feather;
    return smoothstep(edge - feather, edge + feather, golden_dist);
}

float melt_coverage(vec2 uv, float progress, float smoothness) {
    float feather = max(0.02, smoothness * 1.5);
    float wave = sin(uv.x * 14.0 + radians(u_angle)) * 0.05
               + sin(uv.x * 32.0 - radians(u_angle) * 1.5) * 0.025
               + cos(uv.x * 7.0 + 1.2) * 0.04;

    float drip = uv.y + wave;
    float wave_amplitude = 0.115;
    float sweep_span = 1.0 + 2.0 * wave_amplitude;
    float edge = progress * (sweep_span + 2.0 * feather) - wave_amplitude - feather;
    return smoothstep(edge - feather, edge + feather, drip);
}

void main() {
    float progress = clamp(u_progress, 0.0, 1.0);
    vec2 uv = v_texcoord;
    float coverage = 1.0 - progress;
    float smoothness = mix(0.001, 0.5, u_smoothness * u_smoothness);

    if (progress <= 0.00001) {
        coverage = 1.0;
    } else if (progress >= 0.99999) {
        coverage = 0.0;
    } else if (u_transition < 0.5) {
        coverage = 1.0 - progress;
    } else if (u_transition < 1.5) {
        coverage = wipe_coverage(uv, progress, smoothness);
    } else if (u_transition < 2.5) {
        coverage = disc_coverage(uv, progress, smoothness);
    } else if (u_transition < 3.5) {
        coverage = stripe_coverage(uv, progress, mix(0.001, 0.3, u_smoothness * u_smoothness));
    } else if (u_transition < 4.5) {
        float scale = 1.0 + 0.15 * progress;
        uv = (uv - 0.5) / scale + 0.5;
        coverage = 1.0 - progress;
    } else if (u_transition < 5.5) {
        coverage = honeycomb_coverage(uv, progress, smoothness);
    } else if (u_transition < 6.5) {
        coverage = vortex_coverage(uv, progress, smoothness);
    } else if (u_transition < 7.5) {
        coverage = pixelate_coverage(uv, progress, smoothness);
    } else if (u_transition < 8.5) {
        coverage = diamond_coverage(uv, progress, smoothness);
    } else if (u_transition < 9.5) {
        coverage = golden_coverage(uv, progress, smoothness);
    } else {
        coverage = melt_coverage(uv, progress, smoothness);
    }

    vec4 texel = texture2D(u_texture, uv);
    float alpha = texel.a * clamp(coverage * u_opacity, 0.0, 1.0);
    gl_FragColor = vec4(texel.rgb * alpha, alpha);
}
)";

} // namespace

LockscreenTransitionCover::LockscreenTransitionCover() {
  setHitTestVisible(false);
  setParticipatesInLayout(false);
  auto shader = std::make_unique<TextureShaderNode>();
  shader->setParticipatesInLayout(false);
  shader->setFragment(kFragment);
  m_shader = static_cast<TextureShaderNode*>(addChild(std::move(shader)));
}

void LockscreenTransitionCover::setTexture(TextureId texture) { m_shader->setTexture(texture); }

void LockscreenTransitionCover::setTransition(
    LockscreenTransitionKind transition, float progress, const LockscreenTransitionParams& params
) {
  m_shader->setUniform("u_transition", static_cast<float>(transition));
  m_shader->setUniform("u_progress", std::clamp(progress, 0.0F, 1.0F));
  m_shader->setUniform("u_direction", params.direction);
  m_shader->setUniform("u_smoothness", params.smoothness);
  m_shader->setUniform("u_center_x", params.centerX);
  m_shader->setUniform("u_center_y", params.centerY);
  m_shader->setUniform("u_aspect_ratio", params.aspectRatio);
  m_shader->setUniform("u_stripe_count", params.stripeCount);
  m_shader->setUniform("u_angle", params.angle);
  m_shader->setUniform("u_cell_size", params.cellSize);
}

void LockscreenTransitionCover::setSize(float width, float height) {
  Node::setSize(width, height);
  m_shader->setFrameSize(width, height);
}

void LockscreenTransitionCover::setFrameSize(float width, float height) {
  Node::setFrameSize(width, height);
  m_shader->setFrameSize(width, height);
}
