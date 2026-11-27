#define EFFECTS % d
#define COMPOSITE % d
#define CORNERS % d
#define GLASS % d

#if !defined(EFFECTS) || !defined(COMPOSITE) || !defined(CORNERS) || !defined(GLASS)
#error "Missing shader preamble"
#endif
#if GLASS && !(COMPOSITE && CORNERS)
#error "GLASS needs COMPOSITE and CORNERS"
#endif

precision highp float;

varying highp vec2 v_texcoord;
uniform sampler2D tex;

uniform float radius;
uniform vec2 halfpixel;
uniform vec4 sample_bounds;

#if EFFECTS
uniform float brightness;
uniform float contrast;
uniform float saturation;
uniform float noise;
// wlr_color_transfer_function of SDR values in a linear work buffer, or 0 when
// it is already encoded
#define TRANSFER_FUNCTION_SRGB 1
#define TRANSFER_FUNCTION_GAMMA22 8
uniform int linear;

float srgb_channel_to_linear(float x) { return x > 0.04045 ? pow((x + 0.055) / 1.055, 2.4) : x / 12.92; }

vec3 srgb_color_to_linear(vec3 color) {
  return vec3(srgb_channel_to_linear(color.r), srgb_channel_to_linear(color.g), srgb_channel_to_linear(color.b));
}

float linear_channel_to_srgb(float x) { return x > 0.0031308 ? 1.055 * pow(x, 1.0 / 2.4) - 0.055 : 12.92 * x; }

vec3 linear_color_to_srgb(vec3 color) {
  return vec3(linear_channel_to_srgb(color.r), linear_channel_to_srgb(color.g), linear_channel_to_srgb(color.b));
}

mat4 brightnessMatrix() {
  float b = brightness - 1.0;
  // clang-format off
  return mat4(1, 0, 0, 0,
              0, 1, 0, 0,
              0, 0, 1, 0,
              b, b, b, 1);
  // clang-format on
}

mat4 contrastMatrix() {
  float t = (1.0 - contrast) / 2.0;
  // clang-format off
  return mat4(contrast, 0, 0, 0,
              0, contrast, 0, 0,
              0, 0, contrast, 0,
              t, t, t, 1);
  // clang-format on
}

mat4 saturationMatrix() {
  vec3 luminance = vec3(0.3086, 0.6094, 0.0820) * (1.0 - saturation);
  vec3 red = vec3(luminance.x);
  red.x += saturation;
  vec3 green = vec3(luminance.y);
  green.y += saturation;
  vec3 blue = vec3(luminance.z);
  blue.z += saturation;
  // clang-format off
  return mat4(red, 0,
              green, 0,
              blue, 0,
              0, 0, 0, 1);
  // clang-format on
}

float noiseAmount(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 1689.1984);
  p3 += dot(p3, p3.yzx + 33.33);
  float hash = fract((p3.x + p3.y) * p3.z);
  return (mod(hash, 1.0) - 0.5) * noise;
}
#endif

#if COMPOSITE
// The final upsample draws straight into the pass target, in place of storing
// the full-resolution level and drawing that as a texture. An 8-bit level
// would clamp to [0, 1] before alpha and corners scale it, so `clamp_level`
// reproduces that; a half-float level keeps the range.
uniform float alpha;
uniform int clamp_level;

#if CORNERS
uniform vec2 size;
uniform vec2 position;
uniform float radius_top_left;
uniform float radius_top_right;
uniform float radius_bottom_left;
uniform float radius_bottom_right;

uniform vec2 clip_size;
uniform vec2 clip_position;
uniform float clip_radius_top_left;
uniform float clip_radius_top_right;
uniform float clip_radius_bottom_left;
uniform float clip_radius_bottom_right;

float corner_alpha(
    vec2 size, vec2 position, bool is_cutout, float radius_tl, float radius_tr, float radius_bl, float radius_br
);

#if GLASS
// The glass block of tex.frag, applied to the composite in place of a second full-resolution draw. See tex.frag for
// the meaning of each uniform; v_texcoord addresses the full-size texture, as the tex draw's would.
uniform float liquid;
uniform float rim;
uniform float lens;
uniform sampler2D lens_tex;
uniform vec4 tint_mix;
uniform vec2 liquid_texel;
uniform vec2 liquid_size;
uniform vec2 liquid_position;
uniform float liquid_radius;
// Render px per logical px.
uniform float liquid_scale;
uniform vec3 light;
// Merged glass: liquid_count >= 2 shapes (x, y, w, h render px) join by a smooth union over liquid_merge px; the union
// is the coverage.
uniform int liquid_count;
uniform vec4 liquid_shapes[4];
uniform float liquid_radii[4];
uniform float liquid_merge;

