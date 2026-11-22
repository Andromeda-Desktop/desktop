#pragma once

#include "browse_model.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class FileIcon;
struct FileInfo;
class FileModelView;
class Input;
class Flex;
class InputArea;
class Label;
class Node;
class Renderer;
class ThumbnailService;
struct KeyboardEvent;

namespace term {

  // Browse mode (Ctrl+Shift+O): parent | folder | preview columns over a pane, driven by browse::Model. The view
  // lists folders (FileModel) and runs List, Filter, ShowHidden and Hint itself; the host runs every other effect.
  // The root is out of its parent's layout: the owner calls root->layout() after build() and after each relayout
  // request, then bind().
  class BrowseView {
  public:
    struct Host {
      std::function<void(const browse::Effect&)> run; // Cd, Insert, Open, ShowInFiles, Copy, Move, Trash, Rename, Close
      std::function<bool()> promptIdle;
      std::function<void()> relayout;
      std::function<void(InputArea*)> focus; // moves keyboard focus (the bulk rename editor and back)
    };
    struct Rect {
      float x = 0.0F;
      float y = 0.0F;
      float width = 0.0F;
      float height = 0.0F;
    };

    BrowseView(std::filesystem::path dir, ThumbnailService& thumbnails, Host host);
    ~BrowseView();
    BrowseView(const BrowseView&) = delete;
    BrowseView& operator=(const BrowseView&) = delete;

    // Before build(): the card shows only why the mode cannot browse (a remote shell); any key closes it.
    void refuse(std::string reason) { m_refusal = std::move(reason); }
    // pane: the area the card covers (inset p8); slot: the whole content area.
    [[nodiscard]] std::unique_ptr<Node> build(float scale, Rect slot, Rect pane);
    [[nodiscard]] InputArea* focusArea() const { return m_area; }
    // False when the key belongs to the focused editor (bulk rename): the window passes it on.
    bool key(const KeyboardEvent& event);
    // Jump targets for z, most recent first.
    void setFolders(std::vector<std::filesystem::path> folders) { m_model.setFolders(std::move(folders)); }
    // After layout: binds the preview thumbnail.
    void bind(Renderer& renderer);
    // A message under the columns until the next key (an operation's result).
    void hint(std::string text);
    // The host callbacks stop: nodes still in the scene call nothing.
    void detach();

  private:
    void apply(std::vector<browse::Effect> effects);
    void open(const std::filesystem::path& dir);
    void listed();
    void syncSide();
    void refresh();
    void fillAll();
    void fillColumn(Flex& column, const std::vector<std::shared_ptr<const FileInfo>>& rows, std::size_t selected,
                    bool cursor, std::size_t& offset, const std::string& status);
    void fillPreview();
    void fillFooter();
    void fillJump();
    void fillEditor();
    void readText(const std::filesystem::path& path);

    browse::Model m_model;
    ThumbnailService& m_thumbnails;
    Host m_host;
    bool m_detached = false;
    bool m_ready = false; // build() finished: refresh() may fill the columns
    std::unique_ptr<FileModelView> m_current;
    std::unique_ptr<FileModelView> m_parent;
    std::unique_ptr<FileModelView> m_child; // the cursor's folder, for the preview column
    std::vector<std::shared_ptr<const FileInfo>> m_rows; // m_current's rows at the last listed()
    std::string m_hint;
    std::string m_refusal;
    float m_scale = 1.0F;
    float m_rowHeight = 0.0F;
    std::size_t m_visible = 1; // rows a column can show
    std::size_t m_offset = 0;  // the first shown row of the folder column
    std::size_t m_parentOffset = 0;
    std::size_t m_childOffset = 0;
    InputArea* m_area = nullptr;
    Label* m_path = nullptr;
    Flex* m_parentColumn = nullptr;
    Flex* m_folderColumn = nullptr;
    Flex* m_previewColumn = nullptr;
    Flex* m_footer = nullptr;
    FileIcon* m_icon = nullptr; // in the preview column while the cursor is on a file
    std::shared_ptr<const FileInfo> m_iconInfo;
    float m_previewWidth = 0.0F;
    Input* m_editor = nullptr;           // the bulk rename editor in the folder column
    std::filesystem::path m_textPath;    // the file whose head m_text holds or is being read
    std::optional<std::string> m_text;   // none: not read yet, or not text
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // worker results check it
  };

} // namespace term
