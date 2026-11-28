#include "render/fx_renderer/shaders.h"

#include "render/egl.h"
#include "render/fx_renderer/fx_renderer.h"

#include <EGL/egl.h>
#include <GLES2/gl2ext.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <andromedafx/types/fx/clipped_region.h>
#include <wlr/util/log.h>

// shaders
#include "GLES2/gl2.h"
#include "blur1_frag_src.h"
#include "blur2_frag_src.h"
#include "border_frag_src.h"
#include "box_shadow_frag_src.h"
#include "common_vert_src.h"
#include "corner_alpha_frag_src.h"
#include "gradient_frag_src.h"
#include "output_frag_src.h"
#include "quad_frag_src.h"
#include "quad_grad_frag_src.h"
#include "quad_grad_round_frag_src.h"
#include "quad_round_frag_src.h"
#include "tex_frag_src.h"

GLuint compile_shader(GLuint type, const GLchar* src) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, NULL);
  glCompileShader(shader);

  GLint ok;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok == GL_FALSE) {
    char log[4096];
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    wlr_log(WLR_ERROR, "Failed to compile shader: %s", log);
    glDeleteShader(shader);
    shader = 0;
  }

  return shader;
}

// Linked program binaries in $XDG_CACHE_HOME/andromeda/fx-programs, keyed by both sources and the driver; the driver
// rejects a stale binary, which falls back to compile and link. Set once at renderer creation, read-only afterwards.
static struct {
  PFNGLPROGRAMBINARYOESPROC load;
  PFNGLGETPROGRAMBINARYOESPROC get;
  char dir[PATH_MAX];
} program_cache;

void program_cache_init(const char* exts) {
  if (program_cache.load || !check_gl_ext(exts, "GL_OES_get_program_binary")) {
    return;
  }
  GLint formats = 0;
  glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS_OES, &formats);
  const char* xdg = getenv("XDG_CACHE_HOME");
  const char* home = getenv("HOME");
  int n;
  if (xdg && xdg[0] == '/') {
    n = snprintf(program_cache.dir, sizeof(program_cache.dir), "%s/andromeda/fx-programs", xdg);
  } else if (home && home[0] == '/') {
    n = snprintf(program_cache.dir, sizeof(program_cache.dir), "%s/.cache/andromeda/fx-programs", home);
  } else {
    return;
  }
  if (formats <= 0 || n <= 0 || (size_t)n >= sizeof(program_cache.dir)) {
    return;
  }
  load_gl_proc(&program_cache.load, "glProgramBinaryOES");
  load_gl_proc(&program_cache.get, "glGetProgramBinaryOES");
}

static uint64_t fnv1a(uint64_t h, const char* s) {
  for (; s && *s; s++) {
    h = (h ^ (unsigned char)*s) * 0x100000001b3ULL;
  }
  return (h ^ 0xff) * 0x100000001b3ULL;
}

static bool program_cache_path(const GLchar* frag_src, char* path, size_t size) {
  if (!program_cache.load) {
    return false;
  }
  uint64_t h = 0xcbf29ce484222325ULL;
  h = fnv1a(h, common_vert_src);
  h = fnv1a(h, frag_src);
  h = fnv1a(h, (const char*)glGetString(GL_RENDERER));
  h = fnv1a(h, (const char*)glGetString(GL_VERSION));
  int n = snprintf(path, size, "%s/%016llx.bin", program_cache.dir, (unsigned long long)h);
  return n > 0 && (size_t)n < size;
}

static GLuint program_cache_load(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return 0;
  }
  GLenum format;
  void* blob = NULL;
  long size = 0;
  if (fread(&format, sizeof(format), 1, f) == 1 && fseek(f, 0, SEEK_END) == 0) {
    size = ftell(f) - (long)sizeof(format);
  }
  if (size > 0 && fseek(f, sizeof(format), SEEK_SET) == 0) {
    blob = malloc(size);
    if (blob && fread(blob, size, 1, f) != 1) {
      free(blob);
      blob = NULL;
    }
  }
  fclose(f);
  if (!blob) {
    return 0;
  }
  GLuint prog = glCreateProgram();
  program_cache.load(prog, format, blob, size);
  free(blob);
  GLint ok = GL_FALSE;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (ok == GL_FALSE) {
    glDeleteProgram(prog);
    while (glGetError() != GL_NO_ERROR) {
    }
    return 0;
  }
  return prog;
}

