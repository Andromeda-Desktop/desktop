#pragma once

#include <dlfcn.h>
#include <malloc.h>

#include <cstddef>
#include <cstring>

// Hands free heap pages back to the OS (C12), on Linux. Under jemalloc (linked by the compositor, preloaded by the
// shell launcher) it purges every arena; mallctl is looked up because a preloaded jemalloc's static TLS cannot load
// through dlopen. Else glibc malloc_trim. libdesk-ui code calls sys::releaseFreeHeap, which uses this.
namespace heap {

  // Returns 0 or the jemalloc purge error.
  inline int releaseFree() {
    using Mallctl = int (*)(const char*, void*, std::size_t*, void*, std::size_t);
    if (void* symbol = dlsym(RTLD_DEFAULT, "mallctl"); symbol != nullptr) {
      Mallctl mallctl = nullptr;
      std::memcpy(&mallctl, &symbol, sizeof(mallctl));
      // 4096 is MALLCTL_ARENAS_ALL: every arena.
      return mallctl("arena.4096.purge", nullptr, nullptr, nullptr, 0);
    }
#ifdef __GLIBC__
    malloc_trim(0);
#endif
    return 0;
  }

} // namespace heap
