#pragma once

#include "annotations.h"
#include "document_model.h"
#include "file_kind.h"
#include "helper_client.h"
#include "image_loader.h"
#include "render/animation/animation_manager.h"
#include "render/core/texture_handle.h"
#include "render/scene/input_area.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

class AppLoop;
class AppWindow;
class Image;
class Label;
class Box;
class Flex;
class Input;
class Node;
class Renderer;
class ScrollView;
class StateStore;
struct KeyboardEvent;

namespace viewer {


  // One Viewer window (.agent/DESIGN-viewer.md): a PDF with thumbnails, search and text selection, or an image with
  // its folder neighbours.
  class ViewerWindow {
  public:
    enum class Tool : std::uint8_t { Select, Highlight, Note, Draw, Sign };

    // Helper jobs in flight across every document of one window; `released` runs when one ends.
    struct RenderBudget {
      int inFlight = 0;
      std::function<void(ViewerWindow* from)> released;
    };

    ViewerWindow(AppLoop& loop, AppWindow& window, StateStore& state, RenderBudget& budget);
    ~ViewerWindow();
    ViewerWindow(const ViewerWindow&) = delete;
    ViewerWindow& operator=(const ViewerWindow&) = delete;

    void open(const std::filesystem::path& path);
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    // Commands (the app's command table calls these for the active window).
    void focusSearch();
    void searchFor(const std::string& query);
    void copySelection();
    void selectAll();
    void zoomIn();
    void zoomOut();
    void zoomFit();
    void zoomFitPage();
    void zoomActual();
    void toggleSidebar();
    void step(int direction);
    void undo();
    void redo();
    void save() { saveThen({}); }
    void setTool(Tool tool);

    // ViewerTabs drives these for the tab it shows. The window builds the caption, then the content, then the sidebar.
    [[nodiscard]] std::unique_ptr<Node> build(Renderer& renderer, float width, float height);
    // The caption's tools: page, zoom, tool picker; a find button while the sidebar is hidden.
    [[nodiscard]] std::unique_ptr<Node> buildCaption();
    // The sidebar's search capsule; null when the document cannot be searched.
    [[nodiscard]] std::unique_ptr<Node> buildSearch();
    [[nodiscard]] bool isPdf() const noexcept { return m_mode == Mode::Pdf; }
    // The sidebar's switch (Pages, Outline, Results) and its list; null when there is no page list.
    [[nodiscard]] std::unique_ptr<Node> buildSidebarList(Renderer& renderer, float width);
    [[nodiscard]] bool onKey(const KeyboardEvent& event);
    void onLayout();
    [[nodiscard]] bool unsaved() const { return m_savedDepth != m_edits.size(); }
    [[nodiscard]] std::string title() const { return m_path.filename().string(); }
    void setTitleChanged(std::function<void()> changed) { m_titleChanged = std::move(changed); }
    // Another tab shows: this one's scene nodes are about to go; its next build makes them again.
    void detach();
    // Sends queued render jobs while the window's budget has room.
    void resume() { pump(); }
    // Another pane of the split takes focus: this one's caption tools and sidebar nodes go; its pages stay.
    void blur() {
      forgetCaption();
      forgetSidebar();
    }
    // Asks Save / Don't Save / Cancel; `close` runs once the tab may close.
    void askSaveBeforeClose(std::function<void()> close);

  private:
    enum class Mode : std::uint8_t { Empty, Loading, Pdf, Image, Error };
    struct Page {
      std::shared_ptr<preview::MappedFd> pixels;
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      std::uint32_t stride = 0;
      std::uint32_t requestedWidth = 0;
      // Uploaded on the next build; the pixels are then dropped.
      TextureHandle texture;
    };
    struct Job {
      enum class Kind : std::uint8_t { Page, Tile, Thumb };
      Kind kind = Kind::Page;
      std::size_t page = 0;
      std::uint32_t width = 0; // the whole page width in device px
      std::uint32_t x = 0;     // tile only: its rect in device px of that width
      std::uint32_t y = 0;
      std::uint32_t w = 0;
      std::uint32_t h = 0;
      std::uint64_t key = 0;
    };
    struct Tile {
      std::size_t page = 0;
      std::uint32_t pageWidth = 0; // 0 after an edit: rendered again on the next pass
      std::uint32_t x = 0;
      std::uint32_t y = 0;
      Page px;
      Image* node = nullptr;
    };
    // A finished stroke stays drawn until a page render made after the helper took it shows it.
    struct PendingInk {
      std::size_t page = 0;
      annot::Stroke stroke;
      std::optional<std::uint64_t> shownAfter; // set when the helper took the stroke
    };
    struct Selection {
      std::size_t page = 0;
      float x0 = 0.0F; // points, page-local
      float y0 = 0.0F;
      float x1 = 0.0F;
      float y1 = 0.0F;
      std::uint32_t unit = preview::kSelectGlyph; // a double click selects words, a triple click lines
      std::vector<RectF> rects;
      std::string text;
    };

