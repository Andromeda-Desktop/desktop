#include "render/core/icon_color_scheme.h"

#include <algorithm>
#include <mutex>
#include <string_view>

namespace icon_color_scheme {

  namespace {

    struct Scheme {
      std::vector<std::string> roots;
      std::shared_ptr<const std::string> stylesheet;
    };

    struct State {
      std::mutex mutex;
      std::shared_ptr<const Scheme> scheme;
    };

    State& state() {
      static State s;
      return s;
    }

  } // namespace

  void set(std::vector<std::string> roots, std::string stylesheet) {
    std::shared_ptr<const Scheme> scheme;
    if (!roots.empty()) {
      scheme = std::make_shared<const Scheme>(
          Scheme{.roots = std::move(roots), .stylesheet = std::make_shared<const std::string>(std::move(stylesheet))}
      );
    }
    const std::scoped_lock lock(state().mutex);
    state().scheme = std::move(scheme);
  }

  std::shared_ptr<const std::string> stylesheetFor(const std::string& path, std::span<const std::uint8_t> data) {
    std::shared_ptr<const Scheme> scheme;
    {
      const std::scoped_lock lock(state().mutex);
      scheme = state().scheme;
    }
    if (scheme == nullptr) {
      return nullptr;
    }
    const bool inTheme = std::ranges::any_of(scheme->roots, [&path](const std::string& root) {
      return path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/';
    });
    const std::string_view content(reinterpret_cast<const char*>(data.data()), data.size());
    if (!inTheme || !content.contains("current-color-scheme")) {
      return nullptr;
    }
    return scheme->stylesheet;
  }

} // namespace icon_color_scheme
