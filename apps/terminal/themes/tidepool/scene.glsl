// Shallow water: a lit gradient and moving caustics.
// Inputs: scene_time, scene_size, scene_scale, scene_dark, scene_surface/text/accent, and the reactions
// scene_busy (0..1 while a command runs), scene_remote (0..1 while on another host), scene_success, scene_failure,
// scene_bell, scene_cwd, scene_host (one 0 -> 1 -> 0 pulse each).

float caustic(vec2 p, float t) {
  float c = 0.0;
  vec2 q = p;
  for (int i = 0; i < 3; i++) {
    q += vec2(sin(q.y * 1.7 + t * 0.6), cos(q.x * 1.3 - t * 0.5)) * 0.45;
    c += 0.5 / (1.0 + 12.0 * abs(sin(q.x + q.y)));
  }
  return c / 3.0;
}

vec4 scene(vec2 uv) {
  vec2 px = uv * scene_size;
  float t = scene_time;
  vec3 deep = mix(scene_surface * 0.55, scene_surface, 0.4 + 0.6 * (1.0 - uv.y));
  vec3 shallow = mix(scene_surface, vec3(1.0), scene_dark > 0.5 ? 0.06 : 0.35);
  vec3 col = mix(deep, shallow, smoothstep(1.0, 0.0, uv.y + 0.3 * uv.x));
  col += (scene_dark > 0.5 ? 0.16 : 0.22) * (1.0 + 0.5 * scene_busy) * caustic(px / 90.0, t) * mix(vec3(1.0), scene_accent * 0.4 + 0.6, 0.3);

  // Reactions: success lifts the water toward the accent, a failure warms it once, the bell glints.
  col = mix(col, scene_accent, 0.18 * scene_success);
  col = mix(col, vec3(0.85, 0.32, 0.25), 0.22 * scene_failure);
  col += 0.08 * scene_bell;
  // Another host is deeper water; arriving there, or in a new folder, lifts the light once.
  col = mix(col, vec3(0.08, 0.12, 0.30), 0.35 * scene_remote);
  col += 0.05 * scene_cwd + 0.12 * scene_host;
  return vec4(col, 1.0);
}
