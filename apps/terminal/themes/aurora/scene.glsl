// A polar night: slow aurora curtains over a horizon, with a few twinkling stars.
float hash(vec2 p) { return fract(sin(dot(p, vec2(41.3, 289.1))) * 45758.5453); }

float noise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + 1.0), f.x), f.y);
}

vec4 scene(vec2 uv) {
  float t = scene_time * 0.05;
  vec3 sky = mix(scene_surface, scene_surface * 0.6 + scene_accent * 0.05, uv.y);
  vec3 col = sky;
  for (int i = 0; i < 3; i++) {
    float fi = float(i);
    float x = uv.x * (1.4 + fi * 0.5) + t * (1.0 + fi * 0.4);
    float line = 0.25 + fi * 0.09 + 0.08 * sin(x * 3.0 + fi) + 0.05 * noise(vec2(x * 4.0, fi));
    float band = smoothstep(0.0, 0.03, uv.y - line) * exp(-(uv.y - line) * (9.0 - fi * 2.0));
    float rays = 0.6 + 0.4 * noise(vec2(uv.x * 40.0 + fi * 13.0, t * 6.0));
    vec3 tint = mix(scene_accent, scene_text * vec3(0.7, 0.6, 1.0), fi / 3.0);
    col += tint * band * rays * (scene_dark > 0.5 ? 0.7 : 0.35);
  }
  vec2 star = floor(uv * scene_size / 3.0);
  float s = step(0.997, hash(star)) * (0.5 + 0.5 * sin(scene_time * (1.0 + 3.0 * hash(star + 1.0)) + hash(star) * 40.0));
  col += scene_text * s * (1.0 - uv.y) * (scene_dark > 0.5 ? 0.5 : 0.15);
  float horizon = 0.9 + 0.03 * noise(vec2(uv.x * 6.0, 2.0));
  col = mix(col, scene_surface * 0.5, smoothstep(horizon, horizon + 0.004, uv.y));
  return vec4(col, 1.0);
}
