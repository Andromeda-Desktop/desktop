float rounded_rect_distance_at(
    vec2 point, vec2 size, vec2 position, float radius_tl, float radius_tr, float radius_bl, float radius_br
) {
  vec2 relative_pos = point - position;
  vec2 centered_pos = relative_pos - size * 0.5;

  float radius;
  if (centered_pos.x < 0.0) {
    radius = centered_pos.y < 0.0 ? radius_tl : radius_bl;
  } else {
    radius = centered_pos.y < 0.0 ? radius_tr : radius_br;
  }

  // Continuous-curvature corner, the same formula as libdesk-ui smooth_corner_glsl.h (radius.smoothing token).
  vec2 half_size = size * 0.5;
  float max_r = min(half_size.x, half_size.y);
  float s = radius > 0.0 ? clamp((max_r / radius - 1.0) / 0.88, 0.0, TOKEN_RADIUS_SMOOTHING) : 0.0;
  float e = min(radius * (1.0 + 0.88 * s), max_r);
  vec2 q = abs(centered_pos) - (half_size - vec2(e));
  if (s > 0.001 && q.x > 0.0 && q.y > 0.0) {
    float n = 2.0 + 5.0 * s;
    vec2 u = q / e;
    return (pow(pow(u.x, n) + pow(u.y, n), 1.0 / n) - 1.0) * e;
  }
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - e;
}

// The outward unit gradient of rounded_rect_distance_at, in closed form: one pow in a smoothed corner instead of the
// four distance evaluations of a central difference.
vec2 rounded_rect_normal_at(
    vec2 point, vec2 size, vec2 position, float radius_tl, float radius_tr, float radius_bl, float radius_br
) {
  vec2 centered_pos = point - position - size * 0.5;
  float radius;
  if (centered_pos.x < 0.0) {
    radius = centered_pos.y < 0.0 ? radius_tl : radius_bl;
  } else {
    radius = centered_pos.y < 0.0 ? radius_tr : radius_br;
  }
  vec2 half_size = size * 0.5;
  float max_r = min(half_size.x, half_size.y);
  float s = radius > 0.0 ? clamp((max_r / radius - 1.0) / 0.88, 0.0, TOKEN_RADIUS_SMOOTHING) : 0.0;
  float e = min(radius * (1.0 + 0.88 * s), max_r);
  vec2 q = abs(centered_pos) - (half_size - vec2(e));
  vec2 g;
  if (q.x > 0.0 && q.y > 0.0) {
    g = s > 0.001 ? pow(q / e, vec2(1.0 + 5.0 * s)) : q;
  } else {
    g = q.x > q.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
  }
  return normalize(g) * sign(centered_pos + 1e-6);
}

float rounded_rect_distance(
    vec2 size, vec2 position, float radius_tl, float radius_tr, float radius_bl, float radius_br
) {
  return rounded_rect_distance_at(gl_FragCoord.xy, size, position, radius_tl, radius_tr, radius_bl, radius_br);
}

// Glass rim band in render px. Frosted (liquid 0): a 16 px specular rim, at most a quarter of the short side. Liquid:
// a lens bezel of 16 to 64 logical px; a shape narrower than two bezels is lens across its whole width.
float liquid_band(vec2 size, float liquid, float scale) {
  float share = liquid > 0.0 ? 0.5 : 0.25;
  return min(min(size.x, size.y) * share, (16.0 + 48.0 * liquid) * scale);
}

// Sag of a circular convex bezel: 0 at the band's inner end (t = 0), 1 at the edge, steepest at the edge, so the
// backdrop bends hardest at the rim and the flat interior has no seam.
float lens_sag(float t) { return 1.0 - sqrt(1.0 - t * t); }

// Physical refraction: the inward shift, in band units, of a vertical view ray bent by Snell's law (TOKEN_LENS_IOR)
// through a convex squircle bezel of height (1 - t^4)^(1/4) bands resting on the backdrop. 0 in the flat interior,
// about 0.44 near the edge, falling to 0 at the edge where the glass is thin.
float lens_shift(float t) {
  float t2 = t * t;
  float h = sqrt(sqrt(max(1.0 - t2 * t2, 1e-4)));
  vec2 n = normalize(vec2(t2 * t / (h * h * h), 1.0));
  vec2 r = refract(vec2(0.0, -1.0), n, 1.0 / TOKEN_LENS_IOR);
  return h * -r.x / max(-r.y, 1e-3);
}

