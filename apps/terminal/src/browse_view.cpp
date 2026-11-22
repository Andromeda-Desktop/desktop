#include "browse_view.h"

#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "core/files/file_model.h"
#include "platform/input_events.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/files/file_format.h"
#include "ui/files/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace term {

  namespace {
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    DirectoryWatcher& watcher() {
      static DirectoryWatcher watcher;
      return watcher;
    }

    void clear(Node& node) {
      while (!node.children().empty()) {
        node.removeChild(node.children().back().get());
      }
    }

    std::string statusText(FileModelView& view) {
      switch (view.model().status()) {
      case FileModel::Status::Loading:
        return "Loading…";
      case FileModel::Status::Missing:
        return "This folder no longer exists";
      case FileModel::Status::Denied:
        return "No access to this folder";
      case FileModel::Status::Error:
        return view.model().error();
      case FileModel::Status::Ready:
        break;
      }
      return {};
    }

    // The folder for the header: the home folder shows as ~.
    std::string shown(const std::filesystem::path& dir) {
      const char* home = std::getenv("HOME");
      const std::string text = dir.string();
      if (home == nullptr || *home == '\0') {
        return text;
      }
      const std::string_view h(home);
      if (text == h) {
        return "~";
      }
      if (text.starts_with(h) && text.size() > h.size() && text[h.size()] == '/') {
        return "~" + text.substr(h.size());
      }
      return text;
    }

    // Text a preview may show: text/* and the common text formats named by other types.
    bool isText(const FileInfo& info) {
      static constexpr std::string_view kTypes[] = {"application/json", "application/xml", "application/toml",
                                                    "application/x-shellscript", "application/javascript",
                                                    "application/x-yaml", "application/sql"};
      return info.contentType.starts_with("text/") ||
             std::ranges::contains(kTypes, std::string_view(info.contentType.str()));
    }

    // The first lines of a file as valid UTF-8; none when it holds a NUL (binary) or cannot be read.
    std::optional<std::string> readHead(const std::filesystem::path& path) {
      constexpr std::size_t kBytes = 4096;
      constexpr int kLines = 60;
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return std::nullopt;
      }
      std::string raw(kBytes, '\0');
      in.read(raw.data(), static_cast<std::streamsize>(kBytes));
      raw.resize(static_cast<std::size_t>(in.gcount()));
      if (raw.find('\0') != std::string::npos) {
        return std::nullopt;
      }
      std::string out;
      int lines = 0;
      for (std::size_t i = 0; i < raw.size() && lines < kLines;) {
        const auto b = static_cast<unsigned char>(raw[i]);
        const std::size_t len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xe ? 3 : (b >> 3) == 0x1e ? 4 : 0;
        bool valid = len != 0 && i + len <= raw.size();
        for (std::size_t k = 1; valid && k < len; ++k) {
          valid = (static_cast<unsigned char>(raw[i + k]) & 0xc0) == 0x80;
        }
        if (!valid) {
          // A cut sequence at the end of the read is not an error.
          if (len != 0 && i + len > raw.size()) {
            break;
          }
          out += "\xEF\xBF\xBD";
          ++i;
          continue;
        }
        if (b == '\n') {
          ++lines;
        }
        if (b == '\t') {
          out += "    ";
        } else if (b != '\r') {
          out.append(raw, i, len);
        }
        i += len;
      }
      return out;
    }

    std::size_t indexOf(const std::vector<std::shared_ptr<const FileInfo>>& rows, const std::string& name) {
      const auto it = std::ranges::find_if(rows, [&](const auto& info) { return info->name == name; });
      return it == rows.end() ? kNone : static_cast<std::size_t>(it - rows.begin());
    }
  } // namespace

  BrowseView::BrowseView(std::filesystem::path dir, ThumbnailService& thumbnails, Host host)
      : m_model(std::move(dir)), m_thumbnails(thumbnails), m_host(std::move(host)) {}

  BrowseView::~BrowseView() { *m_alive = false; }

  void BrowseView::detach() { m_detached = true; }

  void BrowseView::hint(std::string text) {
    m_hint = std::move(text);
    refresh();
  }

  std::unique_ptr<Node> BrowseView::build(float scale, Rect slot, Rect pane) {
    const float s = m_scale = scale;
    const float inset = Tokens::spaceP8 * s;
    const float pad = Tokens::spaceP12 * s;
    const float gap = Tokens::spaceP8 * s;
    m_rowHeight = Tokens::sizeRowCompact * s;

    auto root = ui::node({.width = slot.width, .height = slot.height, .participatesInLayout = false});
    auto shade = ui::box({.fill = colorSpecFromRole(ColorRole::Shadow, Tokens::componentTerminalPaletteDim),
                          .width = pane.width,
                          .height = pane.height});
    shade->setPosition(pane.x, pane.y);
    root->addChild(std::move(shade));

    const float cardWidth = pane.width - 2.0F * inset;
    const float cardHeight = pane.height - 2.0F * inset;
    auto card = ui::column({.align = FlexAlign::Stretch,
                            .gap = gap,
                            .padding = pad,
                            .fill = colorSpecFromRole(ColorRole::Card),
                            .radius = Style::scaledRadius(Tokens::radiusR12, s),
                            .border = colorSpecFromRole(ColorRole::Stroke),
                            .borderWidth = 0.5F * s,
                            .width = cardWidth,
                            .height = cardHeight});
    card->setPosition(pane.x + inset, pane.y + inset);

    auto header = ui::row({.align = FlexAlign::Center, .gap = gap, .minHeight = m_rowHeight, .maxHeight = m_rowHeight});
    header->addChild(ui::glyph({.glyph = std::string("folder"),
                                .glyphSize = Tokens::sizeSymbol * s,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
    header->addChild(ui::label({.out = &m_path,
                                .text = shown(m_model.dir()),
                                .fontSize = Tokens::typeBody.size * s,
                                .color = colorSpecFromRole(ColorRole::Label1),
                                .maxLines = 1,
                                .ellipsize = TextEllipsize::Start,
                                .flexGrow = 1.0F}));
    header->addChild(ui::keycap({.text = "esc", .scale = s}));
    card->addChild(std::move(header));

    if (!m_refusal.empty()) {
      card->addChild(ui::label({.text = m_refusal,
                                .fontSize = Tokens::typeBody.size * s,
                                .color = colorSpecFromRole(ColorRole::Label2),
                                .maxLines = 4}));
      root->addChild(std::move(card));
      auto area = std::make_unique<InputArea>();
      area->setFocusable(true);
      m_area = static_cast<InputArea*>(root->addChild(std::move(area)));
      return root;
    }

    const float inner = cardWidth - 2.0F * pad - 2.0F * gap;
    const float bodyHeight = cardHeight - 2.0F * pad - 2.0F * gap - 2.0F * m_rowHeight;
    m_visible = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(bodyHeight / m_rowHeight)));
    const float parentWidth = std::floor(inner * 0.22F);
    const float folderWidth = std::floor(inner * 0.40F);
    m_previewWidth = inner - parentWidth - folderWidth;
    auto body = ui::row({.align = FlexAlign::Start, .gap = gap, .height = bodyHeight});
    body->addChild(ui::column({.out = &m_parentColumn, .align = FlexAlign::Stretch, .width = parentWidth}));
    body->addChild(ui::column({.out = &m_folderColumn, .align = FlexAlign::Stretch, .width = folderWidth}));
    body->addChild(ui::column(
        {.out = &m_previewColumn, .align = FlexAlign::Stretch, .gap = gap, .width = m_previewWidth, .height = bodyHeight}));
    card->addChild(std::move(body));

    card->addChild(ui::row({.out = &m_footer,
                            .align = FlexAlign::Center,
                            .gap = gap,
                            .minHeight = m_rowHeight,
                            .maxHeight = m_rowHeight}));
    root->addChild(std::move(card));

    // Holds keyboard focus while the mode is open, so the grid and its input method get no keys.
    auto area = std::make_unique<InputArea>();
    area->setFocusable(true);
    area->setSize(cardWidth, cardHeight);
    area->setPosition(pane.x + inset, pane.y + inset);
    m_area = static_cast<InputArea*>(root->addChild(std::move(area)));

    apply(m_model.start());
    m_ready = true;
    syncSide();
    fillAll();
    return root;
  }

  bool BrowseView::key(const KeyboardEvent& event) {
    if (std::holds_alternative<browse::BulkRename>(m_model.input())) {
      return false;
    }
    if (!event.pressed || event.preedit || m_detached) {
      return true;
    }
    if (!m_refusal.empty()) {
      if (m_host.run) {
        m_host.run(browse::effect::Close{});
      }
      return true;
    }
    m_hint.clear();
    const bool idle = m_host.promptIdle && m_host.promptIdle();
    apply(m_model.key({.sym = event.sym, .utf32 = event.utf32, .mods = event.modifiers}, idle));
    if (m_detached) {
      return true;
    }
    syncSide();
    refresh();
    return true;
  }

  void BrowseView::apply(std::vector<browse::Effect> effects) {
    for (browse::Effect& effect : effects) {
      if (m_detached) {
        return;
      }
      if (const auto* list = std::get_if<browse::effect::List>(&effect)) {
        open(list->dir);
      } else if (const auto* filter = std::get_if<browse::effect::Filter>(&effect)) {
        if (m_current != nullptr) {
          m_current->setFilter(filter->text);
        }
      } else if (const auto* hidden = std::get_if<browse::effect::ShowHidden>(&effect)) {
        for (auto* view : {m_current.get(), m_parent.get(), m_child.get()}) {
          if (view != nullptr) {
            view->setShowHidden(hidden->show);
          }
        }
      } else if (auto* hint = std::get_if<browse::effect::Hint>(&effect)) {
        m_hint = std::move(hint->text);
      } else if (m_host.run) {
        m_host.run(effect);
      }
    }
  }

  void BrowseView::open(const std::filesystem::path& dir) {
    m_rows.clear();
    m_offset = 0;
    m_current = std::make_unique<FileModelView>(FileModel::open(dir, watcher()));
    m_current->setShowHidden(m_model.showHidden());
    m_current->setFilter(m_model.filter());
    m_current->setChangedCallback([this] { listed(); });
    listed();
  }

  void BrowseView::listed() {
    if (m_current == nullptr || m_current->model().status() == FileModel::Status::Loading) {
      refresh();
      return;
    }
    m_rows = m_current->rows();
    std::vector<browse::Row> rows;
    rows.reserve(m_rows.size());
    for (const auto& info : m_rows) {
      rows.push_back({.name = info->name, .directory = info->isDir});
    }
    m_model.listed(m_model.dir(), std::move(rows));
    syncSide();
    refresh();
  }

  void BrowseView::syncSide() {
    const auto follow = [this](std::unique_ptr<FileModelView>& view, std::optional<std::filesystem::path> dir,
                               std::size_t& offset) {
      if (!dir) {
        view.reset();
        return;
      }
      const std::filesystem::path want = dir->lexically_normal();
      if (view != nullptr && view->model().directory() == want) {
        return;
      }
      offset = 0;
      view = std::make_unique<FileModelView>(FileModel::open(want, watcher()));
      view->setShowHidden(m_model.showHidden());
      view->setChangedCallback([this] { refresh(); });
    };
    const std::filesystem::path& dir = m_model.dir();
    follow(m_parent, dir.has_relative_path() ? std::optional(dir.parent_path()) : std::nullopt, m_parentOffset);
    const browse::Row* row = m_model.current();
    follow(m_child, row != nullptr && row->directory ? std::optional(dir / row->name) : std::nullopt, m_childOffset);
  }

  void BrowseView::refresh() {
    if (!m_ready || m_detached || !m_refusal.empty()) {
      return;
    }
    fillAll();
    if (m_host.relayout) {
      m_host.relayout();
    }
  }

  void BrowseView::fillAll() {
    m_path->setText(shown(m_model.dir()));
    if (m_parent != nullptr) {
      fillColumn(*m_parentColumn, m_parent->rows(), indexOf(m_parent->rows(), m_model.dir().filename().string()), false,
                 m_parentOffset, statusText(*m_parent));
    } else {
      clear(*m_parentColumn);
    }
    const browse::Input& input = m_model.input();
    if (std::holds_alternative<browse::BulkRename>(input)) {
      fillEditor();
      clear(*m_previewColumn);
      m_icon = nullptr;
      fillFooter();
      return;
    }
    if (m_editor != nullptr) {
      m_editor = nullptr;
      if (m_host.focus) {
        m_host.focus(m_area);
      }
    }
    if (std::holds_alternative<browse::JumpInput>(input)) {
      fillJump();
      clear(*m_previewColumn);
      m_icon = nullptr;
      fillFooter();
      return;
    }
    std::string status = m_current != nullptr ? statusText(*m_current) : std::string();
    if (status.empty() && m_rows.empty()) {
      status = m_model.filter().empty() ? "Empty folder" : "No match";
    }
    fillColumn(*m_folderColumn, m_rows, m_rows.empty() ? kNone : m_model.cursor(), true, m_offset, status);
    fillPreview();
    fillFooter();
  }

  void BrowseView::fillColumn(Flex& column, const std::vector<std::shared_ptr<const FileInfo>>& rows,
                              std::size_t selected, bool cursor, std::size_t& offset, const std::string& status) {
    clear(column);
    const float s = m_scale;
    if (!status.empty()) {
      column.addChild(ui::label({.text = status,
                                 .fontSize = Tokens::typeBody.size * s,
                                 .color = colorSpecFromRole(ColorRole::Label3),
                                 .maxLines = 2}));
      return;
    }
    if (selected != kNone) {
      if (selected < offset) {
        offset = selected;
      } else if (selected >= offset + m_visible) {
        offset = selected + 1 - m_visible;
      }
    }
    offset = std::min(offset, rows.size() > m_visible ? rows.size() - m_visible : 0);
    const std::size_t end = std::min(rows.size(), offset + m_visible);
    for (std::size_t i = offset; i < end; ++i) {
      const FileInfo& info = *rows[i];
      const bool marked = cursor && m_model.marks().contains(info.name);
      ui::FlexProps props{.align = FlexAlign::Center,
                          .gap = Tokens::spaceP8 * s,
                          .paddingH = Tokens::spaceP8 * s,
                          .radius = Style::scaledRadius(Tokens::radiusR8, s),
                          .minHeight = m_rowHeight,
                          .maxHeight = m_rowHeight};
      if (i == selected) {
        props.fill = colorSpecFromRole(cursor ? ColorRole::Lens : ColorRole::Fill2);
      }
      auto row = ui::row(props);
      row->addChild(ui::glyph({.glyph = std::string(marked ? "check" : fileTypeGlyph(info)),
                               .glyphSize = Tokens::sizeSymbol * s,
                               .color = colorSpecFromRole(marked ? ColorRole::Accent : ColorRole::Label2)}));
      row->addChild(ui::label({.text = info.displayName(),
                               .fontSize = Tokens::typeBody.size * s,
                               .color = colorSpecFromRole(marked ? ColorRole::AccentText
                                                                 : (cursor ? ColorRole::Label1 : ColorRole::Label2)),
                               .maxLines = 1,
                               .ellipsize = TextEllipsize::Middle,
                               .flexGrow = 1.0F}));
      column.addChild(std::move(row));
    }
  }

  void BrowseView::fillJump() {
    clear(*m_folderColumn);
    const float s = m_scale;
    const auto matches = m_model.jumpMatches();
    if (matches.empty()) {
      m_folderColumn->addChild(ui::label({.text = std::string("No visited folder matches"),
                                          .fontSize = Tokens::typeBody.size * s,
                                          .color = colorSpecFromRole(ColorRole::Label3)}));
      return;
    }
    const std::size_t pick = std::get<browse::JumpInput>(m_model.input()).pick;
    const std::size_t end = std::min(matches.size(), m_visible);
    const std::size_t first = pick >= end ? pick + 1 - end : 0;
    for (std::size_t i = first; i < std::min(matches.size(), first + m_visible); ++i) {
      ui::FlexProps props{.align = FlexAlign::Center,
                          .gap = Tokens::spaceP8 * s,
                          .paddingH = Tokens::spaceP8 * s,
                          .radius = Style::scaledRadius(Tokens::radiusR8, s),
                          .minHeight = m_rowHeight,
                          .maxHeight = m_rowHeight};
      if (i == pick) {
        props.fill = colorSpecFromRole(ColorRole::Lens);
      }
      auto row = ui::row(props);
      row->addChild(ui::glyph({.glyph = std::string("folder"),
                               .glyphSize = Tokens::sizeSymbol * s,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
      row->addChild(ui::label({.text = shown(matches[i]),
                               .fontSize = Tokens::typeBody.size * s,
                               .color = colorSpecFromRole(ColorRole::Label1),
                               .maxLines = 1,
                               .ellipsize = TextEllipsize::Start,
                               .flexGrow = 1.0F}));
      m_folderColumn->addChild(std::move(row));
    }
  }

  void BrowseView::fillEditor() {
    if (m_editor != nullptr) {
      return;
    }
    clear(*m_folderColumn);
    std::string text;
    for (const std::string& name : std::get<browse::BulkRename>(m_model.input()).names) {
      text += name + "\n";
    }
    m_folderColumn->addChild(ui::input({.out = &m_editor,
                                        .value = std::move(text),
                                        .fontSize = Tokens::typeBody.size * m_scale,
                                        .height = m_rowHeight * static_cast<float>(m_visible),
                                        .onSubmit =
                                            [this](const std::string& value) {
                                              if (m_detached) {
                                                return;
                                              }
                                              apply(m_model.bulkRenamed(value));
                                              refresh();
                                            },
                                        .onKeyEvent =
                                            [this](std::uint32_t sym, std::uint32_t /*mods*/) {
                                              if (sym != 0xff1b || m_detached) { // Escape
                                                return false;
                                              }
                                              m_model.cancelBulk();
                                              refresh();
                                              return true;
                                            }}));
    m_editor->setMultiline(true);
    if (m_host.focus) {
      m_host.focus(m_editor->inputArea());
    }
  }

  void BrowseView::readText(const std::filesystem::path& path) {
    if (path == m_textPath) {
      return;
    }
    m_textPath = path;
    m_text.reset();
    std::thread([path, alive = std::weak_ptr<bool>(m_alive), this] {
      std::optional<std::string> text = readHead(path);
      DeferredCall::callLater([path, alive, this, text = std::move(text)]() mutable {
        const auto live = alive.lock();
        if (live == nullptr || !*live || path != m_textPath) {
          return;
        }
        m_text = std::move(text);
        refresh();
      });
    }).detach();
  }

  void BrowseView::fillPreview() {
    clear(*m_previewColumn);
    m_icon = nullptr;
    m_iconInfo.reset();
    const browse::Row* row = m_model.current();
    if (row == nullptr || m_model.cursor() >= m_rows.size()) {
      return;
    }
    if (m_child != nullptr) {
      std::string status = statusText(*m_child);
      if (status.empty() && m_child->rows().empty()) {
        status = "Empty folder";
      }
      fillColumn(*m_previewColumn, m_child->rows(), kNone, false, m_childOffset, status);
      return;
    }
    const float s = m_scale;
    m_iconInfo = m_rows[m_model.cursor()];
    const float size = std::min(m_previewWidth, m_rowHeight * 5.0F);
    m_icon = static_cast<FileIcon*>(m_previewColumn->addChild(std::make_unique<FileIcon>(size)));
    m_previewColumn->addChild(ui::label({.text = m_iconInfo->displayName(),
                                         .fontSize = Tokens::typeBody.size * s,
                                         .color = colorSpecFromRole(ColorRole::Label1),
                                         .maxLines = 3,
                                         .ellipsize = TextEllipsize::Middle}));
    std::string facts = fileKindLabel(*m_iconInfo);
    if (!m_iconInfo->pendingStat) {
      facts += " · " + formatFileSize(m_iconInfo->size);
      m_previewColumn->addChild(ui::label({.text = std::move(facts),
                                           .fontSize = Tokens::typeFootnote.size * s,
                                           .color = colorSpecFromRole(ColorRole::Label2),
                                           .maxLines = 1}));
      m_previewColumn->addChild(ui::label({.text = formatFileDate(m_iconInfo->mtimeNs),
                                           .fontSize = Tokens::typeFootnote.size * s,
                                           .color = colorSpecFromRole(ColorRole::Label3),
                                           .maxLines = 1}));
    }
    if (isText(*m_iconInfo)) {
      readText(m_model.dir() / m_iconInfo->name);
      if (m_text && !m_text->empty()) {
        m_previewColumn->addChild(ui::label({.text = *m_text,
                                             .fontSize = Tokens::typeFootnote.size * s,
                                             .fontFamily = std::string("monospace"),
                                             .color = colorSpecFromRole(ColorRole::Label2),
                                             .maxLines = static_cast<int>(m_visible)}));
      }
    }
  }

  void BrowseView::fillFooter() {
    clear(*m_footer);
    const float s = m_scale;
    std::string text;
    ColorRole role = ColorRole::Label2;
    const browse::Input& input = m_model.input();
    if (const auto* filter = std::get_if<browse::FilterInput>(&input)) {
      text = "Filter: " + filter->text + "▏";
      role = ColorRole::Label1;
    } else if (const auto* rename = std::get_if<browse::RenameInput>(&input)) {
      text = "Rename to: " + rename->text + "▏";
      role = ColorRole::Label1;
    } else if (const auto* jump = std::get_if<browse::JumpInput>(&input)) {
      text = "Jump to: " + jump->text + "▏   ↑↓ pick · Enter go · Esc back";
      role = ColorRole::Label1;
    } else if (std::holds_alternative<browse::BulkRename>(input)) {
      text = m_hint.empty() ? "One name per line · Ctrl+Enter renames · Esc cancels" : m_hint;
      role = m_hint.empty() ? ColorRole::Label2 : ColorRole::DangerText;
    } else if (const auto* trash = std::get_if<browse::ConfirmTrash>(&input)) {
      text = "Move " + std::to_string(trash->paths.size()) + " to the Trash? Enter or y: yes; any other key: no";
      role = ColorRole::DangerText;
    } else if (!m_hint.empty()) {
      text = m_hint;
      role = ColorRole::Label1;
    } else {
      text = "q cd here · i insert · z jump · o Files · / filter · Space mark · y x p · r R rename · d trash";
    }
    m_footer->addChild(ui::label({.text = std::move(text),
                                  .fontSize = Tokens::typeFootnote.size * s,
                                  .color = colorSpecFromRole(role),
                                  .maxLines = 1,
                                  .ellipsize = TextEllipsize::End,
                                  .flexGrow = 1.0F}));
  }

  void BrowseView::bind(Renderer& renderer) {
    if (m_icon != nullptr && m_iconInfo != nullptr) {
      m_icon->bind(renderer, *m_iconInfo, m_model.dir() / m_iconInfo->name, &m_thumbnails);
    }
  }

} // namespace term