static fx_file_writer file_writer;

void fx_renderer_set_file_writer(fx_file_writer writer) { file_writer = writer; }

// The file is the binary format (GLenum) followed by the binary.
static void program_cache_save(GLuint prog, const char* path) {
  GLint size = 0;
  glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH_OES, &size);
  if (size <= 0 || !file_writer) {
    return;
  }
  unsigned char* file = malloc(sizeof(GLenum) + (size_t)size);
  if (!file) {
    return;
  }
  GLenum format = 0;
  GLsizei written = 0;
  program_cache.get(prog, size, &written, &format, file + sizeof(GLenum));
  if (written > 0) {
    memcpy(file, &format, sizeof(format));
    file_writer(path, file, sizeof(GLenum) + (size_t)written);
  }
  free(file);
  while (glGetError() != GL_NO_ERROR) {
  }
}

GLuint link_program(const GLchar* frag_src) {
  char cache_path[PATH_MAX];
  bool cached = program_cache_path(frag_src, cache_path, sizeof(cache_path));
  if (cached) {
    GLuint prog = program_cache_load(cache_path);
    if (prog) {
      return prog;
    }
  }

  GLuint vert = compile_shader(GL_VERTEX_SHADER, common_vert_src);
  if (!vert) {
    goto error;
  }

  GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
  if (!frag) {
    glDeleteShader(vert);
    goto error;
  }

  GLuint prog = glCreateProgram();
  glAttachShader(prog, vert);
  glAttachShader(prog, frag);
  glLinkProgram(prog);

  glDetachShader(prog, vert);
  glDetachShader(prog, frag);
  glDeleteShader(vert);
  glDeleteShader(frag);

  GLint ok;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (ok == GL_FALSE) {
    char log[4096];
    glGetProgramInfoLog(prog, sizeof(log), NULL, log);
    wlr_log(WLR_ERROR, "Failed to link shader: %s", log);
    glDeleteProgram(prog);
    goto error;
  }

  if (cached) {
    program_cache_save(prog, cache_path);
  }
  return prog;

error:
  return 0;
}

bool check_gl_ext(const char* exts, const char* ext) {
  size_t extlen = strlen(ext);
  const char* end = exts + strlen(exts);

  while (exts < end) {
    if (exts[0] == ' ') {
      exts++;
      continue;
    }
    size_t n = strcspn(exts, " ");
    if (n == extlen && strncmp(ext, exts, n) == 0) {
      return true;
    }
    exts += n;
  }
  return false;
}

void load_gl_proc(void* proc_ptr, const char* name) {
  void* proc = (void*)eglGetProcAddress(name);
  if (proc == NULL) {
    wlr_log(WLR_ERROR, "FX RENDERER: eglGetProcAddress(%s) failed", name);
    abort();
  }
  *(void**)proc_ptr = proc;
}

void uniform_corner_radii_set(const struct shader_corner_radii* uniform, const struct fx_corner_fradii* corners) {
  glUniform1f(uniform->top_left, corners->top_left);
  glUniform1f(uniform->top_right, corners->top_right);
  glUniform1f(uniform->bottom_left, corners->bottom_left);
  glUniform1f(uniform->bottom_right, corners->bottom_right);
}
// Shaders

bool link_quad_program(struct quad_shader* shader, bool clip) {
  GLchar quad_src_part[2048];
  GLchar quad_src[sizeof(quad_src_part) + sizeof(corner_alpha_frag_src) + 2];
  snprintf(quad_src_part, sizeof(quad_src_part), quad_frag_src, clip);
  snprintf(quad_src, sizeof(quad_src), "%s\n%s\n", quad_src_part, clip ? corner_alpha_frag_src : "");

  GLuint prog;
  shader->program = prog = link_program(quad_src);
  if (!shader->program) {
    return false;
  }

  shader->proj = glGetUniformLocation(prog, "proj");
  shader->color = glGetUniformLocation(prog, "color");
  shader->pos_attrib = glGetAttribLocation(prog, "pos");

  if (!clip) {
    return true;
  }
  shader->effects.clip_size = glGetUniformLocation(prog, "clip_size");
  shader->effects.clip_position = glGetUniformLocation(prog, "clip_position");
  shader->effects.clip_radius.top_left = glGetUniformLocation(prog, "clip_radius_top_left");
  shader->effects.clip_radius.top_right = glGetUniformLocation(prog, "clip_radius_top_right");
  shader->effects.clip_radius.bottom_left = glGetUniformLocation(prog, "clip_radius_bottom_left");
  shader->effects.clip_radius.bottom_right = glGetUniformLocation(prog, "clip_radius_bottom_right");

  return true;
}

