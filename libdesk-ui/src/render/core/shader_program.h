#pragma once

#include <GLES2/gl2.h>

#include <functional>
#include <string>

class ShaderProgram {
public:
  ShaderProgram() = default;
  ~ShaderProgram();

  ShaderProgram(const ShaderProgram&) = delete;
  ShaderProgram& operator=(const ShaderProgram&) = delete;

  ShaderProgram(ShaderProgram&& other) noexcept;
  ShaderProgram& operator=(ShaderProgram&& other) noexcept;

  void create(const char* vertexSource, const char* fragmentSource);
  void destroy();
  // Forget the GL name without deleting it. Used after a robust-context reset, when the
  // program is already invalid and no context is current to delete it from.
  void abandon() noexcept;

  // Frees the driver's shader compiler state if a program compiled since the last call; true when it did.
  static bool releaseCompiler();

  // Loads, into the current context, the cached programs that appId created before its last first frame. A later
  // create() of the same sources, in a context that shares with this one, adopts the loaded program. Runs off the
  // main thread; started() runs once create() would wait for the loads, and no create() may run before it.
  static void preloadCached(const std::string& appId, const std::function<void()>& started);
  // Saves which cached programs were created up to now as the preload list of the preloadCached() app.
  static void saveUsedForPreload();

  [[nodiscard]] bool isValid() const noexcept;
  [[nodiscard]] GLuint id() const noexcept;

private:
  GLuint m_program = 0;
};
