#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

// A package dependency that loads on first use instead of at process start. Include the library's own headers, so
// each member keeps the declared signature, and take only its compile flags (meson partial_dependency):
//   #define CUPS_FUNCTIONS(X) X(cupsGetDests2) X(cupsFreeDests)
//   LAZY_LIBRARY(CupsApi, cups, "libcups.so.2", CUPS_FUNCTIONS)
//   cups().cupsFreeDests(n, dests);
// The getter is thread-safe and keeps the library loaded. A missing library or symbol is a broken install, as a
// missing DT_NEEDED entry was: the getter prints the loader error and aborts.
#define LAZY_LIBRARY(Type, getter, soname, FUNCTIONS)                                                                  \
  struct Type {                                                                                                        \
    FUNCTIONS(LAZY_LIBRARY_MEMBER_)                                                                                    \
  };                                                                                                                   \
  [[maybe_unused]] inline const Type& getter() {                                                                       \
    static const Type table = [] {                                                                                     \
      Type t;                                                                                                          \
      void* handle = lazy_library::open(soname);                                                                       \
      FUNCTIONS(LAZY_LIBRARY_RESOLVE_)                                                                                 \
      return t;                                                                                                        \
    }();                                                                                                               \
    return table;                                                                                                      \
  }

#define LAZY_LIBRARY_MEMBER_(name) decltype(&::name) name = nullptr;
#define LAZY_LIBRARY_RESOLVE_(name) lazy_library::resolve(handle, #name, t.name);

namespace lazy_library {

  [[noreturn]] inline void fail() {
    std::fprintf(stderr, "fatal: %s\n", dlerror());
    std::abort();
  }

  inline void* open(const char* soname) {
    void* handle = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      fail();
    }
    return handle;
  }

  template <typename Fn> void resolve(void* handle, const char* name, Fn& out) {
    void* symbol = dlsym(handle, name);
    if (symbol == nullptr) {
      fail();
    }
    std::memcpy(&out, &symbol, sizeof(out));
  }

} // namespace lazy_library