bool link_quad_grad_program(struct quad_grad_shader* shader, int max_len) {
  GLchar quad_src_part[2048];
  GLchar quad_src[8192];
  snprintf(quad_src_part, sizeof(quad_src_part), quad_grad_frag_src, max_len);
  snprintf(quad_src, sizeof(quad_src), "%s\n%s", quad_src_part, gradient_frag_src);

  GLuint prog;
  shader->program = prog = link_program(quad_src);
  if (!shader->program) {
    return false;
  }

  shader->proj = glGetUniformLocation(prog, "proj");
  shader->pos_attrib = glGetAttribLocation(prog, "pos");
  shader->size = glGetUniformLocation(prog, "size");
  shader->colors = glGetUniformLocation(prog, "colors");
  shader->degree = glGetUniformLocation(prog, "degree");
  shader->grad_box = glGetUniformLocation(prog, "grad_box");
  shader->linear = glGetUniformLocation(prog, "linear");
  shader->origin = glGetUniformLocation(prog, "origin");
  shader->count = glGetUniformLocation(prog, "count");
  shader->blend = glGetUniformLocation(prog, "blend");

  shader->max_len = max_len;

  return true;
}

bool link_quad_round_program(struct quad_round_shader* shader) {
  GLchar quad_src[sizeof(quad_round_frag_src) + sizeof(corner_alpha_frag_src) + 1];
  snprintf(quad_src, sizeof(quad_src), "%s\n%s", quad_round_frag_src, corner_alpha_frag_src);

  GLuint prog;
  shader->program = prog = link_program(quad_src);
  if (!shader->program) {
    return false;
  }

  shader->proj = glGetUniformLocation(prog, "proj");
  shader->color = glGetUniformLocation(prog, "color");
  shader->pos_attrib = glGetAttribLocation(prog, "pos");
  shader->size = glGetUniformLocation(prog, "size");
  shader->position = glGetUniformLocation(prog, "position");
  shader->radius.top_left = glGetUniformLocation(prog, "radius_top_left");
  shader->radius.top_right = glGetUniformLocation(prog, "radius_top_right");
  shader->radius.bottom_left = glGetUniformLocation(prog, "radius_bottom_left");
  shader->radius.bottom_right = glGetUniformLocation(prog, "radius_bottom_right");

  shader->clip_size = glGetUniformLocation(prog, "clip_size");
  shader->clip_position = glGetUniformLocation(prog, "clip_position");
  shader->clip_radius.top_left = glGetUniformLocation(prog, "clip_radius_top_left");
  shader->clip_radius.top_right = glGetUniformLocation(prog, "clip_radius_top_right");
  shader->clip_radius.bottom_left = glGetUniformLocation(prog, "clip_radius_bottom_left");
  shader->clip_radius.bottom_right = glGetUniformLocation(prog, "clip_radius_bottom_right");

  shader->fade = glGetUniformLocation(prog, "fade");
  shader->fade_from = glGetUniformLocation(prog, "fade_from");
  shader->fade_to = glGetUniformLocation(prog, "fade_to");

  return true;
}