    [[nodiscard]] std::unique_ptr<Node> buildPages(Renderer& renderer);
    [[nodiscard]] std::unique_ptr<Node> buildThumbs(Renderer& renderer, float width);
    [[nodiscard]] std::unique_ptr<Node> buildNoteField(std::size_t page, annot::Point at, float pageWidth);
    [[nodiscard]] float pageAreaWidth() const;
    float m_areaWidth = 0.0F; // the width of the last build: the window's content or this document's pane
    [[nodiscard]] std::unique_ptr<Node> buildOutline(float width);
    [[nodiscard]] std::unique_ptr<Node> buildResults(float width);
    // One SELECT_LINE at a time fills the snippet of the next built result row that has none.
    void requestSnippet();
    [[nodiscard]] bool isCurrentRow(std::size_t page, std::size_t index) const; // the row shown as the current hit
    void refreshResults();
    void requestOutline();
    // The outline entry the current page belongs to: the last one that starts at or before it.
    [[nodiscard]] std::optional<std::size_t> currentEntry() const;
    [[nodiscard]] std::unique_ptr<Node> buildMessage(const std::string& title, const std::string& caption);

    void openPdf();
    void openImage();
    void loadFolder();
    void fail(std::string reason);
    void dropHelper();
    void requestVisiblePages();
    void requestThumbs();
    void queueTiles();
    void queueThumbs();
    void pump();
    [[nodiscard]] static std::uint64_t tileKey(std::size_t page, std::uint32_t tx, std::uint32_t ty);
    void dropTile(Tile& tile);
    void requestTile(const Job& job);
    void showTile(Tile& tile);
    [[nodiscard]] Image* placeTile(const Tile& tile);
    void requestRender(std::vector<Page>& cache, std::size_t page, std::uint32_t width);
    void dropPage(Page& page);
    void showRender(std::vector<Page>& cache, std::size_t page);
    void fillMarks(std::size_t page);
    void addInkSegment(Node* layer, const annot::Point& a, const annot::Point& b);
    // Search hits, the selection, the stroke and the toolbar counts change in place, without a rebuild.
    void refreshMarks();
    void refreshLabels();
    [[nodiscard]] TextureHandle pageTexture(Renderer& renderer, Page& page);
    void onScroll();
    void search(const std::string& query);
    void scanNext();
    void showCurrentHit();
    void moveHit(bool forward);
    void beginSelect(std::size_t page, float x, float y);
    void updateSelect(float x, float y);
    void requestSelect();
    void rebuild();
    [[nodiscard]] std::size_t currentPage() const;
    void onPagePress(std::size_t page, float x, float y, bool pressed, std::uint32_t time);
    void goToPage(std::size_t page);
    void onPageMotion(float x, float y);
    struct Edit {
      std::uint32_t id = 0; // the helper's annotation id
      std::size_t page = 0;
      std::uint32_t kind = 0;
      std::uint32_t rgb = 0;
      std::vector<std::uint8_t> body;
    };
    // A new edit by the user: it ends the redo history.
    void addAnnotation(std::size_t page, std::uint32_t kind, std::uint32_t rgb, std::vector<std::uint8_t> body);
    // `replaces`: the id of an edit this one takes the place of in the undo history (a moved stamp).
    void sendAnnotation(Edit edit, std::optional<std::uint32_t> replaces = std::nullopt);
    // True when the press or release went to the selected stamp.
    bool stampPress(std::size_t page, float x, float y, bool pressed);
    void moveStamp(const RectF& box);
    void rerenderPage(std::size_t page);
    void placeSignature(std::size_t page, float x, float y);
    void updateTitle();
    // Forget the scene nodes of one build part: the tree they were in is gone.
    void forgetNodes();
    void forgetCaption();
    void forgetSidebar();
    // `then` runs once the file on disk holds every edit.
    void saveThen(std::function<void()> then);
    [[nodiscard]] std::unique_ptr<Node> buildSignaturePad();

