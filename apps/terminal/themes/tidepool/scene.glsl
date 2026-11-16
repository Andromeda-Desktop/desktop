// Shallow water: a lit gradient, moving caustics, and a manta and bubbles built from glyph tiles.
// Inputs: scene_time, scene_size, scene_scale, scene_dark, scene_surface/text/accent, and the reactions
// scene_busy (0..1 while a command runs), scene_remote (0..1 while on another host), scene_success, scene_failure,
// scene_bell, scene_cwd, scene_host (one 0 -> 1 -> 0 pulse each).
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

// A 3x5 glyph made from hash bits; 1 inside a lit stroke.
float glyph(vec2 cell, vec2 f) {
  vec2 g = floor(f * vec2(3.0, 5.0));
  float bit = g.x + g.y * 3.0;
  return step(0.45, fract(hash(cell) * exp2(bit)));
}

float caustic(vec2 p, float t) {
  float c = 0.0;
  vec2 q = p;
  for (int i = 0; i < 3; i++) {
    q += vec2(sin(q.y * 1.7 + t * 0.6), cos(q.x * 1.3 - t * 0.5)) * 0.45;
    c += 0.5 / (1.0 + 12.0 * abs(sin(q.x + q.y)));
  }
  return c / 3.0;
}

// Manta seen from above, swimming toward +x, in units of its width; > 0 inside.
float manta(vec2 p, float flap) {
  float span = 0.5 * (0.85 + 0.15 * flap);
  float s = abs(p.y) / span; // 0 on the spine, 1 at a wing tip
  float lead = 0.2 - 0.35 * s * s;                     // convex leading edge, tips swept back
  float trail = -0.18 + 0.03 * s + 0.25 * s * (1.0 - s); // crescent trailing edge
  float wings = step(s, 1.0) * min(lead - p.x, p.x - trail);
  float horns = step(0.2, p.x) * step(p.x, 0.27) * step(0.035, abs(p.y)) * step(abs(p.y), 0.07);
  float tail = step(-0.62, p.x) * step(p.x, -0.15) * step(abs(p.y), 0.025);
  return max(wings, max(horns, tail));
}

vec4 scene(vec2 uv) {
  vec2 px = uv * scene_size;
  float t = scene_time;
  vec3 deep = mix(scene_surface * 0.55, scene_surface, 0.4 + 0.6 * (1.0 - uv.y));
  vec3 shallow = mix(scene_surface, vec3(1.0), scene_dark > 0.5 ? 0.06 : 0.35);
  vec3 col = mix(deep, shallow, smoothstep(1.0, 0.0, uv.y + 0.3 * uv.x));
  col += (scene_dark > 0.5 ? 0.16 : 0.22) * (1.0 + 0.5 * scene_busy) * caustic(px / 90.0, t) * mix(vec3(1.0), scene_accent * 0.4 + 0.6, 0.3);

  float tile = 12.0;
  vec2 cell = floor(px / tile);
  vec2 f = fract(px / tile);
  float inset = step(0.08, f.x) * step(f.x, 0.92) * step(0.08, f.y) * step(f.y, 0.92);

  // The manta glides across in 40 s and flaps slowly.
  float w = scene_size.x * 0.3;
  vec2 centre = vec2(mod(t * scene_size.x / 40.0 + scene_size.x * 0.75, scene_size.x + 2.0 * w) - w,
                     scene_size.y * (0.68 + 0.05 * sin(t * 0.3)));
  vec2 m = ((cell + 0.5) * tile - centre) / w;
  if (manta(m, sin(t * 1.2)) > 0.0) {
    vec3 ink = mix(scene_accent, vec3(1.0, 0.85, 0.35), step(0.75, hash(cell + 7.0)));
    vec3 face = mix(ink, col, 0.25);
    col = mix(col, mix(face, ink, glyph(cell, f)), inset);
  }

  // Bubbles: single tiles rising in a few columns.
  float column = floor(cell.x / 5.0);
  float speed = 18.0 + 14.0 * hash(vec2(column, 3.0));
  float rise = mod(cell.y * tile + t * speed + hash(vec2(column, 9.0)) * scene_size.y, scene_size.y * 0.7);
  float bubble = step(mod(cell.x, 5.0), 0.5) * step(rise, tile * 2.0) * step(0.35, hash(vec2(column, 1.0)));
  col = mix(col, mix(col, vec3(1.0, 0.85, 0.35), 0.45 + 0.4 * glyph(cell + 3.0, f)), bubble * inset);
  // Reactions: success lifts the water toward the accent, a failure warms it once, the bell glints.
  col = mix(col, scene_accent, 0.18 * scene_success);
  col = mix(col, vec3(0.85, 0.32, 0.25), 0.22 * scene_failure);
  col += 0.08 * scene_bell;
  // Another host is deeper water; arriving there, or in a new folder, lifts the light once.
  col = mix(col, vec3(0.08, 0.12, 0.30), 0.35 * scene_remote);
  col += 0.05 * scene_cwd + 0.12 * scene_host;
  return vec4(col, 1.0);
}