bool link_border_program(struct border_shader* shader) {
  GLchar border_src[sizeof(border_frag_src) + sizeof(corner_alpha_frag_src) + 1];
  snprintf(border_src, sizeof(border_src), "%s\n%s", border_frag_src, corner_alpha_frag_src);

  GLuint prog;
  shader->program = prog = link_program(border_src);
  if (!shader->program) {
    return false;
  }

  shader->proj = glGetUniformLocation(prog, "proj");
  shader->color = glGetUniformLocation(prog, "color");
  shader->pos_attrib = glGetAttribLocation(prog, "pos");
  shader->clip_size = glGetUniformLocation(prog, "clip_size");
  shader->clip_position = glGetUniformLocation(prog, "clip_position");
  shader->inner_radius.top_left = glGetUniformLocation(prog, "inner_radius_top_left");
  shader->inner_radius.top_right = glGetUniformLocation(prog, "inner_radius_top_right");
  shader->inner_radius.bottom_left = glGetUniformLocation(prog, "inner_radius_bottom_left");
  shader->inner_radius.bottom_right = glGetUniformLocation(prog, "inner_radius_bottom_right");
  shader->seam_radius.top_left = glGetUniformLocation(prog, "seam_radius_top_left");
  shader->seam_radius.top_right = glGetUniformLocation(prog, "seam_radius_top_right");
  shader->seam_radius.bottom_left = glGetUniformLocation(prog, "seam_radius_bottom_left");
  shader->seam_radius.bottom_right = glGetUniformLocation(prog, "seam_radius_bottom_right");
  shader->outer_radius.top_left = glGetUniformLocation(prog, "outer_radius_top_left");
  shader->outer_radius.top_right = glGetUniformLocation(prog, "outer_radius_top_right");
  shader->outer_radius.bottom_left = glGetUniformLocation(prog, "outer_radius_bottom_left");
  shader->outer_radius.bottom_right = glGetUniformLocation(prog, "outer_radius_bottom_right");
  shader->inner_width = glGetUniformLocation(prog, "inner_width");
  shader->outer_width = glGetUniformLocation(prog, "outer_width");
  shader->inner_color = glGetUniformLocation(prog, "inner_color");

  return true;
}

bool link_quad_grad_round_program(struct quad_grad_round_shader* shader, int max_len) {
  GLchar quad_src_part[2048];
  GLchar quad_src[sizeof(quad_src_part) + sizeof(gradient_frag_src) + sizeof(corner_alpha_frag_src) + 2];
  snprintf(quad_src_part, sizeof(quad_src_part), quad_grad_round_frag_src, max_len);
  snprintf(quad_src, sizeof(quad_src), "%s\n%s\n%s", quad_src_part, gradient_frag_src, corner_alpha_frag_src);

  GLuint prog;
  shader->program = prog = link_program(quad_src);
  if (!shader->program) {
    return false;
  }

  shader->proj = glGetUniformLocation(prog, "proj");
  shader->color = glGetUniformLocation(prog, "color");
  shader->pos_attrib = glGetAttribLocation(prog, "pos");
  shader->size = glGetUniformLocation(prog, "size");
  shader->position = glGetUniformLocation(prog, "position");
  shader->radius.top_left = glGetUniformLocation(prog, "radius_top_left");
  shader->radius.top_right = glGetUniformLocation(prog, "radius_top_right");
  shader->radius.bottom_left = glGetUniformLocation(prog, "radius_bottom_left");
  shader->radius.bottom_right = glGetUniformLocation(prog, "radius_bottom_right");

  shader->grad_size = glGetUniformLocation(prog, "grad_size");
  shader->colors = glGetUniformLocation(prog, "colors");
  shader->degree = glGetUniformLocation(prog, "degree");
  shader->grad_box = glGetUniformLocation(prog, "grad_box");
  shader->linear = glGetUniformLocation(prog, "linear");
  shader->origin = glGetUniformLocation(prog, "origin");
  shader->count = glGetUniformLocation(prog, "count");
  shader->blend = glGetUniformLocation(prog, "blend");

  shader->max_len = max_len;

  return true;
}

static const char kFlaresDefine[] = "#define CORNER_FLARES\n";

