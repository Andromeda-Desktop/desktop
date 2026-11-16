#include "index_db.h"

#include "extract.h"
#include "name_fold.h"

#include <emmintrin.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <format>

namespace andromeda::index {

  namespace {
    constexpr const char* kSchema = R"sql(
      CREATE TABLE IF NOT EXISTS files (
        id INTEGER PRIMARY KEY,
        parent INTEGER NOT NULL,
        name TEXT NOT NULL,
        mtime INTEGER NOT NULL,
        size INTEGER NOT NULL,
        is_dir INTEGER NOT NULL,
        UNIQUE (parent, name)
      );
      DROP TABLE IF EXISTS names;
      -- state: 0 being read, 1 done, 2 unreadable
      CREATE TABLE IF NOT EXISTS extracted (id INTEGER PRIMARY KEY, mtime INTEGER NOT NULL, state INTEGER NOT NULL);
      CREATE VIRTUAL TABLE IF NOT EXISTS content USING fts5(body, content='', contentless_delete=1,
                                                            tokenize='unicode61 remove_diacritics 2', prefix='3');
    )sql";
    // Version 2: content index with word positions (BM25 ranking) and a 3-letter prefix index.
    constexpr int kSchemaVersion = 2;
    constexpr const char* kDropContent = "DROP TABLE IF EXISTS content; DROP TABLE IF EXISTS extracted;";

    std::uint32_t seconds(std::int64_t nanos) {
      return static_cast<std::uint32_t>(std::clamp<std::int64_t>(nanos / 1'000'000'000, 0, UINT32_MAX));
    }

    // An older content index is dropped; the content worker reads the documents again.
    bool migrate(sqlite3* db) {
      sqlite3_stmt* stmt = nullptr;
      sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr);
      const int version = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
      sqlite3_finalize(stmt);
      return version >= kSchemaVersion || sqlite3_exec(db, kDropContent, nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    void extractableFunction(sqlite3_context* context, int /*argc*/, sqlite3_value** argv) {
      const auto* name = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
      sqlite3_result_int(context, name != nullptr && extractable(name) ? 1 : 0);
    }

    // An FTS5 query: every word must appear, the last one as a prefix (it may be half typed).
    std::string contentMatch(std::string_view text) {
      std::vector<std::string> words;
      std::string word;
      for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || static_cast<unsigned char>(c) >= 0x80) {
          word += c;
        } else if (!word.empty()) {
          words.push_back(std::move(word));
          word.clear();
        }
      }
      if (!word.empty()) {
        words.push_back(std::move(word));
      }
      std::string match;
      for (std::size_t i = 0; i < words.size(); ++i) {
        match += (i == 0 ? "\"" : " \"") + words[i] + "\"";
        if (i + 1 == words.size()) {
          match += '*';
        }
      }
      return match;
    }

    std::string trimmed(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
      }
      return std::string(text);
    }

