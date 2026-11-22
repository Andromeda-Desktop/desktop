#include "browse_model.h"

#include <algorithm>

namespace term::browse {

  namespace {
    constexpr std::uint32_t kCtrl = 1U << 1;
    constexpr std::uint32_t kAlt = 1U << 2;
    constexpr std::uint32_t kSuper = 1U << 3;

    constexpr std::uint32_t kBackSpace = 0xff08;
    constexpr std::uint32_t kReturn = 0xff0d;
    constexpr std::uint32_t kKpEnter = 0xff8d;
    constexpr std::uint32_t kEscape = 0xff1b;
    constexpr std::uint32_t kHome = 0xff50;
    constexpr std::uint32_t kLeft = 0xff51;
    constexpr std::uint32_t kUp = 0xff52;
    constexpr std::uint32_t kRight = 0xff53;
    constexpr std::uint32_t kDown = 0xff54;
    constexpr std::uint32_t kPageUp = 0xff55;
    constexpr std::uint32_t kPageDown = 0xff56;
    constexpr std::uint32_t kEnd = 0xff57;
    constexpr std::ptrdiff_t kPage = 10;

    bool enter(const Key& key) { return key.sym == kReturn || key.sym == kKpEnter; }

    // The typed character when no command modifier is held; 0 otherwise.
    char32_t typed(const Key& key) {
      if ((key.mods & (kCtrl | kAlt | kSuper)) != 0 || key.utf32 < 0x20 || key.utf32 == 0x7f) {
        return 0;
      }
      return key.utf32;
    }

    void append(std::string& text, char32_t c) {
      if (c < 0x80) {
        text += static_cast<char>(c);
      } else if (c < 0x800) {
        text += static_cast<char>(0xc0 | (c >> 6));
        text += static_cast<char>(0x80 | (c & 0x3f));
      } else if (c < 0x10000) {
        text += static_cast<char>(0xe0 | (c >> 12));
        text += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        text += static_cast<char>(0x80 | (c & 0x3f));
      } else {
        text += static_cast<char>(0xf0 | (c >> 18));
        text += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
        text += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        text += static_cast<char>(0x80 | (c & 0x3f));
      }
    }

    void popChar(std::string& text) {
      while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) {
        text.pop_back();
      }
      if (!text.empty()) {
        text.pop_back();
      }
    }

    // Edits a one-line field; true when the text changed.
    bool edit(std::string& text, const Key& key) {
      if (key.sym == kBackSpace) {
        const std::size_t before = text.size();
        popChar(text);
        return text.size() != before;
      }
      if (const char32_t c = typed(key); c != 0) {
        append(text, c);
        return true;
      }
      return false;
    }