bool link_tex_program(
    struct tex_shader* shader, enum fx_tex_shader_source source, bool effects, bool sample_clamp, bool flares
) {
  GLchar frag_src_part[sizeof(tex_frag_src)];
  GLchar frag_src[sizeof(frag_src_part) + sizeof(kFlaresDefine) + sizeof(corner_alpha_frag_src) + 3];
  snprintf(frag_src_part, sizeof(frag_src_part), tex_frag_src, source, effects, sample_clamp);
  snprintf(
      frag_src, sizeof(frag_src), "%s\n%s%s\n", frag_src_part, effects && flares ? kFlaresDefine : "",
      effects ? corner_alpha_frag_src : ""
  );

  GLuint prog;
  shader->program = prog = link_program(frag_src);
  if (!shader->program) {
    return false;
  }

  shader->proj = glGetUniformLocation(prog, "proj");
  shader->tex = glGetUniformLocation(prog, "tex");
  shader->alpha = glGetUniformLocation(prog, "alpha");
  shader->source_tf = glGetUniformLocation(prog, "source_tf");
  shader->primaries_matrix = glGetUniformLocation(prog, "primaries_matrix");
  shader->lum_multiplier = glGetUniformLocation(prog, "lum_multiplier");
  shader->source_luminances = glGetUniformLocation(prog, "source_luminances");
  shader->target_tf = glGetUniformLocation(prog, "target_tf");
  shader->map_hdr_to_sdr = glGetUniformLocation(prog, "map_hdr_to_sdr");
  shader->tint_mix = glGetUniformLocation(prog, "tint_mix");
  shader->vibrancy = glGetUniformLocation(prog, "vibrancy");
  shader->source_tex = glGetUniformLocation(prog, "source_tex");
  shader->source_mix = glGetUniformLocation(prog, "source_mix");
  shader->pos_attrib = glGetAttribLocation(prog, "pos");
  shader->tex_proj = glGetUniformLocation(prog, "tex_proj");

  shader->discard_transparent = glGetUniformLocation(prog, "discard_transparent");
  shader->sample_bounds = sample_clamp ? glGetUniformLocation(prog, "sample_bounds") : -1;

  if (!effects) {
    return true;
  }
  shader->effects.size = glGetUniformLocation(prog, "size");
  shader->effects.position = glGetUniformLocation(prog, "position");
  shader->effects.radius.top_left = glGetUniformLocation(prog, "radius_top_left");
  shader->effects.radius.top_right = glGetUniformLocation(prog, "radius_top_right");
  shader->effects.radius.bottom_left = glGetUniformLocation(prog, "radius_bottom_left");
  shader->effects.radius.bottom_right = glGetUniformLocation(prog, "radius_bottom_right");

  shader->effects.clip_size = glGetUniformLocation(prog, "clip_size");
  shader->effects.clip_position = glGetUniformLocation(prog, "clip_position");
  shader->effects.clip_radius.top_left = glGetUniformLocation(prog, "clip_radius_top_left");
  shader->effects.clip_radius.top_right = glGetUniformLocation(prog, "clip_radius_top_right");
  shader->effects.clip_radius.bottom_left = glGetUniformLocation(prog, "clip_radius_bottom_left");
  shader->effects.clip_radius.bottom_right = glGetUniformLocation(prog, "clip_radius_bottom_right");
  shader->effects.liquid = glGetUniformLocation(prog, "liquid");
  shader->effects.rim = glGetUniformLocation(prog, "rim");
  shader->effects.lens = glGetUniformLocation(prog, "lens");
  shader->effects.lens_tex = glGetUniformLocation(prog, "lens_tex");
  shader->effects.liquid_texel = glGetUniformLocation(prog, "liquid_texel");
  shader->effects.liquid_size = glGetUniformLocation(prog, "liquid_size");
  shader->effects.liquid_position = glGetUniformLocation(prog, "liquid_position");
  shader->effects.liquid_radius = glGetUniformLocation(prog, "liquid_radius");
  shader->effects.liquid_rotation = glGetUniformLocation(prog, "liquid_rotation");
  shader->effects.liquid_scale = glGetUniformLocation(prog, "liquid_scale");
  shader->effects.light = glGetUniformLocation(prog, "light");
  shader->effects.liquid_count = glGetUniformLocation(prog, "liquid_count");
  shader->effects.liquid_shapes = glGetUniformLocation(prog, "liquid_shapes");
  shader->effects.liquid_radii = glGetUniformLocation(prog, "liquid_radii");
  shader->effects.liquid_merge = glGetUniformLocation(prog, "liquid_merge");

  return true;
}

bool link_output_program(struct output_shader* shader) {
  GLuint prog;
  shader->program = prog = link_program(output_frag_src);
  if (!shader->program) {
    return false;
  }

  shader->proj = glGetUniformLocation(prog, "proj");
  shader->tex_proj = glGetUniformLocation(prog, "tex_proj");
  shader->tex = glGetUniformLocation(prog, "tex");
  shader->matrix = glGetUniformLocation(prog, "color_matrix");
  shader->inverse_eotf = glGetUniformLocation(prog, "inverse_eotf");
  shader->lut = glGetUniformLocation(prog, "lut");
  shader->lut_dim = glGetUniformLocation(prog, "lut_dim");
  shader->has_lut = glGetUniformLocation(prog, "has_lut");
  shader->pos_attrib = glGetAttribLocation(prog, "pos");
  return true;
}

