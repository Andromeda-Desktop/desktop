#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Icon themes whose index.theme sets FollowsColorScheme, drawn as KDE apps draw them: an SVG under one of the
// theme roots with a `current-color-scheme` style takes its ColorScheme-* classes from the shell palette.
namespace icon_color_scheme {

  // Later SVG loads under `roots` render with `stylesheet`; no roots turns it off. Callers reload the icons they show.
  void set(std::vector<std::string> roots, std::string stylesheet);

  // The stylesheet for the SVG at `path` with contents `data`, or null when it is not a color-scheme icon of a
  // following theme. Thread safe: texture workers call it.
  std::shared_ptr<const std::string> stylesheetFor(const std::string& path, std::span<const std::uint8_t> data);

} // namespace icon_color_scheme
