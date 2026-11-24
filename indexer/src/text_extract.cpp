#include "extract.h"

#include <iconv.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>

// Text documents, read in the daemon: no library beyond libc, and no helper process to start.
namespace andromeda::index {

  namespace {
    std::optional<std::string> readHead(const std::string& path, std::size_t maxBytes) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return std::nullopt;
      }
      std::string data(maxBytes, '\0');
      in.read(data.data(), static_cast<std::streamsize>(maxBytes));
      data.resize(static_cast<std::size_t>(in.gcount()));
      return data;
    }

    // The encoding of text that holds NUL bytes: UTF-16 or UTF-32 by byte order mark, else by where the NULs of
    // ASCII characters fall in the first 4 KB; nullptr for binary data.
    const char* wideEncoding(std::string_view data) {
      const auto starts = [&](std::string_view bom) { return data.starts_with(bom); };
      if (starts(std::string_view("\xFF\xFE\0\0", 4))) {
        return "UTF-32LE";
      }
      if (starts(std::string_view("\0\0\xFE\xFF", 4))) {
        return "UTF-32BE";
      }
      if (starts("\xFF\xFE")) {
        return "UTF-16LE";
      }
      if (starts("\xFE\xFF")) {
        return "UTF-16BE";
      }
      const std::size_t n = std::min<std::size_t>(data.size(), 4096) & ~std::size_t{3};
      if (n < 16) {
        return nullptr;
      }
      std::array<std::size_t, 4> zeros{};
      for (std::size_t i = 0; i < n; ++i) {
        zeros[i % 4] += data[i] == '\0' ? 1 : 0;
      }
      const auto mostly = [&](std::size_t lane) { return zeros[lane] * 10 >= (n / 4) * 9; };
      const auto rarely = [&](std::size_t lane) { return zeros[lane] * 10 <= (n / 4); };
      if (rarely(0) && mostly(1) && mostly(2) && mostly(3)) {
        return "UTF-32LE";
      }
      if (mostly(0) && mostly(1) && mostly(2) && rarely(3)) {
        return "UTF-32BE";
      }
      if (rarely(0) && mostly(1) && rarely(2) && mostly(3)) {
        return "UTF-16LE";
      }
      if (mostly(0) && rarely(1) && mostly(2) && rarely(3)) {
        return "UTF-16BE";
      }
      return nullptr;
    }

    std::optional<std::string> plainText(const std::string& path) {
      auto data = readHead(path, kMaxText);
      if (!data || data->find('\0') >= 4096) {
        return data;
      }
      // NULs near the start: UTF-16 or UTF-32 text, or a binary file under a text extension.
      const char* encoding = wideEncoding(*data);
      if (encoding == nullptr) {
        return std::nullopt;
      }
      const iconv_t cd = iconv_open("UTF-8", encoding);
      if (cd == reinterpret_cast<iconv_t>(-1)) {
        return std::nullopt;
      }
      std::size_t inLeft = data->size() & ~std::size_t{3};
      std::string text(inLeft * 2, '\0'); // UTF-16 needs at most 3/2, UTF-32 at most 1 byte out per byte in
      char* in = data->data();
      char* out = text.data();
      std::size_t outLeft = text.size();
      const std::size_t converted = iconv(cd, &in, &inLeft, &out, &outLeft);
      iconv_close(cd);
      if (converted == static_cast<std::size_t>(-1) && errno != EINVAL) {
        return std::nullopt;
      }
      text.resize(text.size() - outLeft);
      return text;
    }

  } // namespace

  std::optional<std::string> extractPlain(const std::string& path) { return plainText(path); }

} // namespace andromeda::index