// Whole-surface magnification of small glass at full lens (main() cannot see the token defines, appended after it).
float lens_magnify() { return TOKEN_LENS_MAGNIFY; }

// The sharp backdrop seen through the lens at uv, which sits `bend` (uv units) inward of the unbent point: red bends
// TOKEN_LENS_DISPERSION less and blue more, the colour fringe of real glass.
vec4 lens_sample(sampler2D tex, vec2 uv, vec2 bend) {
  vec4 color = texture2D(tex, clamp(uv, 0.0, 1.0));
  color.r = texture2D(tex, clamp(uv + bend * TOKEN_LENS_DISPERSION, 0.0, 1.0)).r;
  color.b = texture2D(tex, clamp(uv - bend * TOKEN_LENS_DISPERSION, 0.0, 1.0)).b;
  return color;
}

// Smooth union of two distances: the shapes join with a fillet where they come within k of each other.
float smooth_union(float a, float b, float k) {
  float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
  return mix(b, a, h) - k * h * (1.0 - h);
}

// How near the pointer (light.xy, reach light.z render px) is to p: a Gaussian, 1 at it, 2 % at the reach. It depends on
// distance only, so the light slides along the rim with no side flip when the pointer crosses the edge.
float pointer_near(vec2 p, vec3 light) {
  if (light.z <= 0.0) {
    return 0.0;
  }
  float x = length(light.xy - p) / (0.5 * light.z);
  return exp(-x * x);
}

// The specular rim: 1 at the clip edge, easing to 0 TOKEN_LENS_RIM_FALLOFF logical px inside it; d = signed distance, px.
float rim_edge(float d, float scale) {
  return smoothstep(0.0, 1.0, 1.0 + (d + 0.5) / (TOKEN_LENS_RIM_FALLOFF * scale));
}

// How much a rim pixel at p (outward normal) faces the fixed top-left key light, y-down, plus the pointer light
// (near = pointer_near): a key glint, a back glint opposite, a faint even rim between.
float rim_facing(vec2 normal, float near) {
  float l = dot(normal, vec2(-0.70710678));
  return 0.1 + pow(max(l, 0.0), 8.0) + 0.4 * pow(max(-l, 0.0), 8.0) + TOKEN_LENS_RIM_POINTER_BOOST * near;
}

#ifdef CORNER_FLARES
// Coverage of the concave flare of the corner at `corner` (sign = outward direction from the box centre), radius
// f > 0, along x (`along_x`) or y; -1 when rel is outside that flare's band.
float flare_coverage(vec2 rel, vec2 corner, vec2 outward, float f, bool along_x) {
  vec2 d = rel - corner;
  vec2 band = along_x ? vec2(d.x * outward.x, -d.y * outward.y) : vec2(-d.x * outward.x, d.y * outward.y);
  if (band.x <= 0.0 || band.y < 0.0 || band.x > f || band.y > f) {
    return -1.0;
  }
  return clamp(length(band - vec2(f, f)) - f + 0.5, 0.0, 1.0);
}

// A negative radius is a concave flare of that size outside the box (only in CORNER_FLARES programs: the extra code
// costs every glass pixel on Gen9 even when no radius is negative): the two negative corners of one edge flare
// along that edge (a panel joined to a bar).
float flare_alpha(vec2 rel, vec2 size, float tl, float tr, float bl, float br) {
  float c = -1.0;
  if (tl < 0.0) c = max(c, flare_coverage(rel, vec2(0.0), vec2(-1.0, -1.0), -tl, tr < 0.0));
  if (tr < 0.0) c = max(c, flare_coverage(rel, vec2(size.x, 0.0), vec2(1.0, -1.0), -tr, tl < 0.0));
  if (bl < 0.0) c = max(c, flare_coverage(rel, vec2(0.0, size.y), vec2(-1.0, 1.0), -bl, br < 0.0));
  if (br < 0.0) c = max(c, flare_coverage(rel, size, vec2(1.0, 1.0), -br, bl < 0.0));
  return c;
}

