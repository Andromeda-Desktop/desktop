#include "render/core/shader_program.h"

#include "protocols/async_file_writer.h"
#include "protocols/xdg_dirs.h"
#include "render/core/startup_metrics.h"

#include <GLES3/gl3.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

  // Linked programs on disk: Mesa's own cache keeps only the GPU code, so the GLSL compile and link still run
  // at every start without this. The driver rejects a stale or corrupt binary (build id, CRC); create() then compiles.
  struct BinaryCache {
    std::filesystem::path dir;
    std::string driver; // GL_RENDERER and GL_VERSION: part of each key
  };

  std::optional<BinaryCache> probeBinaryCache() {
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (version == nullptr || renderer == nullptr || !std::string_view(version).starts_with("OpenGL ES 3")) {
      return std::nullopt;
    }
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    const std::filesystem::path base = xdg::cacheHome();
    if (formats <= 0 || base.empty()) {
      return std::nullopt;
    }
    return BinaryCache{.dir = base / "andromeda" / "gl-programs", .driver = std::string(renderer) + '\n' + version};
  }

  // Held by preloadCached() while it loads; create() takes it before it reads the preloaded programs.
  std::mutex g_preloadMutex;
  std::string g_preloadAppId;
  std::unordered_map<std::string, GLuint> g_preloaded; // binary file name -> loaded program
  std::vector<std::string> g_used; // binary file names created up to the first frame of the preloadCached() app

  std::filesystem::path preloadListPath(const std::filesystem::path& dir, const std::string& appId) {
    return dir / ("preload-" + appId);
  }

  const std::optional<BinaryCache>& binaryCache() {
    static const std::optional<BinaryCache> cache = probeBinaryCache();
    return cache;
  }

  std::filesystem::path binaryPath(const BinaryCache& cache, const char* vertexSource, const char* fragmentSource) {
    std::uint64_t hash = 14695981039346656037ULL; // FNV-1a 64
    for (const std::string_view part : {std::string_view(vertexSource), std::string_view(fragmentSource),
                                        std::string_view(cache.driver)}) {
      for (const char c : part) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
      }
      hash = (hash ^ 0xffU) * 1099511628211ULL; // part separator
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(hash));
    return cache.dir / name;
  }

  // File layout: the GLenum binary format, then the driver's blob.
  bool loadBinary(GLuint program, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    GLenum format = 0;
    if (data.size() <= sizeof(format)) {
      return false;
    }
    std::memcpy(&format, data.data(), sizeof(format));
    glProgramBinary(program, format, data.data() + sizeof(format), static_cast<GLsizei>(data.size() - sizeof(format)));
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
      while (glGetError() != GL_NO_ERROR) {
      }
    }
    return linked == GL_TRUE;
  }

  void saveBinary(GLuint program, const std::filesystem::path& path) {
    GLint length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (length <= 0) {
      return;
    }
    GLenum format = 0;
    std::vector<char> data(sizeof(format) + static_cast<std::size_t>(length));
    GLsizei written = 0;
    glGetProgramBinary(program, length, &written, &format, data.data() + sizeof(format));
    if (written <= 0) {
      return;
    }
    std::memcpy(data.data(), &format, sizeof(format));
    andromeda::writeFileAsync(path, std::string(data.data(), sizeof(format) + static_cast<std::size_t>(written)));
  }

  GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
      throw std::runtime_error("glCreateShader failed");
    }

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
      GLint logLength = 0;
      glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
      std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
      glGetShaderInfoLog(shader, logLength, nullptr, log.data());
      glDeleteShader(shader);
      throw std::runtime_error("shader compilation failed: " + log);
    }

    return shader;
  }

  // glCompileShader since the last releaseCompiler on this thread's context.
  thread_local bool g_compilerUsed = false;

} // namespace

bool ShaderProgram::releaseCompiler() {
  if (!g_compilerUsed) {
    return false;
  }
  // Mesa frees its GLSL builtin library here (~6 MiB) and rebuilds it on the next compile.
  glReleaseShaderCompiler();
  g_compilerUsed = false;
  return true;
}

ShaderProgram::~ShaderProgram() { destroy(); }

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept : m_program(other.m_program) { other.m_program = 0; }

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  destroy();
  m_program = other.m_program;
  other.m_program = 0;
  return *this;
}