bool link_box_shadow_program(struct box_shadow_shader* shader) {
  GLchar shadow_src[sizeof(box_shadow_frag_src) + sizeof(corner_alpha_frag_src) + 1];
  snprintf(shadow_src, sizeof(shadow_src), "%s\n%s", box_shadow_frag_src, corner_alpha_frag_src);

  GLuint prog;
  shader->program = prog = link_program(shadow_src);
  if (!shader->program) {
    return false;
  }
  shader->proj = glGetUniformLocation(prog, "proj");
  shader->color = glGetUniformLocation(prog, "color");
  shader->pos_attrib = glGetAttribLocation(prog, "pos");
  shader->position = glGetUniformLocation(prog, "position");
  shader->size = glGetUniformLocation(prog, "size");
  shader->blur_sigma = glGetUniformLocation(prog, "blur_sigma");
  shader->corner_radius = glGetUniformLocation(prog, "corner_radius");
  shader->clip_position = glGetUniformLocation(prog, "clip_position");
  shader->clip_size = glGetUniformLocation(prog, "clip_size");
  shader->clip_radius.top_left = glGetUniformLocation(prog, "clip_radius_top_left");
  shader->clip_radius.top_right = glGetUniformLocation(prog, "clip_radius_top_right");
  shader->clip_radius.bottom_left = glGetUniformLocation(prog, "clip_radius_bottom_left");
  shader->clip_radius.bottom_right = glGetUniformLocation(prog, "clip_radius_bottom_right");

  return true;
}

bool link_blur1_program(struct blur_shader* shader) {
  GLuint prog;
  shader->program = prog = link_program(blur1_frag_src);
  if (!shader->program) {
    return false;
  }
  shader->proj = glGetUniformLocation(prog, "proj");
  shader->tex = glGetUniformLocation(prog, "tex");
  shader->pos_attrib = glGetAttribLocation(prog, "pos");
  shader->tex_proj = glGetUniformLocation(prog, "tex_proj");
  shader->radius = glGetUniformLocation(prog, "radius");
  shader->halfpixel = glGetUniformLocation(prog, "halfpixel");
  shader->sample_bounds = glGetUniformLocation(prog, "sample_bounds");

  return true;
}

// Links one variant of blur2.frag and loads the uniforms every variant shares.
static bool link_blur2_variant(
    struct blur2_with_effects_shader* shader, bool effects, bool composite, bool corners, bool flares, bool glass
) {
  GLchar frag_src_part[sizeof(blur2_frag_src)];
  GLchar frag_src[sizeof(frag_src_part) + sizeof(kFlaresDefine) + sizeof(corner_alpha_frag_src) + 3];
  snprintf(frag_src_part, sizeof(frag_src_part), blur2_frag_src, effects, composite, corners, glass);
  snprintf(
      frag_src, sizeof(frag_src), "%s\n%s%s\n", frag_src_part, corners && flares ? kFlaresDefine : "",
      corners ? corner_alpha_frag_src : ""
  );

  GLuint prog;
  shader->base.program = prog = link_program(frag_src);
  if (!shader->base.program) {
    return false;
  }

  shader->base.proj = glGetUniformLocation(prog, "proj");
  shader->base.tex = glGetUniformLocation(prog, "tex");
  shader->base.pos_attrib = glGetAttribLocation(prog, "pos");
  shader->base.tex_proj = glGetUniformLocation(prog, "tex_proj");
  shader->base.radius = glGetUniformLocation(prog, "radius");
  shader->base.halfpixel = glGetUniformLocation(prog, "halfpixel");
  shader->base.sample_bounds = glGetUniformLocation(prog, "sample_bounds");

  shader->linear = glGetUniformLocation(prog, "linear");
  shader->noise = glGetUniformLocation(prog, "noise");
  shader->brightness = glGetUniformLocation(prog, "brightness");
  shader->contrast = glGetUniformLocation(prog, "contrast");
  shader->saturation = glGetUniformLocation(prog, "saturation");

  return true;
}

bool link_blur2_program(struct blur_shader* shader) {
  struct blur2_with_effects_shader variant;
  if (!link_blur2_variant(&variant, false, false, false, false, false)) {
    return false;
  }
  *shader = variant.base;
  return true;
}