float rounded_rect_distance_at(
    vec2 point, vec2 size, vec2 position, float radius_tl, float radius_tr, float radius_bl, float radius_br
);
float body_distance_at(vec2 point, vec2 size, vec2 position, float tl, float tr, float bl, float br);
vec2 rounded_rect_normal_at(
    vec2 point, vec2 size, vec2 position, float radius_tl, float radius_tr, float radius_bl, float radius_br
);
vec2 body_normal_at(vec2 point, vec2 size, vec2 position, float tl, float tr, float bl, float br);
float liquid_band(vec2 size, float liquid, float scale);
float lens_shift(float t);
float lens_magnify();
vec4 lens_sample(sampler2D tex, vec2 uv, vec2 bend);
float lens_cover(float t, float lens, bool whole);
float glass_specular(float shine, float d, float band, float scale, vec2 normal, float near);
float pointer_near(vec2 p, vec3 light);
float smooth_union(float a, float b, float k);

float liquid_distance(vec2 point) {
  if (liquid_count >= 2) {
    float d = rounded_rect_distance_at(
        point, liquid_shapes[0].zw, liquid_shapes[0].xy, liquid_radii[0], liquid_radii[0], liquid_radii[0], liquid_radii[0]
    );
    for (int i = 1; i < 4; ++i) {
      if (i >= liquid_count) {
        break;
      }
      float r = liquid_radii[i];
      d = smooth_union(d, rounded_rect_distance_at(point, liquid_shapes[i].zw, liquid_shapes[i].xy, r, r, r, r),
                       liquid_merge);
    }
    return d;
  }
  if (liquid_radius >= 0.0) {
    return rounded_rect_distance_at(
        point, liquid_size, liquid_position, liquid_radius, liquid_radius, liquid_radius, liquid_radius
    );
  }
  return body_distance_at(
      point, size, position, radius_top_left, radius_top_right, radius_bottom_left, radius_bottom_right
  );
}

// The outward unit normal of liquid_distance: closed form for one shape, a central difference over merged shapes.
vec2 liquid_normal(vec2 p) {
  if (liquid_count >= 2) {
    vec2 g = vec2(liquid_distance(p + vec2(1.0, 0.0)) - liquid_distance(p - vec2(1.0, 0.0)),
                  liquid_distance(p + vec2(0.0, 1.0)) - liquid_distance(p - vec2(0.0, 1.0)));
    return length(g) > 0.0 ? normalize(g) : vec2(0.0);
  }
  if (liquid_radius >= 0.0) {
    return rounded_rect_normal_at(
        p, liquid_size, liquid_position, liquid_radius, liquid_radius, liquid_radius, liquid_radius
    );
  }
  return body_normal_at(p, size, position, radius_top_left, radius_top_right, radius_bottom_left, radius_bottom_right);
}
#endif
#endif
#endif

vec4 sample_texel(vec2 uv) { return texture2D(tex, clamp(uv, sample_bounds.xy, sample_bounds.zw)); }