    // Per document (key: SHA-1 of the path): page, fit and zoom, for the 50 documents changed last.
    struct ViewState {
      std::size_t page = 0;
      DocumentModel::Fit fit = DocumentModel::Fit::Width;
      float zoom = 1.0F;
      bool operator==(const ViewState&) const = default;
    };
    void restoreViewState();
    void saveViewState();

    AppLoop& m_loop;
    AppWindow& m_window;
    StateStore& m_state;
    std::optional<std::size_t> m_restorePage; // applied at the first layout that keeps the scene
    std::optional<ViewState> m_savedView;
    std::string m_documentKey; // SHA-1 of the canonical path: the entry under /viewer/documents
    Mode m_mode = Mode::Empty;
    std::filesystem::path m_path;
    std::string m_error;
    std::uint64_t m_generation = 0;
    std::uint64_t m_searchGeneration = 0;

    std::unique_ptr<preview::HelperClient> m_helper;
    DocumentModel m_model;
    std::vector<Page> m_pages;
    std::vector<Page> m_thumbs;
    // A link area in points and where it goes: a page index or a URI.
    struct Link {
      RectF area;
      std::variant<std::size_t, std::string> target;
    };
    struct PageLinks {
      enum class State : std::uint8_t { Unasked, Asked, Known };
      State state = State::Unasked;
      std::vector<Link> links;
    };
    std::vector<PageLinks> m_links;
    struct OutlineEntry {
      std::uint32_t depth = 0;
      std::optional<std::size_t> page;
      std::string title;
    };
    std::vector<OutlineEntry> m_outline;
    // Results exists only while a search is open (m_searchText non-empty); closing it returns to m_viewBeforeSearch.
    enum class SidebarView : std::uint8_t { Pages, Outline, Results };
    SidebarView m_sidebarView = SidebarView::Pages;
    SidebarView m_viewBeforeSearch = SidebarView::Pages;
    void setSidebarView(SidebarView view);
    // Result rows of the last sidebar build, in page order: at most kMaxResultRows from the first hit.
    struct ResultRow {
      std::size_t page = 0;
      std::size_t index = 0; // in m_model.hits(page)
      Flex* row = nullptr;
      Label* snippet = nullptr;
    };
    std::vector<ResultRow> m_resultRows;
    Node* m_resultList = nullptr;
    float m_resultWidth = 0.0F;
    Label* m_resultCount = nullptr;
    std::optional<std::pair<std::size_t, std::size_t>> m_shownHit;
    std::unordered_map<std::uint64_t, std::string> m_snippets; // by page << 32 | index, for m_searchGeneration
    bool m_snippetPending = false;
    std::vector<Flex*> m_outlineRows;
    std::optional<std::size_t> m_shownEntry;
    void requestLinks(std::size_t page);
    [[nodiscard]] const Link* linkAt(std::size_t page, float x, float y) const;
    void follow(const Link& link);
    std::unordered_map<std::uint64_t, Tile> m_tiles; // by tileKey
    std::vector<Job> m_jobs;
    int m_inFlight = 0; // this document's share of m_budget.inFlight
    RenderBudget& m_budget;
    void takeJob();
    void endJob();
    // Stale replies are dropped: this document's jobs leave the window's budget at once.
    void releaseJobs();
    std::uint64_t m_renderSeq = 0;
    std::vector<PendingInk> m_pendingInk;
    std::vector<Node*> m_tileLayers;
    // Textures of evicted pages; build() unloads them, as only it has the renderer.
    std::vector<TextureHandle> m_dropped;
    ScrollView* m_thumbScroll = nullptr;
    ScrollView* m_scroll = nullptr;  // vertical; holds m_hscroll, which pans a zoomed page sideways
    ScrollView* m_hscroll = nullptr;
    Node* m_column = nullptr;         // the page column inside m_hscroll
    // Nodes of the last build, by page; a build replaces them all.
    std::vector<Image*> m_pageImages;
    std::vector<Image*> m_thumbImages;
    std::vector<Box*> m_thumbFrames;
    std::vector<Node*> m_markLayers;
    Input* m_pageInput = nullptr;
    Label* m_countLabel = nullptr;
    std::size_t m_shownPage = 0;
    float m_hOffset = 0.0F;
    float m_zoomFrom = 1.0F; // the next page column starts at this scale and springs to 1
    AnimationManager::Id m_zoomAnim = 0;
    // (anchorX, anchorY): the viewport point that stays in place (negative = the middle); animate springs the column
    // from the old size to the new one.
    void zoomTo(const std::function<void()>& apply, float anchorX = -1.0F, float anchorY = -1.0F, bool animate = true);
    [[nodiscard]] float widestPage() const;
    void pinch(const InputArea::PinchData& data);
    [[nodiscard]] bool wheelZoom(const InputArea::PointerData& data);
    float m_zoomAnchorX = 0.0F;   // viewport point of the column's scale origin
    float m_zoomAnchorY = 0.0F;
    float m_pinchStart = 1.0F;    // model zoom when the pinch began
    float m_pinchScale = 1.0F;
    float m_pinchX = 0.0F;        // viewport point under the fingers at the pinch start
    float m_pinchY = 0.0F;    // live pinch scale on the column, clamped to the zoom limits
    float m_scrollOffset = 0.0F;
    bool m_restoreScroll = false;
    Input* m_searchInput = nullptr;
    Input* m_noteInput = nullptr;
    bool m_focusNote = false; // the next layout focuses the note field
    std::string m_searchText;
    bool m_scanPending = false;
    std::optional<Selection> m_selection;
    // A middle-button drag pans: the pointer (scene px) and both scroll offsets at the press.
    struct Pan {
      float x = 0.0F;
      float y = 0.0F;
      float top = 0.0F;
      float left = 0.0F;
    };
    std::optional<Pan> m_pan;
    bool m_selecting = false;
    // The last left press on a page, to count double and triple clicks.
    struct Click {
      std::uint32_t time = 0; // ms
      std::size_t page = 0;
      float x = 0.0F;
      float y = 0.0F;
      int count = 0;
    };
    Click m_lastClick;
    bool m_selectPending = false;
    bool m_selectDirty = false;