void ShaderProgram::create(const char* vertexSource, const char* fragmentSource) {
  destroy();
  const auto buildStart = std::chrono::steady_clock::now();
  const std::lock_guard lock(g_preloadMutex);
  const std::optional<BinaryCache>& cache = binaryCache();
  std::filesystem::path cached;
  if (cache.has_value()) {
    cached = binaryPath(*cache, vertexSource, fragmentSource);
    std::string name = cached.filename().string();
    if (const auto it = g_preloaded.find(name); it != g_preloaded.end()) {
      m_program = it->second;
      g_preloaded.erase(it);
      if (!g_preloadAppId.empty()) {
        g_used.push_back(std::move(name));
      }
      startup_metrics::recordProgramPreloaded();
      return;
    }
    if (!g_preloadAppId.empty()) {
      g_used.push_back(std::move(name));
    }
    m_program = glCreateProgram();
    if (m_program != 0 && loadBinary(m_program, cached)) {
      startup_metrics::recordProgramBuild(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count()
      );
      return;
    }
    destroy();
  }

  g_compilerUsed = true;
  const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
  GLuint fragmentShader = 0;
  try {
    fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
  } catch (...) {
    glDeleteShader(vertexShader);
    throw;
  }

  m_program = glCreateProgram();
  if (m_program == 0) {
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    throw std::runtime_error("glCreateProgram failed");
  }

  glAttachShader(m_program, vertexShader);
  glAttachShader(m_program, fragmentShader);
  if (cache.has_value()) {
    glProgramParameteri(m_program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
  }
  glLinkProgram(m_program);

  // An attached shader stays alive after glDeleteShader; Mesa keeps its NIR (~0.5 MiB each).
  glDetachShader(m_program, vertexShader);
  glDetachShader(m_program, fragmentShader);
  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);

  GLint linked = 0;
  glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
  if (linked == GL_FALSE) {
    GLint logLength = 0;
    glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
    glGetProgramInfoLog(m_program, logLength, nullptr, log.data());
    destroy();
    throw std::runtime_error("shader link failed: " + log);
  }
  if (cache.has_value()) {
    saveBinary(m_program, cached);
  }
  startup_metrics::recordProgramBuild(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count()
  );
}

void ShaderProgram::preloadCached(const std::string& appId, const std::function<void()>& started) {
  const std::lock_guard lock(g_preloadMutex);
  started();
  const std::optional<BinaryCache>& cache = binaryCache();
  if (!cache.has_value()) {
    return;
  }
  g_preloadAppId = appId;
  std::ifstream list(preloadListPath(cache->dir, appId));
  for (std::string name; std::getline(list, name);) {
    if (name.empty() || name.find('/') != std::string::npos || g_preloaded.contains(name)) {
      continue;
    }
    const GLuint program = glCreateProgram();
    if (program != 0 && loadBinary(program, cache->dir / name)) {
      g_preloaded.emplace(std::move(name), program);
    } else if (program != 0) {
      glDeleteProgram(program);
    }
  }
  // Another context of the share group uses these programs next.
  glFlush();
}

void ShaderProgram::saveUsedForPreload() {
  const std::lock_guard lock(g_preloadMutex);
  // Checked before binaryCache(): its first call needs a current context, which a process that never preloads may
  // not have here.
  if (g_preloadAppId.empty() || !binaryCache().has_value()) {
    return;
  }
  const std::filesystem::path path = preloadListPath(binaryCache()->dir, g_preloadAppId);
  std::string content;
  for (const std::string& name : g_used) {
    content += name + '\n';
  }
  g_preloadAppId.clear();
  g_used.clear();
  // Programs no window took stay in the root context until it goes; a graphics reset must never hand them out.
  g_preloaded.clear();
  std::ifstream current(path);
  if (std::string(std::istreambuf_iterator<char>(current), std::istreambuf_iterator<char>()) == content) {
    return;
  }
  andromeda::writeFileAsync(path, std::move(content));
}

void ShaderProgram::destroy() {
  if (m_program != 0) {
    glDeleteProgram(m_program);
    m_program = 0;
  }
}

void ShaderProgram::abandon() noexcept { m_program = 0; }

bool ShaderProgram::isValid() const noexcept { return m_program != 0; }

GLuint ShaderProgram::id() const noexcept { return m_program; }
