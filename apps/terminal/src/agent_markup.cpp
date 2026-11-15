#include "agent_markup.h"

#include <cctype>

namespace term::agent {

  std::string escapeMarkup(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
      switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
      }
    }
    return out;
  }

  namespace {

    // Inline spans: `code` first (its content is literal), then **bold** and *italic*. An unmatched marker stays text.
    std::string inlineMarkup(std::string_view line) {
      std::string out;
      bool bold = false;
      bool italic = false;
      for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '`') {
          if (const auto end = line.find('`', i + 1); end != std::string_view::npos) {
            out += "<tt>" + escapeMarkup(line.substr(i + 1, end - i - 1)) + "</tt>";
            i = end;
            continue;
          }
        }
        if (c == '*' && i + 1 < line.size() && line[i + 1] == '*'
            && (bold || line.find("**", i + 2) != std::string_view::npos)) {
          out += bold ? "</b>" : "<b>";
          bold = !bold;
          ++i;
          continue;
        }
        if (c == '*' && (italic || line.find('*', i + 1) != std::string_view::npos)) {
          out += italic ? "</i>" : "<i>";
          italic = !italic;
          continue;
        }
        out += escapeMarkup(std::string_view(&line[i], 1));
      }
      if (italic) {
        out += "</i>";
      }
      if (bold) {
        out += "</b>";
      }
      return out;
    }

    // The text after a list marker ("- ", "* ", "12. "), or npos when the line is no list item.
    std::size_t itemStart(std::string_view line) {
      if (line.starts_with("- ") || line.starts_with("* ")) {
        return 2;
      }
      std::size_t i = 0;
      while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])) != 0) {
        ++i;
      }
      return i > 0 && line.substr(i).starts_with(". ") ? i + 2 : std::string_view::npos;
    }

    std::string_view trim(std::string_view text) {
      const auto first = text.find_first_not_of(" \t");
      return first == std::string_view::npos ? std::string_view() : text.substr(first, text.find_last_not_of(" \t") - first + 1);
    }

    // "|---|:--:|": the line under a table header.
    bool tableRule(std::string_view line) {
      line = trim(line);
      return line.find('|') != std::string_view::npos && line.find('-') != std::string_view::npos
             && line.find_first_not_of("|-: \t") == std::string_view::npos;
    }

    // "| a | b |" -> {"a", "b"}; an escaped \| stays in its cell.
    std::vector<std::string> tableCells(std::string_view line) {
      line = trim(line);
      if (line.starts_with('|')) {
        line.remove_prefix(1);
      }
      if (line.ends_with('|') && !line.ends_with("\\|")) {
        line.remove_suffix(1);
      }
      std::vector<std::string> cells;
      std::string cell;
      for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '|') {
          cell += '|';
          ++i;
        } else if (line[i] == '|') {
          cells.push_back(inlineMarkup(trim(cell)));
          cell.clear();
        } else {
          cell += line[i];
        }
      }
      cells.push_back(inlineMarkup(trim(cell)));
      return cells;
    }

  } // namespace

  std::vector<Block> markdownBlocks(std::string_view markdown) {
    std::vector<Block> blocks;
    std::string paragraph;
    const auto endParagraph = [&] {
      if (!paragraph.empty()) {
        blocks.push_back({Block::Kind::Paragraph, inlineMarkup(paragraph)});
        paragraph.clear();
      }
    };
    bool inCode = false;
    std::string code;
    std::size_t pos = 0;
    while (pos <= markdown.size()) {
      const auto eol = markdown.find('\n', pos);
      const std::string_view line = markdown.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
      pos = eol == std::string_view::npos ? markdown.size() + 1 : eol + 1;
      if (line.starts_with("```")) {
        if (inCode) {
          blocks.push_back({Block::Kind::Code, code});
          code.clear();
        } else {
          endParagraph();
        }
        inCode = !inCode;
        continue;
      }
      if (inCode) {
        if (!code.empty()) {
          code += '\n';
        }
        code += line;
        continue;
      }
      if (line.find_first_not_of(" \t") == std::string_view::npos) {
        endParagraph();
        continue;
      }
      if (line.find('|') != std::string_view::npos && pos <= markdown.size()
          && tableRule(markdown.substr(pos, markdown.find('\n', pos) - pos))) {
        endParagraph();
        Block table{Block::Kind::Table, {}};
        table.rows.push_back(tableCells(line));
        const std::size_t columns = table.rows.front().size();
        const std::string_view rule = trim(markdown.substr(pos, markdown.find('\n', pos) - pos));
        std::size_t cellStart = rule.starts_with('|') ? 1 : 0;
        while (table.alignEnd.size() < columns && cellStart < rule.size()) {
          const auto bar = rule.find('|', cellStart);
          table.alignEnd.push_back(trim(rule.substr(cellStart, bar - cellStart)).ends_with(':'));
          cellStart = bar == std::string_view::npos ? rule.size() : bar + 1;
        }
        table.alignEnd.resize(columns);
        const auto next = [&] {
          const auto end = markdown.find('\n', pos);
          const std::string_view row = markdown.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
          pos = end == std::string_view::npos ? markdown.size() + 1 : end + 1;
          return row;
        };
        next();
        while (pos <= markdown.size()) {
          const std::size_t at = pos;
          const std::string_view row = next();
          if (row.find('|') == std::string_view::npos || trim(row).empty()) {
            pos = at;
            break;
          }
          table.rows.push_back(tableCells(row));
          table.rows.back().resize(columns);
        }
        blocks.push_back(std::move(table));
        continue;
      }
      if (const std::size_t start = itemStart(line); start != std::string_view::npos) {
        endParagraph();
        const bool numbered = line[0] != '-' && line[0] != '*';
        const std::string marker = numbered ? std::string(line.substr(0, start - 1)) : std::string("•");
        blocks.push_back({Block::Kind::Item, marker + "  " + inlineMarkup(line.substr(start))});
        continue;
      }
      if (!paragraph.empty()) {
        paragraph += ' ';
      }
      paragraph += line;
    }
    if (inCode) {
      blocks.push_back({Block::Kind::Code, code});
    }
    endParagraph();
    return blocks;
  }

} // namespace term::agent
