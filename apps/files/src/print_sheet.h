#pragma once

#include "cups_printers.h"
#include "print_portal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class AppLoop;
class Node;
struct KeyboardEvent;

namespace preview {
  class HelperClient;
}

// The Print sheet (.agent/mockups/system-dialogs.html §3, states in print-states.html): the document preview on the
// left, the printer and the few choices that matter on the right.
class PrintSheet {
public:
  struct Callbacks {
    std::function<void(const PrintOptions&)> print;
    std::function<void()> cancel;
    std::function<void()> rebuild;
  };

  struct Pixels {
    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    int stride = 0;
  };

  PrintSheet(AppLoop& loop, PrintPortal::PrintRequest request, Callbacks callbacks);
  ~PrintSheet();
  PrintSheet(const PrintSheet&) = delete;
  PrintSheet& operator=(const PrintSheet&) = delete;

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  // Esc cancels and Enter prints, also from the range field; they run before the app's own key commands.
  bool firstKey(const KeyboardEvent& event);
  bool key(const KeyboardEvent& event); // Left / Right turn pages

private:
  enum class Printers { Loading, Loaded };
  enum class Preview { Loading, Ready, Failed };

  void startPreview();
  void renderPage(int page);
  void selectPrinter(std::size_t index);
  void submit();
  [[nodiscard]] const PrinterInfo* printer() const;
  [[nodiscard]] bool rangeValid() const;

  AppLoop& m_loop;
  PrintPortal::PrintRequest m_request;
  Callbacks m_cb;
  PrintOptions m_options;
  std::string m_appMedia; // the paper the app laid its pages out for

  Printers m_printersState = Printers::Loading;
  std::vector<PrinterInfo> m_printers;
  std::size_t m_printer = 0;

  bool m_allPages = true;
  std::string m_rangeText;
  std::string m_rangeError;

  Preview m_preview = Preview::Loading;
  std::string m_tempPath;
  std::unique_ptr<preview::HelperClient> m_helper;
  int m_pageCount = 0;
  int m_page = 0;
  std::shared_ptr<Pixels> m_pagePixels;
  std::uint64_t m_renderGeneration = 0;

  bool m_sent = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