// A box with flares (negative radii) includes them: the body is the box less the flares on the two sides of the
// joined edge.
void flare_body(inout vec2 size, inout vec2 position, float tl, float tr, float bl, float br) {
  vec4 f = max(-vec4(tl, tr, bl, br), 0.0);
  if ((tl < 0.0 && tr < 0.0) || (bl < 0.0 && br < 0.0)) {
    vec2 cut = vec2(max(f.x, f.z), max(f.y, f.w));
    position.x += cut.x;
    size.x -= cut.x + cut.y;
  } else {
    vec2 cut = vec2(max(f.x, f.y), max(f.z, f.w));
    position.y += cut.x;
    size.y -= cut.x + cut.y;
  }
}

#endif

// The outward unit normal of body_distance_at.
vec2 body_normal_at(vec2 point, vec2 size, vec2 position, float tl, float tr, float bl, float br) {
#ifdef CORNER_FLARES
  if (min(min(tl, tr), min(bl, br)) < 0.0) {
    flare_body(size, position, tl, tr, bl, br);
  }
  return rounded_rect_normal_at(point, size, position, max(tl, 0.0), max(tr, 0.0), max(bl, 0.0), max(br, 0.0));
#else
  return rounded_rect_normal_at(point, size, position, tl, tr, bl, br);
#endif
}

// The signed distance to the body of a box whose negative radii are flares (the rim and lens outline).
float body_distance_at(vec2 point, vec2 size, vec2 position, float tl, float tr, float bl, float br) {
#ifdef CORNER_FLARES
  if (min(min(tl, tr), min(bl, br)) < 0.0) {
    flare_body(size, position, tl, tr, bl, br);
  }
  return rounded_rect_distance_at(point, size, position, max(tl, 0.0), max(tr, 0.0), max(bl, 0.0), max(br, 0.0));
#else
  return rounded_rect_distance_at(point, size, position, tl, tr, bl, br);
#endif
}

// Returns 0.0 outside and 1.0 inside the rounded rectangle. A cutout reverses
// the result so the rectangle interior becomes transparent.
float corner_alpha(
    vec2 size, vec2 position, bool is_cutout, float radius_tl, float radius_tr, float radius_bl, float radius_br
) {
#ifdef CORNER_FLARES
  if (min(min(radius_tl, radius_tr), min(radius_bl, radius_br)) < 0.0) {
    flare_body(size, position, radius_tl, radius_tr, radius_bl, radius_br);
    float flare = flare_alpha(gl_FragCoord.xy - position, size, radius_tl, radius_tr, radius_bl, radius_br);
    if (flare >= 0.0) {
      return is_cutout ? 1.0 - flare : flare;
    }
    radius_tl = max(radius_tl, 0.0);
    radius_tr = max(radius_tr, 0.0);
    radius_bl = max(radius_bl, 0.0);
    radius_br = max(radius_br, 0.0);
  }
#endif
  // Exact shortcuts: 1 px inside the part no corner curve reaches, or 1 px outside the box, the smoothstep below is
  // 0 or 1. Most fragments of a large surface take the first one.
  vec2 rel = gl_FragCoord.xy - position;
  float radius = max(max(radius_tl, radius_tr), max(radius_bl, radius_br));
  float inset = min(radius * (1.0 + 0.88 * TOKEN_RADIUS_SMOOTHING), min(size.x, size.y) * 0.5) + 1.0;
  if (rel.x >= inset && rel.y >= inset && rel.x <= size.x - inset && rel.y <= size.y - inset) {
    return is_cutout ? 0.0 : 1.0;
  }
  if (rel.x <= -1.0 || rel.y <= -1.0 || rel.x >= size.x + 1.0 || rel.y >= size.y + 1.0) {
    return is_cutout ? 1.0 : 0.0;
  }
  float dist = rounded_rect_distance(size, position, radius_tl, radius_tr, radius_bl, radius_br);
  float result = smoothstep(0.0, 1.0, dist);
  return is_cutout ? result : 1.0 - result;
}