    // First match of a non-empty needle in [at, end). An SSE2 filter on its first and last byte reads about
    // 16 bytes per step; glibc memmem manages about 3 GB/s on short needles.
    const char* findIn(const char* at, const char* end, std::string_view needle) {
      const std::size_t n = needle.size();
      if (n == 1) {
        return static_cast<const char*>(std::memchr(at, needle.front(), static_cast<std::size_t>(end - at)));
      }
      const __m128i first = _mm_set1_epi8(needle.front());
      const __m128i last = _mm_set1_epi8(needle.back());
      for (; end - at >= static_cast<std::ptrdiff_t>(n + 15); at += 16) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(at));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(at + n - 1));
        auto mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(a, first), _mm_cmpeq_epi8(b, last))));
        for (; mask != 0; mask &= mask - 1) {
          const char* candidate = at + std::countr_zero(mask);
          if (std::memcmp(candidate + 1, needle.data() + 1, n - 2) == 0) {
            return candidate;
          }
        }
      }
      return static_cast<const char*>(memmem(at, static_cast<std::size_t>(end - at), needle.data(), n));
    }

    // One bit per a-z and 0-9; every other byte shares one of 28 bits by its value.
    std::uint64_t letterBit(char ch) {
      const auto c = static_cast<unsigned char>(ch);
      return std::uint64_t{1} << (c >= 'a' && c <= 'z' ? c - 'a' : c >= '0' && c <= '9' ? 26 + c - '0' : 36 + c % 28);
    }

    std::uint64_t letterMask(std::string_view text) {
      std::uint64_t mask = 0;
      for (const char ch : text) {
        mask |= letterBit(ch);
      }
      return mask;
    }

    // letterMask of the bytes that start a word (see wordStart).
    std::uint64_t startMask(std::string_view text) {
      std::uint64_t mask = 0;
      for (std::size_t i = 0; i < text.size(); ++i) {
        const auto before = i == 0 ? 0 : static_cast<unsigned char>(text[i - 1]);
        if (before < 0x80 && std::isalnum(before) == 0) {
          mask |= letterBit(text[i]);
        }
      }
      return mask;
    }

    // First p in [at, end) where a 1-2 byte needle starts a word (p is begin or follows a byte wordStart accepts).
    // An SSE2 pass tests 16 positions per step: the needle's bytes and a separator before them.
    const char* findWordStart(const char* begin, const char* at, const char* end, std::string_view needle) {
      const std::size_t n = needle.size();
      const auto isStart = [&](const char* p) {
        if (p[0] != needle[0] || (n == 2 && (p + 1 >= end || p[1] != needle[1]))) {
          return false;
        }
        const auto c = p == begin ? 0 : static_cast<unsigned char>(p[-1]);
        return c < 0x80 && std::isalnum(c) == 0;
      };
      if (at == begin && at < end && isStart(at)) {
        return at;
      }
      at += at == begin ? 1 : 0;
      const __m128i first = _mm_set1_epi8(needle[0]);
      const __m128i second = _mm_set1_epi8(n == 2 ? needle[1] : 0);
      for (; end - at >= 17; at += 16) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(at));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(at - 1));
        __m128i hit = _mm_cmpeq_epi8(a, first);
        if (n == 2) {
          hit = _mm_and_si128(hit, _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(at + 1)), second));
        }
        const __m128i ascii = _mm_cmpgt_epi8(b, _mm_set1_epi8(-1));
        const __m128i digit = _mm_and_si128(_mm_cmpgt_epi8(b, _mm_set1_epi8('0' - 1)), _mm_cmplt_epi8(b, _mm_set1_epi8('9' + 1)));
        const __m128i lower = _mm_and_si128(_mm_cmpgt_epi8(b, _mm_set1_epi8('a' - 1)), _mm_cmplt_epi8(b, _mm_set1_epi8('z' + 1)));
        const __m128i upper = _mm_and_si128(_mm_cmpgt_epi8(b, _mm_set1_epi8('A' - 1)), _mm_cmplt_epi8(b, _mm_set1_epi8('Z' + 1)));
        const __m128i sep = _mm_andnot_si128(_mm_or_si128(_mm_or_si128(digit, lower), upper), ascii);
        if (const auto mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(hit, sep))); mask != 0) {
          return at + std::countr_zero(mask);
        }
      }
      for (; at < end; ++at) {
        if (isStart(at)) {
          return at;
        }
      }
      return nullptr;
    }

    // Typo hits rank after every letters-in-order hit (whose rank grows with its span).
    constexpr std::size_t kTypoRank = 1'000'000;

    // Fewest edits (insert, delete, replace, swap of neighbours) that turn a prefix of [at, end) into needle (1-64
    // bytes): Myers' bit-parallel edit distance with Hyyro's swap step, one step per byte, anchored at `at`.
    using ByteBits = std::array<std::uint64_t, 256>; // bit i of [c]: needle[i] == c
    ByteBits byteBits(std::string_view needle) {
      ByteBits eq{};
      for (std::size_t i = 0; i < needle.size(); ++i) {
        eq[static_cast<unsigned char>(needle[i])] |= std::uint64_t{1} << i;
      }
      return eq;
    }

    int editsFrom(const char* at, const char* end, std::string_view needle, const ByteBits& eq) {
      const std::uint64_t high = std::uint64_t{1} << (needle.size() - 1);
      std::uint64_t vp = ~std::uint64_t{0};
      std::uint64_t vn = 0;
      std::uint64_t d0 = 0;
      std::uint64_t lastEq = 0;
      int score = static_cast<int>(needle.size());
      int best = score;
      for (const char* p = at; p < end && *p != '\0'; ++p) {
        const std::uint64_t e = eq[static_cast<unsigned char>(*p)];
        const std::uint64_t swapped = ((~d0 & e) << 1) & lastEq; // Hyyro: a swapped pair costs one edit
        d0 = (((e & vp) + vp) ^ vp) | e | vn | swapped;
        std::uint64_t hp = vn | ~(d0 | vp);
        std::uint64_t hn = vp & d0;
        score += (hp & high) != 0 ? 1 : (hn & high) != 0 ? -1 : 0;
        hp = (hp << 1) | 1; // anchored: skipping the first k bytes costs k edits
        hn <<= 1;
        vp = hn | ~(d0 | hp);
        vn = hp & d0;
        lastEq = e;
        best = std::min(best, score);
      }
      return best;
    }

    // Names are folded, so a letter or digit byte (or any UTF-8 byte) continues a word; anything else ends one.
    bool wordStart(const char* begin, const char* p) {
      const auto c = p == begin ? 0 : static_cast<unsigned char>(p[-1]);
      return c < 0x80 && std::isalnum(c) == 0;
    }

    // Span of the greedy match of needle as a subsequence of name from its first word start that holds needle's first
    // byte; 0 if none. A later start cannot match when the first one fails.
    std::size_t subsequenceSpan(const char* begin, const char* name, std::string_view needle) {
      for (const char* start = name; *start != '\0'; ++start) {
        if (*start != needle.front() || !wordStart(begin, start)) {
          continue;
        }
        std::size_t matched = 1;
        const char* p = start + 1;
        for (; *p != '\0' && matched < needle.size(); ++p) {
          matched += *p == needle[matched] ? 1 : 0;
        }
        return matched == needle.size() ? static_cast<std::size_t>(p - start) : 0;
      }
      return 0;
    }

  } // namespace

  IndexDb::IndexDb(const std::filesystem::path& file, bool bulk) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const char* pragmas = bulk ? "PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF;"
                               : "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;";
    // The timeout comes first: the schema writes below race the other connections (content worker lanes).
    const bool opened = sqlite3_open(file.c_str(), &m_db) == SQLITE_OK && sqlite3_busy_timeout(m_db, 5000) == SQLITE_OK;
    if (!opened || sqlite3_exec(m_db, pragmas, nullptr, nullptr, nullptr) != SQLITE_OK
        || !migrate(m_db) || sqlite3_exec(m_db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK
        || sqlite3_exec(m_db, std::format("PRAGMA user_version = {}", kSchemaVersion).c_str(), nullptr, nullptr, nullptr)
            != SQLITE_OK) {
      std::fprintf(stderr, "andromeda-index: %s: %s\n", file.c_str(), m_db != nullptr ? sqlite3_errmsg(m_db) : "open failed");
      sqlite3_close(m_db);
      m_db = nullptr;
      return;
    }
    sqlite3_prepare_v3(
        m_db, "INSERT INTO files(id, parent, name, mtime, size, is_dir) VALUES (?1, ?2, ?3, ?4, ?5, ?6)", -1,
        SQLITE_PREPARE_PERSISTENT, &m_insert, nullptr
    );
    sqlite3_prepare_v3(m_db, "SELECT name, mtime, size, is_dir FROM files WHERE id = ?1", -1, SQLITE_PREPARE_PERSISTENT, &m_row, nullptr);
    sqlite3_prepare_v3(m_db, "SELECT parent, name FROM files WHERE id = ?1", -1, SQLITE_PREPARE_PERSISTENT, &m_parentOf, nullptr);
    sqlite3_prepare_v3(m_db, "SELECT id FROM files WHERE parent = ?1 AND name = ?2", -1, SQLITE_PREPARE_PERSISTENT, &m_child, nullptr);
    sqlite3_create_function(m_db, "extractable", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, extractableFunction,
                            nullptr, nullptr);
  }

  IndexDb::~IndexDb() {
    sqlite3_finalize(m_insert);
    sqlite3_finalize(m_row);
    sqlite3_finalize(m_parentOf);
    sqlite3_finalize(m_child);
    sqlite3_close(m_db);
  }

  void IndexDb::begin() { sqlite3_exec(m_db, "BEGIN", nullptr, nullptr, nullptr); }
  void IndexDb::commit() { sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr); }

  void IndexDb::insert(std::int64_t id, std::int64_t parent, std::string_view name, std::int64_t mtime, std::int64_t size, bool isDir) {
    sqlite3_bind_int64(m_insert, 1, id);
    sqlite3_bind_int64(m_insert, 2, parent);
    sqlite3_bind_text(m_insert, 3, name.data(), static_cast<int>(name.size()), SQLITE_STATIC);
    sqlite3_bind_int64(m_insert, 4, mtime);
    sqlite3_bind_int64(m_insert, 5, size);
    sqlite3_bind_int(m_insert, 6, isDir ? 1 : 0);
    sqlite3_step(m_insert);
    sqlite3_reset(m_insert);
  }

  void IndexDb::finishBulk() { sqlite3_exec(m_db, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr); }

  std::string IndexDb::pathOf(std::int64_t id) const {
    std::vector<std::string> parts;
    while (id != 0) {
      sqlite3_bind_int64(m_parentOf, 1, id);
      if (sqlite3_step(m_parentOf) != SQLITE_ROW) {
        sqlite3_reset(m_parentOf);
        return {};
      }
      id = sqlite3_column_int64(m_parentOf, 0);
      parts.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(m_parentOf, 1)));
      sqlite3_reset(m_parentOf);
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
      path += path.empty() || path.ends_with('/') ? *it : "/" + *it;
    }
    return path;
  }

  std::optional<std::int64_t> IndexDb::idOf(std::string_view path) const {
    // Roots carry their absolute path as name; the longest root that prefixes path wins.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT id, name FROM files WHERE parent = 0", -1, &stmt, nullptr);
    std::optional<std::int64_t> id;
    std::size_t rootLength = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const std::string_view name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      const bool prefix = path == name || (path.starts_with(name) && (name.ends_with('/') || path[name.size()] == '/'));
      if (prefix && name.size() >= rootLength) {
        id = sqlite3_column_int64(stmt, 0);
        rootLength = name.size();
      }
    }
    sqlite3_finalize(stmt);
    if (!id) {
      return std::nullopt;
    }
    std::string_view rest = path.substr(rootLength);
    while (!rest.empty()) {
      rest.remove_prefix(rest.front() == '/' ? 1 : 0);
      const std::string_view part = rest.substr(0, rest.find('/'));
      rest.remove_prefix(part.size());
      if (part.empty()) {
        continue;
      }
      sqlite3_bind_int64(m_child, 1, *id);
      sqlite3_bind_text(m_child, 2, part.data(), static_cast<int>(part.size()), SQLITE_STATIC);
      const bool found = sqlite3_step(m_child) == SQLITE_ROW;
      id = found ? std::optional(sqlite3_column_int64(m_child, 0)) : std::nullopt;
      sqlite3_reset(m_child);
      if (!found) {
        return std::nullopt;
      }
    }
    return id;
  }

  std::int64_t IndexDb::add(std::int64_t parent, std::string_view name, std::int64_t mtime, std::int64_t size, bool isDir) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT coalesce(max(id), 0) + 1 FROM files", -1, &stmt, nullptr);
    std::int64_t id = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 1;
    sqlite3_finalize(stmt);
    // A removed newest entry still sits in memory; its id must not be given out again.
    if (m_loaded && !m_entries.empty()) {
      id = std::max<std::int64_t>(id, m_entries.back().id + 1);
    }
    insert(id, parent, name, mtime, size, isDir);
    if (m_loaded) {
      appendName(id, name, mtime, isDir);
    }
    return id;
  }

  void IndexDb::update(std::int64_t id, std::int64_t mtime, std::int64_t size) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "UPDATE files SET mtime = ?2, size = ?3 WHERE id = ?1", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, mtime);
    sqlite3_bind_int64(stmt, 3, size);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (NameEntry* entry = entryOf(id)) {
      entry->mtime = seconds(mtime);
    }
  }

  std::vector<Child> IndexDb::children(std::int64_t parent) const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT id, name, mtime, size, is_dir FROM files WHERE parent = ?1", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, parent);
    std::vector<Child> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      out.push_back({
          .id = sqlite3_column_int64(stmt, 0),
          .name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
          .mtime = sqlite3_column_int64(stmt, 2),
          .size = sqlite3_column_int64(stmt, 3),
          .isDir = sqlite3_column_int(stmt, 4) != 0,
      });
    }
    sqlite3_finalize(stmt);
    return out;
  }

  std::vector<Folder> IndexDb::folders() const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(
        m_db,
        "WITH RECURSIVE t(id, path, mtime) AS (SELECT id, name, mtime FROM files WHERE parent = 0 "
        "UNION ALL SELECT f.id, t.path || '/' || f.name, f.mtime FROM files f JOIN t ON f.parent = t.id WHERE f.is_dir) "
        "SELECT id, path, mtime FROM t",
        -1, &stmt, nullptr
    );
    std::vector<Folder> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      out.push_back({
          .id = sqlite3_column_int64(stmt, 0),
          .path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
          .mtime = sqlite3_column_int64(stmt, 2),
      });
    }
    sqlite3_finalize(stmt);
    return out;
  }

  void IndexDb::removeTree(std::string_view path) {
    if (const auto id = idOf(path)) {
      removeTree(*id);
    }
  }

  void IndexDb::removeTree(std::int64_t id) {
    // External-content FTS5 needs the old value to delete a row from the index.
    sqlite3_exec(m_db, "CREATE TEMP TABLE IF NOT EXISTS doomed(id INTEGER PRIMARY KEY); DELETE FROM doomed;", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(
        m_db,
        "INSERT INTO doomed WITH RECURSIVE sub(id) AS (SELECT ?1 UNION ALL SELECT f.id FROM files f JOIN sub ON f.parent = sub.id) "
        "SELECT id FROM sub",
        -1, &stmt, nullptr
    );
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (m_loaded) {
      sqlite3_prepare_v2(m_db, "SELECT id FROM doomed", -1, &stmt, nullptr);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (NameEntry* entry = entryOf(sqlite3_column_int64(stmt, 0)); entry != nullptr && !entry->removed) {
          entry->removed = true;
          ++m_removed;
        }
      }
      sqlite3_finalize(stmt);
    }
    sqlite3_exec(m_db,
                 "DELETE FROM content WHERE rowid IN (SELECT id FROM doomed);"
                 "DELETE FROM extracted WHERE id IN (SELECT id FROM doomed);"
                 "DELETE FROM files WHERE id IN (SELECT id FROM doomed);",
                 nullptr, nullptr, nullptr);
    // Removed names stay in m_folded as dead space until it is half dead.
    if (m_removed * 2 > m_entries.size()) {
      m_loaded = false;
      loadNames();
    }
  }

  void IndexDb::loadNames() const {
    if (m_loaded || m_db == nullptr) {
      return;
    }
    m_folded.clear();
    m_entries.clear();
    m_removed = 0;
    sqlite3_stmt* stmt = nullptr;
    // Exact sizes up front: growth by doubling would leave up to twice the memory in the heap.
    sqlite3_prepare_v2(m_db, "SELECT count(*), coalesce(sum(length(CAST(name AS BLOB))), 0) FROM files", -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      m_entries.reserve(static_cast<std::size_t>(sqlite3_column_int64(stmt, 0)) + 64);
      m_folded.reserve(static_cast<std::size_t>(sqlite3_column_int64(stmt, 0) + sqlite3_column_int64(stmt, 1)) + 4096);
    }
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(m_db, "SELECT id, name, mtime, is_dir FROM files ORDER BY id", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      appendName(sqlite3_column_int64(stmt, 0), {name, static_cast<std::size_t>(sqlite3_column_bytes(stmt, 1))},
                 sqlite3_column_int64(stmt, 2), sqlite3_column_int(stmt, 3) != 0);
    }
    sqlite3_finalize(stmt);
    m_loaded = true;
  }

  void IndexDb::appendName(std::int64_t id, std::string_view name, std::int64_t mtime, bool isDir) const {
    const std::string folded = foldName(name);
    m_entries.push_back({.id = static_cast<std::uint32_t>(id), .offset = static_cast<std::uint32_t>(m_folded.size()),
                         .mtime = seconds(mtime), .kind = fileKind(name, isDir), .removed = false,
                         .letters = letterMask(folded), .starts = startMask(folded)});
    m_folded += folded;
    m_folded += '\0';
  }

  IndexDb::NameEntry* IndexDb::entryOf(std::int64_t id) const {
    if (!m_loaded) {
      return nullptr;
    }
    const auto it = std::ranges::lower_bound(m_entries, static_cast<std::uint32_t>(id), {}, &NameEntry::id);
    return it != m_entries.end() && it->id == id ? &*it : nullptr;
  }

  std::vector<FileRow> IndexDb::queryNames(std::string_view text, int limit, FileKind kind) const {
    const std::string needle = foldName(trimmed(text));
    const bool anyKind = kind == FileKind::Any;
    if ((anyKind && needle.empty()) || m_db == nullptr) {
      return {};
    }
    loadNames();
    // A 1-2 letter needle matches only at word starts: inside words it would match nearly every name.
    const bool shortNeedle = needle.size() < 3;
    struct Hit {
      std::size_t rank; // 0 exact name, 1 name prefix, 2 substring, 3 + extra span for letters in order with gaps
      std::uint32_t mtime;
      std::int64_t id;
    };
    // Only the best `limit` hits are kept, in a heap whose top is the worst kept: a 1-letter query at 1M names has
    // ~500k hits.
    const auto keep = static_cast<std::size_t>(std::max(limit, 0));
    const auto better = [](const Hit& a, const Hit& b) { return a.rank != b.rank ? a.rank < b.rank : a.mtime > b.mtime; };
    std::vector<Hit> hits;
    hits.reserve(keep);
    const auto offer = [&](const Hit& hit) {
      if (hits.size() < keep) {
        hits.push_back(hit);
        std::ranges::push_heap(hits, better);
      } else if (keep != 0 && better(hit, hits.front())) {
        std::ranges::pop_heap(hits, better);
        hits.back() = hit;
        std::ranges::push_heap(hits, better);
      }
    };
    if (needle.empty()) {
      for (const NameEntry& entry : m_entries) {
        if (!entry.removed && entry.kind == kind) {
          offer({.rank = 0, .mtime = entry.mtime, .id = entry.id});
        }
      }
    }
    const char* const begin = m_folded.data();
    const char* const end = begin + m_folded.size();
    auto entry = m_entries.begin();
    for (const char* at = needle.empty() ? end : begin; at < end;) {
      const char* found = shortNeedle ? findWordStart(begin, at, end, needle) : findIn(at, end, needle);
      if (found == nullptr) {
        break;
      }
      const auto offset = static_cast<std::uint32_t>(found - begin);
      // Hits come in offset order and `entry` only moves forward: a near hit is a few steps away, a far one is
      // found by binary search (a walk over sparse hits would read every entry).
      for (int step = 0; step < 8 && entry + 1 != m_entries.end() && (entry + 1)->offset <= offset; ++step) {
        ++entry;
      }
      if (entry + 1 != m_entries.end() && (entry + 1)->offset <= offset) {
        entry = std::ranges::upper_bound(entry, m_entries.end(), offset, {}, &NameEntry::offset) - 1;
      }
      const auto next = entry + 1;
      at = next != m_entries.end() ? begin + next->offset : end;
      if (entry->removed || (!anyKind && entry->kind != kind)) {
        continue;
      }
      const std::size_t rank = offset != entry->offset ? 2 : (found[needle.size()] == '\0' ? 0 : 1);
      offer({.rank = rank, .mtime = entry->mtime, .id = entry->id});
    }
    const auto seenIds = [&] {
      std::vector<std::int64_t> seen;
      for (const Hit& hit : hits) {
        seen.push_back(hit.id);
      }
      std::ranges::sort(seen);
      return seen;
    };
    const std::uint64_t letters = letterMask(needle);
    // Few substring hits: names with a word that starts with the needle's first byte and from there holds the
    // needle's letters in order ("dwnhelp" -> "download-helper"), or begins within 1 edit of it (2 from 9 bytes; a
    // swapped pair is one edit). Names with a substring hit are skipped; the masks skip a name with no such word start,
    // or missing more needle letters than the edits allow, without reading it.
    if (!shortNeedle && hits.size() < keep) {
      const std::vector<std::int64_t> seen = seenIds();
      const bool typos = needle.size() >= 4 && needle.size() <= 64;
      const int most = !typos ? 0 : needle.size() >= 9 ? 2 : 1;
      const std::size_t window = needle.size() + static_cast<std::size_t>(most);
      const ByteBits eq = byteBits(typos ? needle : std::string_view());
      const std::uint64_t first = letterBit(needle.front());
      for (std::size_t i = 0; i < m_entries.size(); ++i) {
        const NameEntry& e = m_entries[i];
        const int missing = std::popcount(letters & ~e.letters);
        if ((e.starts & first) == 0 || missing > most || e.removed || (!anyKind && e.kind != kind)) {
          continue;
        }
        const char* name = begin + e.offset;
        const char* nameEnd = i + 1 < m_entries.size() ? begin + m_entries[i + 1].offset - 1 : end - 1;
        std::size_t rank = SIZE_MAX;
        if (missing == 0) {
          if (const std::size_t span = subsequenceSpan(begin, name, needle); span != 0) {
            rank = 3 + span - needle.size();
          }
        }
        for (const char* p = name; typos && rank >= kTypoRank && p < nameEnd; ++p) {
          if (*p == needle.front() && wordStart(begin, p)) {
            if (const int edits = editsFrom(p, std::min(nameEnd, p + window), needle, eq); edits <= most) {
              rank = std::min(rank, kTypoRank + static_cast<std::size_t>(edits));
            }
          }
        }
        if (rank != SIZE_MAX && !std::ranges::binary_search(seen, static_cast<std::int64_t>(e.id))) {
          offer({.rank = rank, .mtime = e.mtime, .id = e.id});
        }
      }
    }
    std::ranges::sort_heap(hits, better);
    std::vector<FileRow> rows;
    for (std::size_t i = 0; i < hits.size(); ++i) {
      sqlite3_bind_int64(m_row, 1, hits[i].id);
      if (sqlite3_step(m_row) == SQLITE_ROW) {
        rows.push_back({
            .path = pathOf(hits[i].id),
            .name = reinterpret_cast<const char*>(sqlite3_column_text(m_row, 0)),
            .mtime = sqlite3_column_int64(m_row, 1),
            .size = sqlite3_column_int64(m_row, 2),
            .isDir = sqlite3_column_int(m_row, 3) != 0,
        });
      }
      sqlite3_reset(m_row);
    }
    return rows;
  }

  std::vector<ContentTask> IndexDb::contentTasks(std::int64_t minMtime, std::int64_t limit) const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db,
                       "SELECT f.id, f.mtime FROM files f LEFT JOIN extracted e ON e.id = f.id "
                       "WHERE f.is_dir = 0 AND f.size <= ?1 AND (e.id IS NULL OR e.mtime != f.mtime) AND extractable(f.name) "
                       "AND f.mtime >= ?2 ORDER BY f.mtime DESC LIMIT ?3",
                       -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, kMaxContentFile);
    sqlite3_bind_int64(stmt, 2, minMtime);
    sqlite3_bind_int64(stmt, 3, limit);
    std::vector<ContentTask> tasks;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      tasks.push_back({.id = sqlite3_column_int64(stmt, 0), .mtime = sqlite3_column_int64(stmt, 1), .path = {}});
    }
    sqlite3_finalize(stmt);
    for (ContentTask& task : tasks) {
      task.path = pathOf(task.id);
    }
    return tasks;
  }

  bool IndexDb::needsContent(std::int64_t id, std::int64_t mtime) const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db,
                       "SELECT 1 FROM files f WHERE f.id = ?1 AND f.mtime = ?2 "
                       "AND NOT EXISTS (SELECT 1 FROM extracted e WHERE e.id = ?1 AND e.mtime = ?2)",
                       -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, mtime);
    const bool needed = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return needed;
  }

  void IndexDb::markExtracting(std::int64_t id, std::int64_t mtime) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "INSERT OR REPLACE INTO extracted(id, mtime, state) VALUES (?1, ?2, 0)", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, mtime);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void IndexDb::storeContent(std::int64_t id, std::int64_t mtime, const std::optional<std::string>& text) {
    // IMMEDIATE: the write lock from the start, so the live index cannot remove the file between check and write.
    sqlite3_exec(m_db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT 1 FROM files WHERE id = ?1", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (!exists) {
      commit();
      return;
    }
    sqlite3_prepare_v2(m_db, "DELETE FROM content WHERE rowid = ?1", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (text && !text->empty()) {
      sqlite3_prepare_v2(m_db, "INSERT INTO content(rowid, body) VALUES (?1, ?2)", -1, &stmt, nullptr);
      sqlite3_bind_int64(stmt, 1, id);
      sqlite3_bind_text(stmt, 2, text->data(), static_cast<int>(text->size()), SQLITE_STATIC);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    sqlite3_prepare_v2(m_db, "INSERT OR REPLACE INTO extracted(id, mtime, state) VALUES (?1, ?2, ?3)", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, mtime);
    sqlite3_bind_int(stmt, 3, text ? 1 : 2);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    commit();
  }

  void IndexDb::releaseMemory() { sqlite3_db_release_memory(m_db); }

  void IndexDb::failInterrupted() {
    sqlite3_exec(m_db, "UPDATE extracted SET state = 2 WHERE state = 0", nullptr, nullptr, nullptr);
  }

  std::vector<FileRow> IndexDb::queryContent(std::string_view text, int limit,
                                             const std::vector<std::string>& excludePaths) const {
    const std::string match = contentMatch(text);
    if (match.empty() || limit <= 0) {
      return {};
    }
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db,
                       "SELECT f.id, f.name, f.mtime, f.size FROM content c JOIN files f ON f.id = c.rowid "
                       "WHERE content MATCH ?1 ORDER BY rank LIMIT ?2",
                       -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit + static_cast<int>(excludePaths.size()));
    std::vector<FileRow> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW && static_cast<int>(rows.size()) < limit) {
      FileRow row{
          .path = pathOf(sqlite3_column_int64(stmt, 0)),
          .name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
          .mtime = sqlite3_column_int64(stmt, 2),
          .size = sqlite3_column_int64(stmt, 3),
          .isDir = false,
      };
      if (std::ranges::find(excludePaths, row.path) == excludePaths.end()) {
        rows.push_back(std::move(row));
      }
    }
    sqlite3_finalize(stmt);
    return rows;
  }

  std::int64_t IndexDb::count() const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT count(*) FROM files", -1, &stmt, nullptr);
    const std::int64_t n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return n;
  }

} // namespace andromeda::index
