#pragma once

#include "printing/cups_printers.h"
#include "print_portal.h"
#include "render/core/texture_handle.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class AppLoop;
class Button;
class Flex;
class Input;
class InputArea;
class Label;
class Node;
class Renderer;
struct KeyboardEvent;

namespace preview {
  class HelperClient;
}

// The Print sheet (.agent/mockups/system-dialogs.html §3, states in print-states.html): the document preview on the
// left, the printer and the few choices that matter on the right.
class PrintSheet {
public:
  struct Callbacks {
    std::function<void(const PrintOptions&, const std::string& printerLabel, int pages)> print;
    std::function<void()> savePdf; // pick a path and save the document as the app made it
    std::function<void()> cancel;
    std::function<void()> rebuild;
  };

  struct Pixels {
    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    int stride = 0;
    TextureHandle texture; // uploaded once at the first build that shows it; bgra is freed then
  };

  PrintSheet(AppLoop& loop, PrintPortal::PrintRequest request, Callbacks callbacks);
  ~PrintSheet();
  // The GL context is gone: drops the uploaded pages (their pixels were freed at upload); a build requests them again.
  // Renders in flight stay requested.
  void abandonGpuResources();
  PrintSheet(const PrintSheet&) = delete;
  PrintSheet& operator=(const PrintSheet&) = delete;

  [[nodiscard]] std::unique_ptr<Node> build(Renderer& renderer, float scale, float width, float height);
  // Esc goes back from More Options, else cancels; Enter prints. Both also work from the range field and run
  // before the app's own key commands.
  // focused: the window's keyboard focus; Enter goes to a focused control that is not a text field.
  bool firstKey(const KeyboardEvent& event, const InputArea* focused);
  bool key(const KeyboardEvent& event); // Left / Right turn sheets

private:
  enum class Printers { Loading, Loaded };
  enum class Preview { Loading, Ready, Failed };
  enum class Screen { Main, More };
  enum class Target { Printer, SaveAsPdf };
  enum class Size { Page, Thumb };

  void startPreview();
  void requestPage(int page, Size size);
  void selectPrinter(std::size_t index);
  void selectSaveAsPdf();
  void submit();
  void showSheet(int sheet);
  [[nodiscard]] const PrinterInfo* printer() const;
  [[nodiscard]] bool rangeValid() const;
  [[nodiscard]] int pageLimit() const;
  void validateRange();
  void showRange();
  [[nodiscard]] std::vector<int> pagesToPrint() const; // 0-based
  [[nodiscard]] int perSheet() const;
  [[nodiscard]] std::unique_ptr<Node> buildPreview(Renderer& renderer, float scale, float width, float height);
  [[nodiscard]] std::unique_ptr<Node> buildSheet(Renderer& renderer, int sheet, const std::vector<int>& pages, float height, Size size,
                                                 bool selected);
  void buildMain(Node& column, float scale);
  void buildMore(Node& column, float scale);

  AppLoop& m_loop;
  PrintPortal::PrintRequest m_request;
  Callbacks m_cb;
  PrintOptions m_options;
  std::string m_appMedia; // the paper the app laid its pages out for

  Printers m_printersState = Printers::Loading;
  std::vector<PrinterInfo> m_printers;
  std::size_t m_printer = 0;
  Target m_target = Target::Printer;
  Screen m_screen = Screen::Main;

  bool m_allPages = true;
  std::string m_rangeText;
  std::string m_rangeError;
  // The range field updates these in place: a rebuild would take its focus and caret. Valid until the next build.
  std::size_t m_rangeCaret = std::numeric_limits<std::size_t>::max(); // byte offset; selectRange clamps max to the end
  Input* m_rangeInput = nullptr;
  Flex* m_rangeNote = nullptr;
  Label* m_rangeNoteLabel = nullptr;
  Button* m_printButton = nullptr;

  Preview m_preview = Preview::Loading;
  std::string m_tempPath;
  std::unique_ptr<preview::HelperClient> m_helper;
  int m_pageCount = 0;
  int m_sheet = 0;
  std::map<int, std::shared_ptr<Pixels>> m_pages;  // 0-based page → preview pixels
  std::map<int, std::shared_ptr<Pixels>> m_thumbs; // 0-based page → thumbnail pixels
  std::set<std::pair<int, Size>> m_requested;
  std::vector<TextureHandle> m_dropped; // textures of evicted pages, unloaded at the next build

  bool m_sent = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