bool link_blur2_with_effects_program(struct blur2_with_effects_shader* shader) {
  return link_blur2_variant(shader, true, false, false, false, false);
}

bool link_blur2_composite_program(
    struct blur2_composite_shader* shader, bool effects, bool corners, bool flares, bool glass
) {
  if (!link_blur2_variant(&shader->blur, effects, true, corners, flares, glass)) {
    return false;
  }
  GLuint prog = shader->blur.base.program;
  shader->alpha = glGetUniformLocation(prog, "alpha");
  shader->clamp_level = glGetUniformLocation(prog, "clamp_level");

  shader->corners.size = glGetUniformLocation(prog, "size");
  shader->corners.position = glGetUniformLocation(prog, "position");
  shader->corners.radius.top_left = glGetUniformLocation(prog, "radius_top_left");
  shader->corners.radius.top_right = glGetUniformLocation(prog, "radius_top_right");
  shader->corners.radius.bottom_left = glGetUniformLocation(prog, "radius_bottom_left");
  shader->corners.radius.bottom_right = glGetUniformLocation(prog, "radius_bottom_right");

  shader->corners.clip_size = glGetUniformLocation(prog, "clip_size");
  shader->corners.clip_position = glGetUniformLocation(prog, "clip_position");
  shader->corners.clip_radius.top_left = glGetUniformLocation(prog, "clip_radius_top_left");
  shader->corners.clip_radius.top_right = glGetUniformLocation(prog, "clip_radius_top_right");
  shader->corners.clip_radius.bottom_left = glGetUniformLocation(prog, "clip_radius_bottom_left");
  shader->corners.clip_radius.bottom_right = glGetUniformLocation(prog, "clip_radius_bottom_right");

  shader->glass.liquid = glGetUniformLocation(prog, "liquid");
  shader->glass.rim = glGetUniformLocation(prog, "rim");
  shader->glass.vibrancy = glGetUniformLocation(prog, "vibrancy");
  shader->glass.lens = glGetUniformLocation(prog, "lens");
  shader->glass.lens_tex = glGetUniformLocation(prog, "lens_tex");
  shader->glass.tint_mix = glGetUniformLocation(prog, "tint_mix");
  shader->glass.liquid_texel = glGetUniformLocation(prog, "liquid_texel");
  shader->glass.liquid_size = glGetUniformLocation(prog, "liquid_size");
  shader->glass.liquid_position = glGetUniformLocation(prog, "liquid_position");
  shader->glass.liquid_radius = glGetUniformLocation(prog, "liquid_radius");
  shader->glass.liquid_rotation = glGetUniformLocation(prog, "liquid_rotation");
  shader->glass.liquid_scale = glGetUniformLocation(prog, "liquid_scale");
  shader->glass.light = glGetUniformLocation(prog, "light");
  shader->glass.liquid_count = glGetUniformLocation(prog, "liquid_count");
  shader->glass.liquid_shapes = glGetUniformLocation(prog, "liquid_shapes");
  shader->glass.liquid_radii = glGetUniformLocation(prog, "liquid_radii");
  shader->glass.liquid_merge = glGetUniformLocation(prog, "liquid_merge");

  return true;
}

bool ensure_tex_program(struct tex_shader* shader) {
  struct tex_shader_variant* v = &shader->variant;
  if (shader->program != 0 || v->link_failed) {
    return shader->program != 0;
  }
  if (!link_tex_program(shader, v->source, v->effects, v->sample_clamp, v->flares)) {
    v->link_failed = true;
    wlr_log(WLR_ERROR, "Could not link tex shader (source %d effects %d clamp %d flares %d)", v->source, v->effects,
        v->sample_clamp, v->flares);
    return false;
  }
  return true;
}

bool ensure_blur2_composite_program(struct blur2_composite_shader* shader) {
  struct blur2_composite_variant* v = &shader->variant;
  if (shader->blur.base.program != 0 || v->link_failed) {
    return shader->blur.base.program != 0;
  }
  if (!link_blur2_composite_program(shader, v->effects, v->corners, v->flares, v->glass)) {
    v->link_failed = true;
    wlr_log(WLR_ERROR, "Could not link blur composite shader (effects %d corners %d flares %d glass %d)", v->effects,
        v->corners, v->flares, v->glass);
    return false;
  }
  return true;
}