    Tool m_tool = Tool::Select;
    // Annotations in the order they were made (Ctrl+Z removes the last); m_undone holds what Ctrl+Shift+Z adds again.
    std::vector<Edit> m_edits;
    std::vector<Edit> m_undone;
    // m_edits.size() when the file on disk last matched; empty once undo went below it (a redo adds a new
    // annotation with a new id, so the file never matches again).
    std::optional<std::size_t> m_savedDepth = 0;
    bool m_saving = false;
    std::optional<std::pair<std::size_t, annot::Point>> m_notePoint;
    std::size_t m_strokePage = 0;
    annot::Stroke m_stroke;
    bool m_drawing = false;
    std::optional<std::pair<std::size_t, annot::Point>> m_pendingSign;
    // The placed signature the user can move (drag inside) or resize (drag the corner handle); points.
    struct StampFocus {
      enum class Drag : std::uint8_t { None, Move, Resize };
      std::uint32_t id = 0;
      std::size_t page = 0;
      RectF at;  // where the helper has it
      RectF box; // where the frame shows it
      Drag drag = Drag::None;
      annot::Point grab;
    };
    std::optional<StampFocus> m_stamp;
    std::function<void()> m_titleChanged;
    std::string m_shownTitle;
    bool m_signing = false;
    std::vector<annot::Stroke> m_padStrokes;
    bool m_padDrawing = false;
    // The selection under the Highlight tool turns into a highlight when its last select reply arrives.
    bool m_highlightOnSelect = false;

    preview::ImageDecoder m_decoder;
    preview::Pixels m_image;
    std::vector<std::filesystem::path> m_folder;
    std::size_t m_folderIndex = 0;
  };

} // namespace viewer
