#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace term::browse {

  // Browse mode (Ctrl+Shift+O): keys over a folder listing, with no IO. The owner lists folders, runs the effects and
  // feeds the rows back through listed().
  struct Row {
    std::string name;
    bool directory = false;
  };

  struct Key {
    std::uint32_t sym = 0;   // XKB keysym
    std::uint32_t utf32 = 0; // the typed character; 0 for other keys
    std::uint32_t mods = 0;  // KeyMod bits: shift 1, ctrl 2, alt 4, super 8
  };

  namespace effect {
    struct List { // show this folder; listed() brings its rows
      std::filesystem::path dir;
    };
    struct Filter {
      std::string text;
    };
    struct ShowHidden {
      bool show = false;
    };
    struct Cd { // the shell changes to dir; the mode closes
      std::filesystem::path dir;
    };
    struct Insert { // the paths go to the prompt, quoted; the mode closes
      std::vector<std::filesystem::path> paths;
    };
    struct Open { // the default app opens the file
      std::filesystem::path path;
    };
    struct ShowInFiles {
      std::vector<std::filesystem::path> paths;
    };
    struct Copy {
      std::vector<std::filesystem::path> paths;
      std::filesystem::path dest;
    };
    struct Move {
      std::vector<std::filesystem::path> paths;
      std::filesystem::path dest;
    };
    struct Trash {
      std::vector<std::filesystem::path> paths;
    };
    struct Rename {
      std::filesystem::path path;
      std::string name;
    };
    struct RenameMany {
      std::vector<std::pair<std::filesystem::path, std::string>> items;
    };
    struct Close {};
    struct Hint {
      std::string text;
    };
  } // namespace effect
  using Effect = std::variant<effect::List, effect::Filter, effect::ShowHidden, effect::Cd, effect::Insert, effect::Open,
                              effect::ShowInFiles, effect::Copy, effect::Move, effect::Trash, effect::Rename, effect::RenameMany,
                              effect::Close, effect::Hint>;

  struct NoInput {};
  struct FilterInput {
    std::string text;
  };
  struct RenameInput {
    std::string original;
    std::string text;
  };
  struct ConfirmTrash {
    std::vector<std::filesystem::path> paths;
  };
  struct JumpInput { // z: a fuzzy query over the folders the terminal visited
    std::string text;
    std::size_t pick = 0; // index into jumpMatches()
  };
  struct BulkRename { // R: the owner edits these names one per line, then calls bulkRenamed() or cancelBulk()
    std::vector<std::string> names;
  };
  using Input = std::variant<NoInput, FilterInput, RenameInput, ConfirmTrash, JumpInput, BulkRename>;

  struct Yank {
    std::vector<std::filesystem::path> paths;
    bool cut = false;
  };

  class Model {
  public:
    // dir must be absolute. The first effect the owner runs is List{dir} (start()).
    explicit Model(std::filesystem::path dir);

    [[nodiscard]] std::vector<Effect> start() const;
    // promptIdle: the shell waits at its prompt, so a typed cd is safe.
    [[nodiscard]] std::vector<Effect> key(const Key& key, bool promptIdle);
    // The shown rows of dir, sorted and filtered. Rows for another folder (a late listing) are dropped.
    void listed(const std::filesystem::path& dir, std::vector<Row> rows);
    // Jump targets, most recent first.
    void setFolders(std::vector<std::filesystem::path> folders) { m_folders = std::move(folders); }
    // The jump list for the current query, best first (at most 50); empty outside JumpInput.
    [[nodiscard]] std::vector<std::filesystem::path> jumpMatches() const;
    // The edited names of BulkRename, one per line, in the same order.
    [[nodiscard]] std::vector<Effect> bulkRenamed(std::string_view text);
    void cancelBulk();

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return m_dir; }
    [[nodiscard]] const std::vector<Row>& rows() const noexcept { return m_rows; }
    [[nodiscard]] bool loaded() const noexcept { return m_loaded; }
    [[nodiscard]] std::size_t cursor() const noexcept { return m_cursor; }
    [[nodiscard]] const Row* current() const;
    [[nodiscard]] const std::set<std::string>& marks() const noexcept { return m_marks; }
    [[nodiscard]] const Input& input() const noexcept { return m_input; }
    [[nodiscard]] const std::optional<Yank>& yank() const noexcept { return m_yank; }
    [[nodiscard]] bool showHidden() const noexcept { return m_hidden; }
    [[nodiscard]] const std::string& filter() const noexcept { return m_filter; }

  private:
    std::vector<Effect> go(std::filesystem::path dir, std::string select);
    std::vector<Effect> inputKey(const Key& key);
    void move(std::ptrdiff_t by);
    [[nodiscard]] std::vector<std::filesystem::path> chosen() const;

    std::filesystem::path m_dir;
    std::vector<Row> m_rows;
    bool m_loaded = false;
    std::size_t m_cursor = 0;
    std::string m_select; // the row the cursor goes to when the listing comes (the folder just left)
    std::set<std::string> m_marks;
    Input m_input = NoInput{};
    std::string m_filter;
    bool m_hidden = false;
    std::optional<Yank> m_yank;
    std::vector<std::filesystem::path> m_folders;
  };

} // namespace term::browse