void main() {
  vec2 texcoord = v_texcoord;
#if COMPOSITE && GLASS
  float specular = 0.0;
  float lens_weight = 0.0;
  vec2 lens_uv = texcoord;
  vec2 lens_bend = vec2(0.0);
  float shine = max(liquid, rim);
  if (shine > 0.0) {
    float band = liquid_band(liquid_size, liquid, liquid_scale);
    vec2 p = gl_FragCoord.xy;
    float d = liquid_distance(p);
    // A shape narrower than two bands is lens across its whole surface: no flat interior to skip.
    bool whole = band >= 0.5 * min(liquid_size.x, liquid_size.y) - 0.5;
    if (d > -band || whole) {
      vec2 normal = liquid_normal(p);
      float t = clamp(1.0 + d / band, 0.0, 1.0);
      float shift = lens_shift(t) * band;
      texcoord -= normal * (shift * liquid) * liquid_texel;
      lens_bend = normal * (shift * lens) * liquid_texel;
      if (whole) {
        // Small glass magnifies its whole surface toward the centre, the rim bending hardest.
        lens_bend += (p - (liquid_position + 0.5 * liquid_size)) * (lens_magnify() * lens) * liquid_texel;
      }
      lens_uv = texcoord - lens_bend;
      lens_weight = lens_cover(t, lens, whole);
      // The pointer light also bleeds softly into the band, so it reads as light on glass, not a thicker line.
      float near = pointer_near(p, light);
      specular = glass_specular(shine, d, band, liquid_scale, normal, near);
    }
  }
#endif
  vec2 uv = texcoord / 2.0;

  vec4 sum = sample_texel(uv + vec2(-halfpixel.x * 2.0, 0.0) * radius);

  sum += sample_texel(uv + vec2(-halfpixel.x, halfpixel.y) * radius) * 2.0;
  sum += sample_texel(uv + vec2(0.0, halfpixel.y * 2.0) * radius);
  sum += sample_texel(uv + vec2(halfpixel.x, halfpixel.y) * radius) * 2.0;
  sum += sample_texel(uv + vec2(halfpixel.x * 2.0, 0.0) * radius);
  sum += sample_texel(uv + vec2(halfpixel.x, -halfpixel.y) * radius) * 2.0;
  sum += sample_texel(uv + vec2(0.0, -halfpixel.y * 2.0) * radius);
  sum += sample_texel(uv + vec2(-halfpixel.x, -halfpixel.y) * radius) * 2.0;

  vec4 color = sum / 12.0;

#if EFFECTS
  float color_alpha = color.a;
  // Blur controls are defined for gamma-encoded content. Convert FP16 linear
  // work-buffer samples so HDR rendering preserves the SDR blur appearance.
  if (linear == TRANSFER_FUNCTION_GAMMA22 && color_alpha != 0.0) {
    color.rgb = pow(max(color.rgb / color_alpha, vec3(0.0)), vec3(1.0 / 2.2)) * color_alpha;
  } else if (linear == TRANSFER_FUNCTION_SRGB && color_alpha != 0.0) {
    color.rgb = linear_color_to_srgb(max(color.rgb / color_alpha, vec3(0.0))) * color_alpha;
  }
  // Do *not* transpose the combined matrix when multiplying
  color = brightnessMatrix() * contrastMatrix() * saturationMatrix() * color;
  color.xyz += noiseAmount(texcoord);
  if (linear == TRANSFER_FUNCTION_GAMMA22 && color_alpha != 0.0) {
    color.rgb = pow(max(color.rgb / color_alpha, vec3(0.0)), vec3(2.2)) * color_alpha;
  } else if (linear == TRANSFER_FUNCTION_SRGB && color_alpha != 0.0) {
    color.rgb = srgb_color_to_linear(color.rgb / color_alpha) * color_alpha;
  }
#endif

#if COMPOSITE
  if (clamp_level != 0) {
    color = clamp(color, 0.0, 1.0);
  }
#if GLASS
  // The bezel bends the blurred backdrop (this pass's half-size input), never sharp detail.
  if (lens_weight > 0.0) {
    color = mix(color, lens_sample(tex, lens_uv / 2.0, lens_bend / 2.0), lens_weight);
  }
  color.rgb = mix(color.rgb, tint_mix.rgb * color.a, tint_mix.a);
  // Screen, not add: the rim lifts what is below white, so over a bright backdrop it stays a highlight, not a white band.
  color.rgb += specular * (color.a - color.rgb);
#endif
#if CORNERS
  float quad_corner_alpha = corner_alpha(
      size - 0.5, position + 0.25, false, radius_top_left, radius_top_right, radius_bottom_left, radius_bottom_right
  );
  float clip_corner_alpha = corner_alpha(
      clip_size - 1.0, clip_position + 0.5, true, clip_radius_top_left, clip_radius_top_right, clip_radius_bottom_left,
      clip_radius_bottom_right
  );
#if GLASS
  quad_corner_alpha *= liquid_count >= 2 ? clamp(0.5 - liquid_distance(gl_FragCoord.xy), 0.0, 1.0) : 1.0;
#endif
  gl_FragColor = color * alpha * quad_corner_alpha * clip_corner_alpha;
#else
  gl_FragColor = color * alpha;
#endif
#else
  gl_FragColor = color;
#endif
}