    std::string count(std::size_t n, const char* what) {
      return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
    }
  } // namespace

  Model::Model(std::filesystem::path dir) : m_dir(std::move(dir)) {}

  std::vector<Effect> Model::start() const { return {effect::List{m_dir}}; }

  const Row* Model::current() const { return m_cursor < m_rows.size() ? &m_rows[m_cursor] : nullptr; }

  void Model::listed(const std::filesystem::path& dir, std::vector<Row> rows) {
    if (dir != m_dir) {
      return;
    }
    const Row* row = current();
    const std::string keep = m_loaded && row != nullptr ? row->name : m_select;
    m_rows = std::move(rows);
    m_loaded = true;
    m_select.clear();
    const auto it = std::ranges::find(m_rows, keep, &Row::name);
    if (it != m_rows.end()) {
      m_cursor = static_cast<std::size_t>(it - m_rows.begin());
    } else {
      m_cursor = m_rows.empty() ? 0 : std::min(m_cursor, m_rows.size() - 1);
    }
    std::erase_if(m_marks,
                  [&](const std::string& name) { return std::ranges::find(m_rows, name, &Row::name) == m_rows.end(); });
  }

  void Model::move(std::ptrdiff_t by) {
    if (m_rows.empty()) {
      return;
    }
    const auto last = static_cast<std::ptrdiff_t>(m_rows.size()) - 1;
    m_cursor = static_cast<std::size_t>(std::clamp(static_cast<std::ptrdiff_t>(m_cursor) + by, std::ptrdiff_t{0}, last));
  }

  std::vector<std::filesystem::path> Model::chosen() const {
    std::vector<std::filesystem::path> out;
    if (!m_marks.empty()) {
      for (const Row& row : m_rows) {
        if (m_marks.contains(row.name)) {
          out.push_back(m_dir / row.name);
        }
      }
    } else if (const Row* row = current()) {
      out.push_back(m_dir / row->name);
    }
    return out;
  }

  std::vector<Effect> Model::go(std::filesystem::path dir, std::string select) {
    std::vector<Effect> out;
    if (!m_filter.empty()) {
      m_filter.clear();
      out.emplace_back(effect::Filter{});
    }
    m_dir = std::move(dir);
    m_rows.clear();
    m_loaded = false;
    m_cursor = 0;
    m_select = std::move(select);
    m_marks.clear();
    m_input = NoInput{};
    out.emplace_back(effect::List{m_dir});
    return out;
  }

  std::vector<Effect> Model::inputKey(const Key& key) {
    if (auto* filter = std::get_if<FilterInput>(&m_input)) {
      if (key.sym == kEscape) {
        m_input = NoInput{};
        m_filter.clear();
        return {effect::Filter{}};
      }
      if (enter(key)) {
        m_input = NoInput{};
        return {};
      }
      if (edit(filter->text, key)) {
        m_filter = filter->text;
        return {effect::Filter{m_filter}};
      }
      return {};
    }
    if (auto* rename = std::get_if<RenameInput>(&m_input)) {
      if (key.sym == kEscape) {
        m_input = NoInput{};
        return {};
      }
      if (enter(key)) {
        RenameInput done = std::move(*rename);
        m_input = NoInput{};
        if (done.text.empty() || done.text == "." || done.text == ".." || done.text.find('/') != std::string::npos) {
          return {effect::Hint{"A name cannot be empty, . or .., or hold a /"}};
        }
        if (done.text == done.original) {
          return {};
        }
        return {effect::Rename{m_dir / done.original, done.text}};
      }
      edit(rename->text, key);
      return {};
    }
    if (auto* trash = std::get_if<ConfirmTrash>(&m_input)) {
      std::vector<std::filesystem::path> paths = std::move(trash->paths);
      m_input = NoInput{};
      if (enter(key) || typed(key) == U'y') {
        m_marks.clear();
        const std::size_t n = paths.size();
        return {effect::Trash{std::move(paths)}, effect::Hint{count(n, "item") + " moved to the Trash"}};
      }
      return {};
    }
    return {};
  }

  std::vector<Effect> Model::key(const Key& key, bool promptIdle) {
    if (!std::holds_alternative<NoInput>(m_input)) {
      return inputKey(key);
    }
    const char32_t c = typed(key);
    if ((enter(key) && (key.mods & kCtrl) != 0) || c == U'q') {
      if (!promptIdle) {
        return {effect::Hint{"A command runs in the shell: it cannot change folder now"}};
      }
      return {effect::Cd{m_dir}, effect::Close{}};
    }
    if (key.sym == kEscape) {
      if (!m_marks.empty()) {
        m_marks.clear();
        return {};
      }
      if (!m_filter.empty()) {
        m_filter.clear();
        return {effect::Filter{}};
      }
      return {effect::Close{}};
    }
    if (key.sym == kDown || c == U'j') {
      move(1);
    } else if (key.sym == kUp || c == U'k') {
      move(-1);
    } else if (key.sym == kPageDown) {
      move(kPage);
    } else if (key.sym == kPageUp) {
      move(-kPage);
    } else if (key.sym == kHome || c == U'g') {
      m_cursor = 0;
    } else if (key.sym == kEnd || c == U'G') {
      m_cursor = m_rows.empty() ? 0 : m_rows.size() - 1;
    } else if (key.sym == kLeft || key.sym == kBackSpace || c == U'h') {
      if (m_dir.has_relative_path()) {
        std::string from = m_dir.filename().string();
        return go(m_dir.parent_path(), std::move(from));
      }
    } else if (key.sym == kRight || enter(key) || c == U'l') {
      if (const Row* row = current()) {
        if (row->directory) {
          return go(m_dir / row->name, {});
        }
        return {effect::Open{m_dir / row->name}};
      }
    } else if (c == U' ') {
      if (const Row* row = current()) {
        if (!m_marks.erase(row->name)) {
          m_marks.insert(row->name);
        }
        move(1);
      }
    } else if (c == U'/') {
      m_input = FilterInput{m_filter};
    } else if (c == U'.') {
      m_hidden = !m_hidden;
      return {effect::ShowHidden{m_hidden}};
    } else if (c == U'o') {
      return {effect::ShowInFiles{m_marks.empty() && current() == nullptr ? std::vector{m_dir} : chosen()}};
    } else if (c == U'i') {
      if (auto paths = chosen(); !paths.empty()) {
        return {effect::Insert{std::move(paths)}, effect::Close{}};
      }
    } else if (c == U'y' || c == U'x') {
      if (auto paths = chosen(); !paths.empty()) {
        const std::size_t n = paths.size();
        m_yank = Yank{std::move(paths), c == U'x'};
        m_marks.clear();
        return {effect::Hint{count(n, "item") + (c == U'x' ? " to move: p puts them here" : " to copy: p puts them here")}};
      }
    } else if (c == U'p') {
      if (!m_yank) {
        return {effect::Hint{"Nothing to put: y copies, x moves"}};
      }
      Yank yank = *m_yank;
      if (yank.cut) {
        m_yank.reset();
        return {effect::Move{std::move(yank.paths), m_dir}};
      }
      return {effect::Copy{std::move(yank.paths), m_dir}};
    } else if (c == U'd') {
      if (auto paths = chosen(); !paths.empty()) {
        m_input = ConfirmTrash{std::move(paths)};
      }
    } else if (c == U'r') {
      if (const Row* row = current()) {
        m_input = RenameInput{row->name, row->name};
      }
    }
    return {};
  }

} // namespace term::browse
