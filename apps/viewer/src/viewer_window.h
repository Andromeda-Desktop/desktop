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
#include <vector>

class AppLoop;
class AppWindow;
class Image;
class Label;
class Box;
class Input;
class Node;
class Renderer;
class ScrollView;
struct KeyboardEvent;

namespace viewer {


  // One Viewer window (.agent/DESIGN-viewer.md): a PDF with thumbnails, search and text selection, or an image with
  // its folder neighbours.
  class ViewerWindow {
  public:
    enum class Tool : std::uint8_t { Select, Highlight, Note, Draw, Sign };

    ViewerWindow(AppLoop& loop, AppWindow& window);
    ~ViewerWindow();
    ViewerWindow(const ViewerWindow&) = delete;
    ViewerWindow& operator=(const ViewerWindow&) = delete;

    void open(const std::filesystem::path& path);
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    // Commands (the app's command table calls these for the active window).
    void focusSearch();
    void searchFor(const std::string& query);
    void copySelection();
    void zoomIn();
    void zoomOut();
    void zoomFit();
    void toggleSidebar();
    void step(int direction);
    void undo();
    void save();
    void setTool(Tool tool);

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
      std::vector<RectF> rects;
      std::string text;
    };

    [[nodiscard]] std::unique_ptr<Node> build(Renderer& renderer, float width, float height);
    [[nodiscard]] std::unique_ptr<Node> buildToolbar(float width);
    [[nodiscard]] std::unique_ptr<Node> buildPages(Renderer& renderer);
    [[nodiscard]] std::unique_ptr<Node> buildSidebar(Renderer& renderer);
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
    [[nodiscard]] bool onKey(const KeyboardEvent& event);
    [[nodiscard]] std::size_t currentPage() const;
    void onPagePress(std::size_t page, float x, float y, bool pressed);
    void onPageMotion(float x, float y);
    void addAnnotation(std::size_t page, std::uint32_t kind, std::uint32_t rgb, const std::vector<std::uint8_t>& body);
    void rerenderPage(std::size_t page);
    void placeSignature(std::size_t page, float x, float y);
    void updateTitle();
    [[nodiscard]] std::unique_ptr<Node> buildSignaturePad();

    AppLoop& m_loop;
    AppWindow& m_window;
    Mode m_mode = Mode::Empty;
    std::filesystem::path m_path;
    std::string m_error;
    std::uint64_t m_generation = 0;
    std::uint64_t m_searchGeneration = 0;

    std::unique_ptr<preview::HelperClient> m_helper;
    DocumentModel m_model;
    std::vector<Page> m_pages;
    std::vector<Page> m_thumbs;
    std::unordered_map<std::uint64_t, Tile> m_tiles; // by tileKey
    std::vector<Job> m_jobs;
    int m_inFlight = 0;
    std::uint64_t m_renderSeq = 0;
    std::vector<PendingInk> m_pendingInk;
    std::vector<Node*> m_tileLayers;
    // Textures of evicted pages; build() unloads them, as only it has the renderer.
    std::vector<TextureHandle> m_dropped;
    ScrollView* m_thumbScroll = nullptr;
    bool m_sidebar = true;
    ScrollView* m_scroll = nullptr;  // vertical; holds m_hscroll, which pans a zoomed page sideways
    ScrollView* m_hscroll = nullptr;
    Node* m_column = nullptr;         // the page column inside m_hscroll
    // Nodes of the last build, by page; a build replaces them all.
    std::vector<Image*> m_pageImages;
    std::vector<Image*> m_thumbImages;
    std::vector<Box*> m_thumbFrames;
    std::vector<Node*> m_markLayers;
    Label* m_positionLabel = nullptr;
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
    bool m_selecting = false;
    bool m_selectPending = false;
    bool m_selectDirty = false;

    Tool m_tool = Tool::Select;
    // Helper annotation ids in the order they were made (Ctrl+Z removes the last).
    std::vector<std::uint32_t> m_edits;
    bool m_unsaved = false;
    bool m_saving = false;
    std::optional<std::pair<std::size_t, annot::Point>> m_notePoint;
    std::size_t m_strokePage = 0;
    annot::Stroke m_stroke;
    bool m_drawing = false;
    std::optional<std::pair<std::size_t, annot::Point>> m_pendingSign;
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
