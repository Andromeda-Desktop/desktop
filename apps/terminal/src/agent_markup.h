#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace term::agent {

  // One block of an agent reply. Prose and list items hold Pango markup; code holds plain text.
  struct Block {
    enum class Kind : unsigned char { Paragraph, Item, Code, Table };
    Kind kind;
    std::string text;
    // Table only: the header row first; every row has the header's column count; cells hold Pango markup.
    std::vector<std::vector<std::string>> rows = {};
  };

  // The Markdown subset agents write: paragraphs, `code`, **bold**, *italic*, fenced code, - and 1. lists, pipe tables.
  // An unclosed fence (a reply still streaming) ends the code block at the end of the text.
  [[nodiscard]] std::vector<Block> markdownBlocks(std::string_view markdown);

  [[nodiscard]] std::string escapeMarkup(std::string_view text);

} // namespace term::agent
