#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace term {

  // The terminal grid in device pixels, for image sizes in cells and percent.
  struct CellGrid {
    int cols = 80;
    int rows = 24;
    int cellWidth = 8;
    int cellHeight = 16;
  };

  // iTerm2 inline image arguments (the keys before ':' in OSC 1337 ; File=).
  struct ImageArgs {
    std::string width = "auto";  // N (cells), Npx, N% or auto
    std::string height = "auto";
    bool preserveAspect = true;
    bool inlineImage = false;    // 0: a download, which the terminal drops
  };

  [[nodiscard]] ImageArgs parseImageArgs(std::string_view args);
  // The size in pixels to draw an image of `imageWidth` x `imageHeight` px; nullopt when an extent is invalid. The
  // width never exceeds the grid width.
  [[nodiscard]] std::optional<std::pair<int, int>> imageTargetSize(const ImageArgs& args, int imageWidth, int imageHeight,
                                                                   const CellGrid& grid);

  // libghostty has no iTerm2 image protocol. The filter rewrites each inline image (OSC 1337 ; File=args:base64, or
  // MultipartFile, FilePart..., FileEnd) before libghostty sees it into a kitty graphics image at the cursor; the
  // cursor ends right of the image on its last row. Other OSC 1337 keys (SetUserVar, CurrentDir) pass unchanged.
  class ImageFilter {
  public:
    // Appends `in`, rewritten, to `out`. Bytes may split a sequence anywhere; state carries over.
    void feed(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, const CellGrid& grid);
    bool idle() const { return m_state == State::Ground; }

  private:
    enum class State : std::uint8_t { Ground, Prefix, Key, Payload, PayloadEscape, Skip, SkipEscape };
    void flush(std::vector<std::uint8_t>& out);
    void finish(std::vector<std::uint8_t>& out, const CellGrid& grid);
    void draw(std::string_view args, std::string_view data, std::vector<std::uint8_t>& out, const CellGrid& grid);

    State m_state = State::Ground;
    std::string m_held;    // ESC ] 1337 ; and the key, sent unchanged when the key is not an image key
    std::string m_payload; // after the key's '='
    std::optional<std::string> m_multipart; // MultipartFile arguments until FileEnd
    std::string m_parts;                    // base64 of the FileParts so far
  };

} // namespace term
