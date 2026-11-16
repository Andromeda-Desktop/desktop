#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

enum class FileKind : std::uint8_t { File, Directory, Symlink, Other };

// A string kept once per process: rows of the same type share it. Interned strings live until exit.
class ContentType {
public:
  ContentType() = default;
  ContentType(std::string_view type);
  ContentType(const char* type) : ContentType(std::string_view(type)) {}
  ContentType(const std::string& type) : ContentType(std::string_view(type)) {}

  operator const std::string&() const { return *m_type; }
  operator std::string_view() const { return *m_type; }
  [[nodiscard]] const std::string& str() const { return *m_type; }
  [[nodiscard]] const char* c_str() const { return m_type->c_str(); }
  [[nodiscard]] bool empty() const { return m_type->empty(); }
  [[nodiscard]] bool starts_with(std::string_view prefix) const { return m_type->starts_with(prefix); }

  friend bool operator==(const ContentType& a, const ContentType& b) { return a.m_type == b.m_type; }
  friend bool operator==(const ContentType& a, std::string_view b) { return *a.m_type == b; }
  friend bool operator==(const ContentType& a, const std::string& b) { return *a.m_type == b; }
  friend bool operator==(const ContentType& a, const char* b) { return *a.m_type == b; }

private:
  static const std::string kEmpty;
  const std::string* m_type = &kEmpty;
};

struct FileInfo {
  std::string name;              // file name bytes as on disk (not always UTF-8)
  std::string ownDisplayName;    // empty when displayName() is name: most rows, which saves a heap string per row
  std::string collateKey;        // natural, locale-aware order: "file2" before "file10"
  std::string ownFoldedName;     // empty when foldedName() is displayName()
  ContentType contentType;       // from the name only, e.g. "image/png", "inode/directory"
  FileKind kind = FileKind::File;
  bool isDir = false;            // a directory, or a symlink to one
  bool hidden = false;           // a dot file
  std::uint64_t size = 0;        // bytes; 0 for directories
  std::int64_t mtimeNs = 0;      // CLOCK_REALTIME ns
  std::int64_t addedNs = 0;      // when the entry landed here: birth time, or the change time without one
  std::uint32_t mode = 0;
  std::uint64_t inode = 0;
  bool pendingStat = false;      // listed by name only: size, mtimeNs, mode and inode are not read yet

  // Valid UTF-8 for labels.
  [[nodiscard]] const std::string& displayName() const { return ownDisplayName.empty() ? name : ownDisplayName; }
  // displayName() case-folded: the filter matches against it.
  [[nodiscard]] const std::string& foldedName() const {
    return ownFoldedName.empty() ? displayName() : ownFoldedName;
  }
  void setDisplayName(std::string shown) { ownDisplayName = shown == name ? std::string() : std::move(shown); }
  // name becomes a path or another key; displayName() keeps the label.
  void setNameKeepLabel(std::string key) {
    if (ownDisplayName.empty()) {
      ownDisplayName = std::move(name);
    }
    name = std::move(key);
  }
};

// lstat + stat of one entry. Empty when it no longer exists.
[[nodiscard]] std::optional<FileInfo> statFile(int dirFd, std::string_view name);
[[nodiscard]] std::optional<FileInfo> statFile(const std::filesystem::path& path);
// An entry from its directory record: no stat when dType names a file or directory (pendingStat), else statFile.
// With describe false, such an entry leaves its name fields to describeText and describeType.
[[nodiscard]] std::optional<FileInfo> fileFromDirent(int dirFd, std::string_view name, unsigned char dType,
                                                     bool describe = true);
// The fields that come from the name and kind alone, in two parts that touch different fields, so two threads may fill
// one entry: hidden, displayName, collateKey and foldedName; and contentType (GLib guesses types under one global lock).
void describeText(FileInfo& info);
void describeType(FileInfo& info);
// Reads the stat fields of info.name and clears pendingStat. False when it no longer exists.
bool fillStat(int dirFd, FileInfo& info);
