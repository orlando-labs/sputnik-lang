#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/binder/binder.h"
#include "frontend/checker/checker.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "notebook/notebook.h"
#include "runtime/module_loader.h"
#include "runtime/vm.h"
#include "tools/iamber/dispatch.h"
#include "tools/iamber/project_session.h"
#include "tools/iamber/session.h"
#include "tools/iamber/tabs.h"
#include "tools/iamber/terminal_wait.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <clocale>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <curses.h>
#include <unistd.h>

namespace {

constexpr short kBorderEditColor = 1;
constexpr short kBorderErrorColor = 2;
constexpr short kBorderRunningColor = 3;
constexpr short kFooterKeyColor = 4;
constexpr short kFooterLabelColor = 5;
constexpr short kFooterStatusColor = 6;
constexpr short kLineNumberColor = 7;
constexpr short kErrorHighlightColor = 8;
constexpr short kSyntaxKeywordColor = 9;
constexpr short kSyntaxLiteralColor = 10;
constexpr short kSyntaxStringColor = 11;
constexpr short kSyntaxOperatorColor = 12;
constexpr short kSyntaxCommentColor = 13;
constexpr short kSyntaxSpecialColor = 14;

enum class SyntaxStyle {
  Plain,
  Keyword,
  Literal,
  String,
  Operator,
  Comment,
  Special,
};

struct SyntaxSpan {
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  SyntaxStyle style = SyntaxStyle::Plain;
};

std::string read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open input file: " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

amber::lexer::LexResult lex_source(const std::string &source,
                                   const std::string &path) {
  amber::lexer::Lexer lexer(source, path);
  return lexer.lex();
}

bool is_keyword_token(amber::lexer::TokenKind kind) {
  return static_cast<int>(kind) >=
             static_cast<int>(amber::lexer::TokenKind::KeywordAnd) &&
         static_cast<int>(kind) <=
             static_cast<int>(amber::lexer::TokenKind::KeywordWhile);
}

bool is_literal_token(amber::lexer::TokenKind kind) {
  switch (kind) {
  case amber::lexer::TokenKind::Integer:
  case amber::lexer::TokenKind::Float:
  case amber::lexer::TokenKind::KeywordFalse:
  case amber::lexer::TokenKind::KeywordNull:
  case amber::lexer::TokenKind::KeywordTrue:
    return true;
  default:
    return false;
  }
}

SyntaxStyle syntax_style_for_token(amber::lexer::TokenKind kind) {
  switch (kind) {
  case amber::lexer::TokenKind::Eof:
  case amber::lexer::TokenKind::Newline:
  case amber::lexer::TokenKind::Indent:
  case amber::lexer::TokenKind::Dedent:
  case amber::lexer::TokenKind::Identifier:
    return SyntaxStyle::Plain;
  default:
    break;
  }

  if (kind == amber::lexer::TokenKind::String) {
    return SyntaxStyle::String;
  }
  if (kind == amber::lexer::TokenKind::Placeholder ||
      kind == amber::lexer::TokenKind::LastValue) {
    return SyntaxStyle::Special;
  }
  if (is_literal_token(kind)) {
    return SyntaxStyle::Literal;
  }
  if (is_keyword_token(kind)) {
    return SyntaxStyle::Keyword;
  }
  return SyntaxStyle::Operator;
}

bool comment_starts_at_for_highlight(const std::string &source,
                                     std::size_t offset) {
  if (offset >= source.size() || source[offset] != '#') {
    return false;
  }
  if (offset == 0U) {
    return true;
  }

  std::size_t line_start = offset;
  while (line_start > 0U && source[line_start - 1U] != '\n' &&
         source[line_start - 1U] != '\r') {
    --line_start;
  }

  bool first_non_space = true;
  for (std::size_t i = line_start; i < offset; ++i) {
    if (source[i] != ' ' && source[i] != '\t') {
      first_non_space = false;
      break;
    }
  }
  if (first_non_space) {
    return true;
  }

  const char previous = source[offset - 1U];
  return previous == ' ' || previous == '\t';
}

void append_comment_syntax_spans(std::vector<SyntaxSpan> *spans,
                                 const std::string &source,
                                 std::size_t start_offset,
                                 std::size_t end_offset) {
  if (spans == nullptr || start_offset >= source.size()) {
    return;
  }
  end_offset = std::min(end_offset, source.size());
  std::size_t cursor = start_offset;
  while (cursor < end_offset) {
    if (comment_starts_at_for_highlight(source, cursor)) {
      std::size_t comment_end = cursor;
      while (comment_end < end_offset && source[comment_end] != '\n' &&
             source[comment_end] != '\r') {
        ++comment_end;
      }
      spans->push_back(SyntaxSpan{cursor, comment_end, SyntaxStyle::Comment});
      cursor = comment_end;
      continue;
    }
    ++cursor;
  }
}

std::vector<SyntaxSpan> syntax_spans_for_source(const std::string &source) {
  std::vector<SyntaxSpan> spans;
  amber::lexer::LexResult result = lex_source(source, "<iamber-highlight>");
  std::size_t previous_end = 0;
  for (const amber::lexer::Token &token : result.tokens) {
    const std::size_t start = std::min(token.span.start.offset, source.size());
    const std::size_t end = std::min(token.span.end.offset, source.size());
    if (start > previous_end) {
      append_comment_syntax_spans(&spans, source, previous_end, start);
    }
    const SyntaxStyle style = syntax_style_for_token(token.kind);
    if (style != SyntaxStyle::Plain && start < end) {
      spans.push_back(SyntaxSpan{start, end, style});
    }
    previous_end = std::max(previous_end, end);
  }
  if (previous_end < source.size()) {
    append_comment_syntax_spans(&spans, source, previous_end, source.size());
  }
  return spans;
}

void draw(Session *session, bool edit_mode);

void mark_cell_running(Session *session, std::size_t index, bool edit_mode) {
  if (session == nullptr || index >= session->cells.size()) {
    return;
  }
  session->cells[index].running = true;
  session->status = "cell running";
  draw(session, edit_mode);
}

// Keep the interactive redraw hook in the curses frontend while the actual
// graph evaluation lives in the UI-free session adapter.
void evaluate_from_ui(Session *session, std::size_t start, bool force_all,
                      bool edit_mode = false, bool show_running = true) {
  evaluate_from_with_progress(
      session, start, force_all, show_running,
      [edit_mode](Session *running_session, std::size_t index) {
        mark_cell_running(running_session, index, edit_mode);
      });
}

RuntimeEventPumpResult pump_runtime_events_ui(Session *session,
                                              bool edit_mode) {
  return pump_runtime_events_detailed(
      session, true, [edit_mode](Session *running_session, std::size_t index) {
        mark_cell_running(running_session, index, edit_mode);
      });
}

void evaluate_cell(Session *session, std::size_t index, bool edit_mode,
                   bool show_running = true) {
  if (session == nullptr || index >= session->cells.size()) {
    return;
  }
  evaluate_from_ui(session, index, false, edit_mode, show_running);
}

bool should_evaluate_on_leave(const Session &session, std::size_t index) {
  // Composite source edits currently rebuild and initialize a whole module
  // world. Navigation must not implicitly repeat module side effects.
  if (index >= session.cells.size() || !session.bundled_modules.empty()) {
    return false;
  }
  const Cell &cell = session.cells[index];
  return session.auto_watch && cell.watch && cell.dirty && !cell.source.empty();
}

void evaluate_selected_on_leave(Session *session, bool edit_mode) {
  if (session == nullptr || session->selected >= session->cells.size()) {
    return;
  }
  if (should_evaluate_on_leave(*session, session->selected)) {
    evaluate_from_ui(session, session->selected, false, edit_mode);
  }
}

std::vector<std::string> split_lines(const std::string &text) {
  std::vector<std::string> lines;
  std::string line;
  for (char c : text) {
    if (c == '\n') {
      lines.push_back(line);
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  lines.push_back(line);
  return lines;
}

std::string first_line(std::string text) {
  const std::size_t newline = text.find('\n');
  if (newline != std::string::npos) {
    text = text.substr(0, newline);
  }
  return text;
}

std::vector<std::size_t> line_start_offsets(const std::string &text) {
  std::vector<std::size_t> offsets{0U};
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      offsets.push_back(i + 1U);
    }
  }
  return offsets;
}

std::pair<int, int> cursor_line_column(const std::string &text,
                                       std::size_t cursor) {
  int line = 0;
  int column = 0;
  cursor = std::min(cursor, text.size());
  for (std::size_t i = 0; i < cursor; ++i) {
    if (text[i] == '\n') {
      ++line;
      column = 0;
    } else {
      ++column;
    }
  }
  return {line, column};
}

std::size_t offset_for_line_column(const std::string &text, int target_line,
                                   int target_column) {
  target_line = std::max(0, target_line);
  target_column = std::max(0, target_column);
  int line = 0;
  int column = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (line == target_line && column == target_column) {
      return i;
    }
    if (text[i] == '\n') {
      if (line == target_line) {
        return i;
      }
      ++line;
      column = 0;
    } else {
      ++column;
    }
  }
  return text.size();
}

void clamp_cursor(Cell *cell) {
  if (cell != nullptr && cell->cursor > cell->source.size()) {
    cell->cursor = cell->source.size();
  }
}

void mark_edited(Session *session) {
  if (session == nullptr || session->selected >= session->cells.size()) {
    return;
  }
  session->cells[session->selected].dirty = true;
  session->document_dirty = true;
  for (std::size_t i = session->selected; i < session->cells.size(); ++i) {
    session->cells[i].error_ranges.clear();
    session->cells[i].errors.clear();
    session->cells[i].selected_error = 0;
  }
  session->status = "cell edited";
}

void insert_text(Session *session, const std::string &text) {
  Cell &cell = session->cells[session->selected];
  clamp_cursor(&cell);
  cell.source.insert(cell.cursor, text);
  cell.cursor += text.size();
  session->preferred_column =
      cursor_line_column(cell.source, cell.cursor).second;
  mark_edited(session);
}

void insert_char(Session *session, char c) { insert_text(session, {c}); }

std::string current_line_before_cursor(const Cell &cell) {
  const std::size_t cursor = std::min(cell.cursor, cell.source.size());
  if (cursor == 0U) {
    return "";
  }
  const std::size_t newline = cell.source.rfind('\n', cursor - 1U);
  const std::size_t line_start =
      newline == std::string::npos ? 0U : newline + 1U;
  return cell.source.substr(line_start, cursor - line_start);
}

std::string leading_indent(const std::string &line) {
  std::size_t count = 0;
  while (count < line.size() && (line[count] == ' ' || line[count] == '\t')) {
    ++count;
  }
  return line.substr(0, count);
}

std::string rstrip_copy(std::string value) {
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

void insert_auto_newline(Session *session) {
  const Cell &cell = session->cells[session->selected];
  const std::string line = current_line_before_cursor(cell);
  std::string indent = leading_indent(line);
  const std::string trimmed_right = rstrip_copy(line);
  if (!trimmed_right.empty() && trimmed_right.back() == ':') {
    indent += "  ";
  }
  insert_text(session, "\n" + indent);
}

void erase_before_cursor(Session *session) {
  Cell &cell = session->cells[session->selected];
  clamp_cursor(&cell);
  if (cell.cursor == 0U) {
    return;
  }
  cell.source.erase(cell.cursor - 1U, 1U);
  --cell.cursor;
  session->preferred_column =
      cursor_line_column(cell.source, cell.cursor).second;
  mark_edited(session);
}

void erase_at_cursor(Session *session) {
  Cell &cell = session->cells[session->selected];
  clamp_cursor(&cell);
  if (cell.cursor >= cell.source.size()) {
    return;
  }
  cell.source.erase(cell.cursor, 1U);
  mark_edited(session);
}

void move_cursor_horizontal(Session *session, int delta) {
  Cell &cell = session->cells[session->selected];
  clamp_cursor(&cell);
  if (delta < 0 && cell.cursor > 0U) {
    --cell.cursor;
  } else if (delta > 0 && cell.cursor < cell.source.size()) {
    ++cell.cursor;
  }
  session->preferred_column =
      cursor_line_column(cell.source, cell.cursor).second;
}

void move_cursor_vertical(Session *session, int delta) {
  Cell &cell = session->cells[session->selected];
  const auto [line, column] = cursor_line_column(cell.source, cell.cursor);
  if (session->preferred_column < 0) {
    session->preferred_column = column;
  }
  const std::vector<std::string> lines = split_lines(cell.source);
  const int target_line =
      std::max(0, std::min(static_cast<int>(lines.size()) - 1, line + delta));
  const int target_column = std::min(
      session->preferred_column, static_cast<int>(lines[target_line].size()));
  cell.cursor = offset_for_line_column(cell.source, target_line, target_column);
}

void select_cell(Session *session, int delta) {
  if (session == nullptr || session->cells.empty()) {
    return;
  }
  const int selected = static_cast<int>(session->selected);
  const int next =
      std::max(0, std::min(static_cast<int>(session->cells.size()) - 1,
                           selected + delta));
  session->selected = static_cast<std::size_t>(next);
  clamp_cursor(&session->cells[session->selected]);
  session->preferred_column =
      cursor_line_column(session->cells[session->selected].source,
                         session->cells[session->selected].cursor)
          .second;
  session->editor_scroll = 0;
}

bool selection_would_change(const Session *session, int delta) {
  if (session == nullptr || session->cells.empty()) {
    return false;
  }
  const int selected = static_cast<int>(session->selected);
  const int next =
      std::max(0, std::min(static_cast<int>(session->cells.size()) - 1,
                           selected + delta));
  return next != selected;
}

void select_error(Session *session, int delta) {
  if (!selected_cell_has_errors(session)) {
    return;
  }
  Cell &cell = session->cells[session->selected];
  clamp_selected_error(&cell);
  const int current = static_cast<int>(cell.selected_error);
  const int next = std::max(
      0, std::min(static_cast<int>(cell.errors.size()) - 1, current + delta));
  cell.selected_error = static_cast<std::size_t>(next);
  focus_selected_error(session);
  std::ostringstream status;
  status << "error " << (cell.selected_error + 1U) << "/" << cell.errors.size();
  session->status = status.str();
}

void new_cell(Session *session) {
  if (session != nullptr && session->module_editor) {
    session->status = "module tab has one full-source buffer";
    return;
  }
  Cell cell;
  cell.id = amber::notebook::allocate_cell_id();
  const std::size_t insert_at =
      session->cells.empty() ? 0U : session->selected + 1U;
  session->cells.insert(session->cells.begin() + insert_at, std::move(cell));
  session->selected = insert_at;
  session->editor_scroll = 0;
  session->preferred_column = 0;
  clear_error_ranges_until(session, session->cells.size() - 1U);
  session->status = "new cell";
  session->document_dirty = true;
}

void delete_cell(Session *session) {
  if (session == nullptr || session->cells.empty()) {
    return;
  }
  if (session->module_editor) {
    session->status = "module source buffer cannot be deleted";
    return;
  }
  if (session->cells[session->selected].kind != "code") {
    session->status = "non-code cells are preserved read-only";
    return;
  }
  session->cells.erase(session->cells.begin() + session->selected);
  if (session->cells.empty()) {
    Cell replacement;
    replacement.id = amber::notebook::allocate_cell_id();
    session->cells.push_back(std::move(replacement));
  }
  if (session->selected >= session->cells.size()) {
    session->selected = session->cells.size() - 1U;
  }
  clear_error_ranges_until(session, session->cells.size() - 1U);
  session->status = "cell deleted";
  session->document_dirty = true;
}

void print_clipped(WINDOW *window, int y, int x, int width,
                   const std::string &text) {
  if (width <= 0) {
    return;
  }
  std::string clipped = text;
  if (static_cast<int>(clipped.size()) > width) {
    clipped = clipped.substr(0, static_cast<std::size_t>(width));
  }
  mvwaddnstr(window, y, x, clipped.c_str(), width);
}

void append_wrapped_line(std::vector<std::string> *lines,
                         const std::string &text, int width) {
  if (lines == nullptr || width <= 0) {
    return;
  }
  if (text.empty()) {
    lines->push_back("");
    return;
  }

  std::size_t offset = 0;
  const std::size_t wrap_width = static_cast<std::size_t>(width);
  while (offset < text.size()) {
    const std::size_t remaining = text.size() - offset;
    if (remaining <= wrap_width) {
      lines->push_back(text.substr(offset));
      return;
    }

    std::size_t next = text.rfind(' ', offset + wrap_width);
    if (next == std::string::npos || next < offset) {
      next = offset + wrap_width;
    }
    lines->push_back(text.substr(offset, next - offset));
    offset = next;
    while (offset < text.size() && text[offset] == ' ') {
      ++offset;
    }
  }
}

std::vector<std::string> wrap_text(const std::string &text, int width) {
  std::vector<std::string> wrapped;
  if (width <= 0) {
    return wrapped;
  }
  for (const std::string &line : split_lines(text)) {
    append_wrapped_line(&wrapped, line, width);
  }
  if (wrapped.empty()) {
    wrapped.push_back("");
  }
  return wrapped;
}

std::vector<std::string> output_detail_lines(
    const std::vector<amber::runtime::RuntimeTextOutputEvent> &events,
    int width) {
  std::vector<std::string> lines;
  std::string current_stream;
  std::string current_text;
  auto flush_current = [&]() {
    if (current_text.empty()) {
      return;
    }
    const std::string prefix = current_stream == "stderr" ? "stderr: " : "";
    std::vector<std::string> raw_lines = split_lines(current_text);
    if (!raw_lines.empty() && raw_lines.back().empty()) {
      raw_lines.pop_back();
    }
    for (const std::string &line : raw_lines) {
      const std::vector<std::string> wrapped = wrap_text(prefix + line, width);
      lines.insert(lines.end(), wrapped.begin(), wrapped.end());
    }
    current_text.clear();
  };

  for (const amber::runtime::RuntimeTextOutputEvent &event : events) {
    if (!current_text.empty() && event.stream != current_stream) {
      flush_current();
    }
    current_stream = event.stream;
    current_text += event.text;
  }
  flush_current();
  return lines;
}

int visible_cell_count(const Session &session, int body_height) {
  if (session.cells.empty() || body_height <= 0) {
    return 0;
  }
  const int min_pane_height = 8;
  return std::max(1, std::min(static_cast<int>(session.cells.size()),
                              std::max(1, body_height / min_pane_height)));
}

void clamp_cell_scroll(Session *session, int visible_count) {
  if (session == nullptr || session->cells.empty() || visible_count <= 0) {
    return;
  }
  if (static_cast<int>(session->selected) < session->cell_scroll) {
    session->cell_scroll = static_cast<int>(session->selected);
  }
  if (static_cast<int>(session->selected) >=
      session->cell_scroll + visible_count) {
    session->cell_scroll =
        static_cast<int>(session->selected) - visible_count + 1;
  }
  const int max_scroll =
      std::max(0, static_cast<int>(session->cells.size()) - visible_count);
  session->cell_scroll =
      std::max(0, std::min(session->cell_scroll, max_scroll));
}

std::string cell_status(const Cell &cell) {
  if (cell.kind != "code")
    return "read-only:" + cell.kind;
  if (cell.running) {
    return "running";
  }
  if (cell.dirty) {
    return "dirty";
  }
  return cell.ok ? "ok" : "error";
}

bool cell_has_error(const Cell &cell) {
  return !cell.running && !cell.dirty &&
         ((!cell.ok && !cell.error.empty()) || !cell.errors.empty());
}

int border_attr_for(const Cell &cell, bool selected, bool edit_mode) {
  if (cell.running) {
    return COLOR_PAIR(kBorderRunningColor) | A_BOLD;
  }
  if (cell_has_error(cell)) {
    return COLOR_PAIR(kBorderErrorColor) | A_BOLD;
  }
  if (selected && edit_mode) {
    return COLOR_PAIR(kBorderEditColor) | A_BOLD;
  }
  return selected ? A_BOLD : A_NORMAL;
}

std::vector<std::pair<std::string, std::string>>
footer_actions(bool edit_mode) {
  if (edit_mode) {
    return {{"Esc", "Nav"}, {"C-S", "Save"}, {"C-X", "Run"},  {"C-R", "All"},
            {"F2", "New"},  {"Arw", "Move"}, {"Bksp", "Del"}, {"F10", "Quit"}};
  }
  return {{"C-S", "Save"}, {"F2", "New"},  {"Ent", "Run"}, {"R", "All"},
          {"E", "Edit"},   {"L/R", "Err"}, {"D", "Del"},   {"W", "Watch"},
          {"A", "Auto"},   {"F10", "Quit"}};
}

int footer_key_attr() {
  return has_colors() ? COLOR_PAIR(kFooterKeyColor) | A_BOLD
                      : A_REVERSE | A_BOLD;
}

int footer_label_attr() {
  return has_colors() ? COLOR_PAIR(kFooterLabelColor) : A_REVERSE;
}

int footer_status_attr() {
  return has_colors() ? COLOR_PAIR(kFooterStatusColor) : A_REVERSE;
}

int line_number_attr() {
  return has_colors() ? COLOR_PAIR(kLineNumberColor) | A_DIM : A_DIM;
}

int error_highlight_attr() {
  return has_colors() ? COLOR_PAIR(kErrorHighlightColor) | A_BOLD
                      : A_REVERSE | A_BOLD;
}

int error_frame_attr() {
  return has_colors() ? COLOR_PAIR(kBorderErrorColor) | A_BOLD | A_UNDERLINE
                      : A_BOLD | A_UNDERLINE;
}

int syntax_attr(SyntaxStyle style) {
  if (!has_colors()) {
    switch (style) {
    case SyntaxStyle::Keyword:
    case SyntaxStyle::Special:
      return A_BOLD;
    case SyntaxStyle::Comment:
      return A_DIM;
    default:
      return A_NORMAL;
    }
  }

  switch (style) {
  case SyntaxStyle::Keyword:
    return COLOR_PAIR(kSyntaxKeywordColor) | A_BOLD;
  case SyntaxStyle::Literal:
    return COLOR_PAIR(kSyntaxLiteralColor);
  case SyntaxStyle::String:
    return COLOR_PAIR(kSyntaxStringColor);
  case SyntaxStyle::Operator:
    return COLOR_PAIR(kSyntaxOperatorColor);
  case SyntaxStyle::Comment:
    return COLOR_PAIR(kSyntaxCommentColor) | A_DIM;
  case SyntaxStyle::Special:
    return COLOR_PAIR(kSyntaxSpecialColor) | A_BOLD;
  case SyntaxStyle::Plain:
    return A_NORMAL;
  }
  return A_NORMAL;
}

SyntaxStyle syntax_style_at_offset(const std::vector<SyntaxSpan> &spans,
                                   std::size_t offset) {
  for (const SyntaxSpan &span : spans) {
    if (offset < span.start_offset) {
      return SyntaxStyle::Plain;
    }
    if (offset >= span.start_offset && offset < span.end_offset) {
      return span.style;
    }
  }
  return SyntaxStyle::Plain;
}

std::pair<int, int> highlighted_columns_for_line(const CodeErrorRange &range,
                                                 int line_index,
                                                 int line_length) {
  if (line_index < range.start_line || line_index > range.end_line) {
    return {0, 0};
  }
  if (range.whole_line) {
    return {0, std::max(1, line_length)};
  }

  const int start =
      line_index == range.start_line ? std::max(0, range.start_column) : 0;
  int end = line_index == range.end_line ? range.end_column : line_length;
  if (line_index != range.end_line) {
    end = std::max(1, end);
  }
  end = std::max(start + 1, end);
  return {start, end};
}

enum class ErrorColumnStyle {
  None,
  Framed,
  Selected,
};

ErrorColumnStyle error_column_style_for(const Cell &cell, bool selected_cell,
                                        int line_index, int column,
                                        int line_length) {
  bool framed = false;
  for (std::size_t i = 0; i < cell.errors.size(); ++i) {
    const CellErrorView &error = cell.errors[i];
    if (!error.has_range) {
      continue;
    }
    const auto [start, end] =
        highlighted_columns_for_line(error.range, line_index, line_length);
    if (column >= start && column < end) {
      if (selected_cell && i == cell.selected_error) {
        return ErrorColumnStyle::Selected;
      }
      framed = true;
    }
  }
  return framed ? ErrorColumnStyle::Framed : ErrorColumnStyle::None;
}

void draw_code_text_line(WINDOW *window, int y, int x, int width,
                         const std::string &text, const Cell &cell,
                         bool selected_cell, int line_index,
                         std::size_t line_start_offset,
                         const std::vector<SyntaxSpan> &syntax_spans) {
  if (width <= 0) {
    return;
  }
  const int line_length = static_cast<int>(text.size());
  int draw_limit = line_length;
  for (const CellErrorView &error : cell.errors) {
    if (!error.has_range) {
      continue;
    }
    const auto [start, end] =
        highlighted_columns_for_line(error.range, line_index, line_length);
    if (end > start) {
      draw_limit = std::max(draw_limit, end);
    }
  }
  draw_limit = std::min(width, draw_limit);

  for (int column = 0; column < draw_limit; ++column) {
    const ErrorColumnStyle style = error_column_style_for(
        cell, selected_cell, line_index, column, line_length);
    if (style == ErrorColumnStyle::Selected) {
      wattron(window, error_highlight_attr());
    } else if (style == ErrorColumnStyle::Framed) {
      wattron(window, error_frame_attr());
    } else if (column < line_length) {
      const SyntaxStyle syntax_style = syntax_style_at_offset(
          syntax_spans, line_start_offset + static_cast<std::size_t>(column));
      if (syntax_style != SyntaxStyle::Plain) {
        wattron(window, syntax_attr(syntax_style));
      }
    }
    const char ch =
        column < line_length ? text[static_cast<std::size_t>(column)] : ' ';
    mvwaddch(window, y, x + column, ch);
    if (style == ErrorColumnStyle::Selected) {
      wattroff(window, error_highlight_attr());
    } else if (style == ErrorColumnStyle::Framed) {
      wattroff(window, error_frame_attr());
    } else if (column < line_length) {
      const SyntaxStyle syntax_style = syntax_style_at_offset(
          syntax_spans, line_start_offset + static_cast<std::size_t>(column));
      if (syntax_style != SyntaxStyle::Plain) {
        wattroff(window, syntax_attr(syntax_style));
      }
    }
  }
}

void draw_footer_segment(int y, int *x, int cols, const std::string &key,
                         const std::string &label) {
  if (x == nullptr || *x >= cols) {
    return;
  }
  const int needed = static_cast<int>(key.size() + label.size() + 2U);
  if (*x + needed > cols) {
    return;
  }

  attron(footer_key_attr());
  mvaddnstr(y, *x, key.c_str(), cols - *x);
  attroff(footer_key_attr());
  *x += static_cast<int>(key.size());

  attron(footer_label_attr());
  mvaddch(y, *x, ' ');
  ++(*x);
  mvaddnstr(y, *x, label.c_str(), cols - *x);
  attroff(footer_label_attr());
  *x += static_cast<int>(label.size());

  if (*x < cols) {
    mvaddch(y, *x, ' ');
    ++(*x);
  }
}

void draw_footer(const Session &session, bool edit_mode, int rows, int cols) {
  const int status_y = rows - 2;
  const int actions_y = rows - 1;

  std::ostringstream status;
  status << " iamber";
  if (!session.project_label.empty()) {
    status << " " << session.project_label
           << (session.document_dirty ? "*" : "");
  }
  status << "  mode:" << (edit_mode ? "edit" : "nav")
         << "  auto-watch:" << (session.auto_watch ? "on" : "off") << "  "
         << session.status;

  attron(footer_status_attr());
  for (int x = 0; x < cols; ++x) {
    mvaddch(status_y, x, ' ');
  }
  mvaddnstr(status_y, 0, status.str().c_str(), cols);
  attroff(footer_status_attr());

  for (int x = 0; x < cols; ++x) {
    mvaddch(actions_y, x, ' ');
  }
  int x = 0;
  if (!session.tab_bar.empty())
    draw_footer_segment(actions_y, &x, cols, "F7/F8", "Tabs");
  if (!session.project_sheet_id.empty() && !session.module_editor)
    draw_footer_segment(actions_y, &x, cols, "F6", "Apply");
  for (const auto &action : footer_actions(edit_mode)) {
    draw_footer_segment(actions_y, &x, cols, action.first, action.second);
  }
}

void draw_double_border(WINDOW *window, int height, int width) {
  if (height <= 1 || width <= 1) {
    return;
  }
  mvwaddstr(window, 0, 0, "╔");
  mvwaddstr(window, 0, width - 1, "╗");
  mvwaddstr(window, height - 1, 0, "╚");
  mvwaddstr(window, height - 1, width - 1, "╝");
  for (int x = 1; x < width - 1; ++x) {
    mvwaddstr(window, 0, x, "═");
    mvwaddstr(window, height - 1, x, "═");
  }
  for (int y = 1; y < height - 1; ++y) {
    mvwaddstr(window, y, 0, "║");
    mvwaddstr(window, y, width - 1, "║");
  }
}

void draw_single_border(WINDOW *window, int height, int width) {
  if (height <= 1 || width <= 1) {
    return;
  }
  mvwaddstr(window, 0, 0, "┌");
  mvwaddstr(window, 0, width - 1, "┐");
  mvwaddstr(window, height - 1, 0, "└");
  mvwaddstr(window, height - 1, width - 1, "┘");
  for (int x = 1; x < width - 1; ++x) {
    mvwaddstr(window, 0, x, "─");
    mvwaddstr(window, height - 1, x, "─");
  }
  for (int y = 1; y < height - 1; ++y) {
    mvwaddstr(window, y, 0, "│");
    mvwaddstr(window, y, width - 1, "│");
  }
}

void draw_pane_border(WINDOW *window, int height, int width, bool selected) {
  if (selected) {
    draw_double_border(window, height, width);
  } else {
    draw_single_border(window, height, width);
  }
}

void draw_split_border(WINDOW *window, int height, int split_x,
                       bool outer_double) {
  if (split_x <= 0) {
    return;
  }
  for (int y = 1; y < height - 1; ++y) {
    mvwaddstr(window, y, split_x, "│");
  }
  if (outer_double) {
    mvwaddstr(window, 0, split_x, "╤");
    mvwaddstr(window, height - 1, split_x, "╧");
  } else {
    mvwaddstr(window, 0, split_x, "┬");
    mvwaddstr(window, height - 1, split_x, "┴");
  }
}

std::string selected_error_message(const Cell &cell) {
  if (cell.errors.empty()) {
    return "";
  }
  const std::size_t index =
      std::min(cell.selected_error, cell.errors.size() - 1U);
  return cell.errors[index].message;
}

std::string cell_result_line(const Cell &cell) {
  return cell.ok ? "=> " + cell.result
                 : (cell.error.empty() ? "=> not evaluated"
                                       : "! " + first_line(cell.error));
}

std::vector<std::string> cell_detail_lines(const Cell &cell, int width) {
  if (!cell.errors.empty()) {
    return wrap_text("! " + selected_error_message(cell), width);
  }
  std::vector<std::string> lines =
      output_detail_lines(cell.output_events, width);
  lines.push_back(cell_result_line(cell));
  return lines;
}

int cell_detail_row_count(const Cell &cell, int height, int width) {
  const int content_rows = std::max(0, height - 2);
  if (content_rows == 0 || width <= 0) {
    return 0;
  }
  const std::vector<std::string> detail_lines = cell_detail_lines(cell, width);
  return std::min(content_rows, static_cast<int>(detail_lines.size()));
}

void draw_cell_code(WINDOW *window, Session *session, std::size_t index,
                    bool edit_mode, int height, int code_width) {
  Cell &cell = session->cells[index];
  const bool selected = index == session->selected;
  const std::vector<std::string> lines = split_lines(cell.source);
  const std::vector<std::size_t> line_offsets = line_start_offsets(cell.source);
  const std::vector<SyntaxSpan> syntax_spans =
      syntax_spans_for_source(cell.source);
  const int text_width = code_width - 2;
  const int detail_rows = cell_detail_row_count(cell, height, text_width);
  const int detail_start = height - 1 - detail_rows;
  const int code_rows = std::max(0, detail_start - 1);
  int code_scroll = 0;

  const auto [cursor_line, cursor_column] =
      cursor_line_column(cell.source, cell.cursor);
  if (selected) {
    if (code_rows > 0) {
      if (cursor_line < session->editor_scroll) {
        session->editor_scroll = cursor_line;
      }
      if (cursor_line >= session->editor_scroll + code_rows) {
        session->editor_scroll = cursor_line - code_rows + 1;
      }
      session->editor_scroll = std::max(0, session->editor_scroll);
      code_scroll = session->editor_scroll;
    } else {
      session->editor_scroll = std::max(0, cursor_line);
    }
  }

  for (int row = 0; row < code_rows; ++row) {
    const int line_index = code_scroll + row;
    if (line_index >= static_cast<int>(lines.size())) {
      break;
    }
    std::ostringstream prefix;
    prefix.width(4);
    prefix << (line_index + 1);
    prefix << " ";
    wattron(window, line_number_attr());
    mvwaddnstr(window, row + 1, 1, prefix.str().c_str(), code_width - 2);
    wattroff(window, line_number_attr());
    const std::size_t line_start =
        line_index < static_cast<int>(line_offsets.size())
            ? line_offsets[static_cast<std::size_t>(line_index)]
            : cell.source.size();
    draw_code_text_line(window, row + 1, 6, code_width - 7, lines[line_index],
                        cell, selected, line_index, line_start, syntax_spans);
  }

  wattron(window, A_DIM);
  const std::vector<std::string> detail_lines =
      cell_detail_lines(cell, text_width);
  for (int row = 0; row < detail_rows; ++row) {
    const std::string &line = detail_lines[static_cast<std::size_t>(row)];
    mvwaddnstr(window, detail_start + row, 1, line.c_str(), text_width);
  }
  wattroff(window, A_DIM);

  (void)selected;
  (void)edit_mode;
  (void)cursor_column;
}

int first_visible_error_index(const Cell &cell, int visible_rows) {
  if (visible_rows <= 0 || cell.errors.empty()) {
    return 0;
  }
  const int selected =
      static_cast<int>(std::min(cell.selected_error, cell.errors.size() - 1U));
  if (selected < visible_rows) {
    return 0;
  }
  return selected - visible_rows + 1;
}

void draw_cell_errors(WINDOW *window, Cell *cell, int height, int split_x,
                      int width, bool selected) {
  const int panel_x = split_x + 1;
  const int panel_width = width - panel_x - 1;
  if (cell == nullptr || panel_width <= 0) {
    return;
  }
  clamp_selected_error(cell);

  wattron(window, A_BOLD);
  std::ostringstream title;
  title << "errors";
  if (!cell->errors.empty()) {
    title << " " << (cell->selected_error + 1U) << "/" << cell->errors.size();
  }
  print_clipped(window, 1, panel_x + 1, panel_width - 2, title.str());
  wattroff(window, A_BOLD);

  const int visible_rows = std::max(0, height - 3);
  const int first = first_visible_error_index(*cell, visible_rows);
  int row = 2;
  for (int shown = 0; shown < visible_rows; ++shown) {
    const int error_index = first + shown;
    if (error_index >= static_cast<int>(cell->errors.size())) {
      break;
    }
    const bool active = selected && static_cast<std::size_t>(error_index) ==
                                        cell->selected_error;
    std::ostringstream line;
    line << (active ? "> " : "  ") << (error_index + 1) << ". "
         << cell->errors[static_cast<std::size_t>(error_index)].message;
    if (active) {
      wattron(window, A_BOLD);
    }
    print_clipped(window, row++, panel_x + 1, panel_width - 2, line.str());
    if (active) {
      wattroff(window, A_BOLD);
    }
  }
}

void draw_cell_locals(WINDOW *window, const Cell &cell, int height, int split_x,
                      int width) {
  const int locals_x = split_x + 1;
  const int locals_width = width - locals_x - 1;
  if (locals_width <= 0) {
    return;
  }

  wattron(window, A_BOLD);
  print_clipped(window, 1, locals_x + 1, locals_width - 2, "locals");
  wattroff(window, A_BOLD);

  int row = 2;
  if (!cell.ok && !cell.error.empty()) {
    print_clipped(window, row++, locals_x + 1, locals_width - 2, "error");
  } else if (cell.locals.empty()) {
    print_clipped(window, row++, locals_x + 1, locals_width - 2, "(none)");
  }

  for (const LocalView &local : cell.locals) {
    if (row > height - 2) {
      break;
    }
    std::ostringstream line;
    line << local.name << " = " << local.value;
    print_clipped(window, row++, locals_x + 1, locals_width - 2, line.str());
    if (row > height - 2) {
      break;
    }
    std::ostringstream meta;
    meta << "  " << local.role;
    if (local.watched) {
      meta << " watch#" << local.watch_cell_id << " r" << local.watch_revision;
    }
    wattron(window, A_DIM);
    print_clipped(window, row++, locals_x + 1, locals_width - 2, meta.str());
    wattroff(window, A_DIM);
  }
}

void place_edit_cursor(WINDOW *window, Session *session, std::size_t index,
                       int height, int code_width) {
  if (index != session->selected) {
    return;
  }
  const Cell &cell = session->cells[index];
  const auto [cursor_line, cursor_column] =
      cursor_line_column(cell.source, cell.cursor);
  const int detail_rows = cell_detail_row_count(cell, height, code_width - 2);
  const int detail_start = height - 1 - detail_rows;
  const int code_rows = std::max(0, detail_start - 1);
  const int code_scroll = session->editor_scroll;
  if (code_rows <= 0) {
    return;
  }
  if (cursor_line < code_scroll || cursor_line >= code_scroll + code_rows) {
    return;
  }
  const int y = 1 + cursor_line - code_scroll;
  const int x = std::min(code_width - 2, 6 + cursor_column);
  wmove(window, y, x);
}

void draw_cell_pane(WINDOW *window, Session *session, std::size_t index,
                    bool edit_mode, int height, int width) {
  Cell &cell = session->cells[index];
  const bool selected = index == session->selected;
  const int border_attr = border_attr_for(cell, selected, edit_mode);
  wattron(window, border_attr);
  draw_pane_border(window, height, width, selected);

  const int locals_width = std::max(24, std::min(42, width / 3));
  const int split_x = std::max(18, width - locals_width - 1);
  draw_split_border(window, height, split_x, selected);

  std::ostringstream title;
  title << (session->module_editor ? " module " : " cell ");
  title << (session->module_editor ? session->project_module_id
                                   : std::to_string(index + 1U))
        << " " << cell_status(cell);
  if (!session->module_editor)
    title << " " << (cell.watch ? "watch:on" : "watch:off");
  if (cell.watch_event_count != 0U) {
    title << " events:" << cell.watch_event_count
          << " epoch:" << cell.watch_epoch;
  }
  if (selected) {
    if (cell.running) {
      title << " RUNNING";
    } else {
      title << (edit_mode ? " EDIT" : " NAV");
    }
  }
  mvwaddnstr(window, 0, 2, title.str().c_str(), std::max(0, width - 4));
  wattroff(window, border_attr);

  draw_cell_code(window, session, index, edit_mode, height, split_x);
  if (!cell.errors.empty()) {
    draw_cell_errors(window, &cell, height, split_x, width, selected);
  } else {
    draw_cell_locals(window, cell, height, split_x, width);
  }
  if (selected && edit_mode) {
    place_edit_cursor(window, session, index, height, split_x);
  }
}

void draw(Session *session, bool edit_mode) {
  int rows = 0;
  int cols = 0;
  getmaxyx(stdscr, rows, cols);
  curs_set(edit_mode ? 1 : 0);
  erase();
  if (rows < 12 || cols < 60) {
    mvaddstr(0, 0, "iamber needs at least 60x12");
    refresh();
    return;
  }

  const int tab_height = session->tab_bar.empty() ? 0 : 1;
  if (tab_height != 0) {
    attron(footer_status_attr());
    mvaddnstr(0, 0, session->tab_bar.c_str(), cols);
    attroff(footer_status_attr());
  }

  std::vector<std::string> environment_lines;
  if (!session->environment_error.empty())
    environment_lines = wrap_text("environment: " + session->environment_error,
                                   cols - 2);
  else if (!session->environment_output.empty()) {
    environment_lines = output_detail_lines(session->environment_output, cols - 2);
    environment_lines.insert(environment_lines.begin(), "environment output:");
  }
  if (session->environment_stale)
    environment_lines.insert(environment_lines.begin(),
                             "environment changed; F6 Apply this sheet");
  const int environment_height = std::min(
      static_cast<int>(environment_lines.size()), std::min(5, rows / 4));
  for (int line = 0; line < environment_height; ++line) {
    const bool clipped = line + 1 == environment_height &&
        environment_lines.size() > static_cast<std::size_t>(environment_height);
    const std::string &message = environment_lines[static_cast<std::size_t>(line)];
    mvaddnstr(tab_height + line, 1,
              clipped ? "... (environment output truncated)" : message.c_str(),
              cols - 2);
  }

  const int footer_height = 2;
  const int body_height = rows - footer_height - environment_height - tab_height;
  const int visible_count = visible_cell_count(*session, body_height);
  clamp_cell_scroll(session, visible_count);
  const int base_height =
      visible_count <= 0 ? body_height : body_height / visible_count;
  const int remainder = visible_count <= 0 ? 0 : body_height % visible_count;

  draw_footer(*session, edit_mode, rows, cols);
  wnoutrefresh(stdscr);

  std::vector<WINDOW *> panes;
  WINDOW *active_pane = nullptr;
  int y = tab_height + environment_height;
  for (int slot = 0; slot < visible_count; ++slot) {
    const std::size_t index =
        static_cast<std::size_t>(session->cell_scroll + slot);
    if (index >= session->cells.size()) {
      break;
    }
    const int pane_height = base_height + (slot < remainder ? 1 : 0);
    WINDOW *pane = newwin(pane_height, cols, y, 0);
    draw_cell_pane(pane, session, index, edit_mode, pane_height, cols);
    wnoutrefresh(pane);
    if (index == session->selected) {
      active_pane = pane;
    }
    panes.push_back(pane);
    y += pane_height;
  }
  if (active_pane != nullptr) {
    touchwin(active_pane);
    wnoutrefresh(active_pane);
  }
  doupdate();

  for (WINDOW *pane : panes) {
    delwin(pane);
  }
}

bool handle_nav_key(Session *session, int ch, bool *edit_mode) {
  switch (ch) {
  case 'q':
  case KEY_F(10):
    return false;
  case 'e':
    if (session->cells.empty())
      new_cell(session);
    if (session->cells[session->selected].kind != "code") {
      session->status = "non-code cells are preserved read-only";
      return true;
    }
    *edit_mode = true;
    curs_set(1);
    return true;
  case KEY_UP:
    if (selection_would_change(session, -1)) {
      evaluate_selected_on_leave(session, false);
    }
    select_cell(session, -1);
    return true;
  case KEY_DOWN:
    if (selection_would_change(session, 1)) {
      evaluate_selected_on_leave(session, false);
    }
    select_cell(session, 1);
    return true;
  case KEY_LEFT:
    select_error(session, -1);
    return true;
  case KEY_RIGHT:
    select_error(session, 1);
    return true;
  case '\n':
  case '\r':
    evaluate_cell(session, session->selected, false);
    return true;
  case 'r':
    evaluate_from_ui(session, 0, true, false);
    return true;
  case 'n':
  case KEY_F(2):
    evaluate_selected_on_leave(session, false);
    new_cell(session);
    return true;
  case 'd':
    delete_cell(session);
    return true;
  case 'w':
    if (session->module_editor) {
      session->status = "module tabs are run explicitly, never watched";
      return true;
    }
    if (session->cells.empty() ||
        session->cells[session->selected].kind != "code") {
      session->status = "watch mode requires a code cell";
      return true;
    }
    session->cells[session->selected].watch =
        !session->cells[session->selected].watch;
    session->document_dirty = true;
    session->status = session->cells[session->selected].watch
                          ? "object.watch enabled"
                          : "object.watch disabled";
    return true;
  case 'a':
    session->auto_watch = !session->auto_watch;
    session->status =
        session->auto_watch ? "auto-watch enabled" : "auto-watch disabled";
    return true;
  default:
    return true;
  }
}

bool handle_edit_key(Session *session, int ch, bool *edit_mode) {
  switch (ch) {
  case KEY_F(10):
    return false;
  case 27:
    evaluate_selected_on_leave(session, true);
    *edit_mode = false;
    curs_set(0);
    return true;
  case KEY_F(2):
  case 14:
    evaluate_selected_on_leave(session, true);
    new_cell(session);
    return true;
  case 24:
    evaluate_from_ui(session, session->selected, false, true);
    *edit_mode = false;
    curs_set(0);
    return true;
  case 18:
    evaluate_from_ui(session, 0, true, true);
    return true;
  case KEY_LEFT:
    move_cursor_horizontal(session, -1);
    return true;
  case KEY_RIGHT:
    move_cursor_horizontal(session, 1);
    return true;
  case KEY_UP:
    move_cursor_vertical(session, -1);
    return true;
  case KEY_DOWN:
    move_cursor_vertical(session, 1);
    return true;
  case KEY_HOME:
  case 1: {
    Cell &cell = session->cells[session->selected];
    const auto [line, column] = cursor_line_column(cell.source, cell.cursor);
    (void)column;
    cell.cursor = offset_for_line_column(cell.source, line, 0);
    session->preferred_column = 0;
    return true;
  }
  case KEY_END:
  case 5: {
    Cell &cell = session->cells[session->selected];
    const auto [line, column] = cursor_line_column(cell.source, cell.cursor);
    (void)column;
    const std::vector<std::string> lines = split_lines(cell.source);
    cell.cursor = offset_for_line_column(cell.source, line,
                                         static_cast<int>(lines[line].size()));
    session->preferred_column = static_cast<int>(lines[line].size());
    return true;
  }
  case KEY_BACKSPACE:
  case 127:
  case 8:
    erase_before_cursor(session);
    return true;
  case KEY_DC:
    erase_at_cursor(session);
    return true;
  case '\n':
  case '\r':
    insert_auto_newline(session);
    return true;
  default:
    if ((ch >= 32 && ch < 127) || (ch >= 128 && ch <= 255)) {
      insert_char(session, static_cast<char>(ch));
    }
    return true;
  }
}

int run_curses_console(
    std::optional<amber::notebook::LoadedProject> project = {},
    const std::string &sheet_id = {},
    std::optional<amber::notebook::LoadedProjectModule> project_module = {},
    const std::vector<amber::runtime::RuntimeCapabilityGrant> &grants = {}) {
  if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
    throw std::runtime_error(
        "interactive mode requires a terminal; use --eval or --project-info");
  }
  // Establish signal inheritance before the first runtime worker exists.
  // Destroy this guard after the screen and all Sessions/backends, here.
  TerminalEventWaiter terminal_wait;
  std::unique_ptr<ProjectTabs> workspace;
  std::unique_ptr<ProjectTab> scratch;
  std::vector<ProjectTab *> tabs;
  if (project) {
    workspace = std::make_unique<ProjectTabs>(
        std::move(*project), sheet_id, project_module ? project_module->id : "");
    for (const auto &tab : workspace->tabs())
      tabs.push_back(tab.get());
  } else {
    scratch = std::make_unique<ProjectTab>();
    Cell initial;
    initial.id = amber::notebook::allocate_cell_id();
    scratch->session.cells.push_back(std::move(initial));
    scratch->activity = scratch->session.activity_waiter();
    tabs.push_back(scratch.get());
  }
  const auto current_tab = [&]() -> ProjectTab & {
    return workspace ? workspace->active() : *scratch;
  };
  for (auto *tab : tabs) tab->session.runtime_capability_grants = grants;
  // One stable descriptor wakes the owner for any tab. Local mailboxes retain
  // the identity of pending work; neither switching nor a hint executes code.
  const auto activity = workspace ? workspace->activity_waiter()
                                  : scratch->activity;
  const int activity_descriptor = activity.poll_descriptor();
  if (activity_descriptor < 0) {
    throw std::runtime_error("terminal activity descriptors are unavailable");
  }
  // Validate fd_set bounds before taking over the user's terminal.
  (void)terminal_wait.wait(STDIN_FILENO, activity_descriptor,
                           std::chrono::milliseconds(0));

  setlocale(LC_ALL, "");
  initscr();
  struct CursesScope {
    ~CursesScope() { endwin(); }
  } screen;
  raw();
  noecho();
  keypad(stdscr, TRUE);
  meta(stdscr, TRUE);
  timeout(0);
  notimeout(stdscr, FALSE);
  curs_set(0);
  if (has_colors()) {
    start_color();
    use_default_colors();
    init_pair(kBorderEditColor, COLOR_CYAN, -1);
    init_pair(kBorderErrorColor, COLOR_RED, -1);
    init_pair(kBorderRunningColor, COLOR_YELLOW, -1);
    init_pair(kFooterKeyColor, COLOR_BLACK, COLOR_CYAN);
    init_pair(kFooterLabelColor, COLOR_BLACK, COLOR_GREEN);
    init_pair(kFooterStatusColor, COLOR_BLACK, COLOR_WHITE);
    init_pair(kLineNumberColor, COLOR_WHITE, -1);
    init_pair(kErrorHighlightColor, COLOR_WHITE, COLOR_RED);
    init_pair(kSyntaxKeywordColor, COLOR_CYAN, -1);
    init_pair(kSyntaxLiteralColor, COLOR_YELLOW, -1);
    init_pair(kSyntaxStringColor, COLOR_GREEN, -1);
    init_pair(kSyntaxOperatorColor, COLOR_MAGENTA, -1);
    init_pair(kSyntaxCommentColor, COLOR_WHITE, -1);
    init_pair(kSyntaxSpecialColor, COLOR_BLUE, -1);
  }

  bool running = true;
  bool redraw = true;
  bool confirm_discard = false;
  std::size_t runtime_cursor = 0;
  while (running) {
    // Deliver a queued SIGWINCH even under continuous keyboard input; curses
    // remains the only parser/resize handler, and every call stays here.
    terminal_wait.dispatch_pending_signals();
    if (redraw) {
      auto &tab = current_tab();
      tab.session.tab_bar = workspace ? workspace->tab_bar() : "";
      draw(&tab.session, tab.edit_mode);
      redraw = false;
    }
    const int ch = getch();
    if (ch != ERR) {
      try {
        auto &tab = current_tab();
        auto &session = tab.session;
        const bool discard = confirm_discard && ch == 'Q';
        confirm_discard = false;
        if (discard) {
          running = false;
        } else if (workspace && (ch == KEY_F(7) || ch == KEY_F(8))) {
          workspace->switch_by(ch == KEY_F(7) ? -1 : 1);
        } else if (ch == KEY_F(6) && workspace) {
          workspace->apply_active(
              false, true,
              [edit_mode = tab.edit_mode](Session *running_session,
                                          std::size_t index) {
                mark_cell_running(running_session, index, edit_mode);
              });
        } else if (ch == 19 || ch == KEY_F(4)) {
          if (workspace)
            workspace->save_active();
          else
            session.status =
                "no project; create with --new-project, open with --project";
        } else if (workspace &&
                   (ch == KEY_F(10) || (!tab.edit_mode && ch == 'q')) &&
                   workspace->modified_count() != 0U) {
          session.status =
              "unsaved changes: C-S save tab, Q discard all & quit, other key cancels";
          confirm_discard = true;
        } else if (tab.edit_mode) {
          running = handle_edit_key(&session, ch, &tab.edit_mode);
        } else {
          running = handle_nav_key(&session, ch, &tab.edit_mode);
        }
      } catch (const std::exception &error) {
        current_tab().session.status = std::string("project: ") + error.what();
      }
      redraw = true;
    }
    if (!running) {
      break;
    }

    const auto hints = activity.wait(std::chrono::milliseconds(0));
    if (hints.shutdown) {
      break;
    }
    if (workspace) {
      // All tab producers forward to the host. Avoid locking every local
      // mailbox on ordinary keystrokes with no activity to dispatch.
      if (hints.runtime_ready || hints.input_ready) {
        for (auto *tab : tabs) {
          const auto local = tab->activity.wait(std::chrono::milliseconds(0));
          if (local.runtime_ready)
            tab->runtime_dispatch.notify_runtime();
        }
      }
    } else if (hints.runtime_ready) {
      scratch->runtime_dispatch.notify_runtime();
    }
    // At most one bounded pump between keyboard reads. Round-robin service
    // prevents a busy inactive tab from starving other tabs or terminal input.
    for (std::size_t offset = 0; offset < tabs.size(); ++offset) {
      const auto index = (runtime_cursor + offset) % tabs.size();
      auto &tab = *tabs[index];
      if (!tab.runtime_dispatch.due(RuntimePumpDispatch::Clock::now()))
        continue;
      const bool visible = &tab == &current_tab();
      const auto result = visible
          ? pump_runtime_events_ui(&tab.session, tab.edit_mode)
          : pump_runtime_events_detailed(&tab.session, false);
      tab.runtime_dispatch.complete(result, RuntimePumpDispatch::Clock::now());
      redraw = redraw ||
          (visible && (result.handled() || result.needs_recovery()));
      runtime_cursor = (index + 1U) % tabs.size();
      break;
    }
    if (ch != ERR || redraw) {
      continue;
    }

    if (TerminalEventWaiter::input_closed(STDIN_FILENO)) {
      break;
    }
    std::optional<std::chrono::milliseconds> wait_timeout;
    const auto now = RuntimePumpDispatch::Clock::now();
    for (const auto *tab : tabs) {
      const auto deadline = tab->runtime_dispatch.wait_timeout(now);
      if (deadline && (!wait_timeout || *deadline < *wait_timeout))
        wait_timeout = deadline;
    }
    // No timer at genuine idle. Pending work coalesces more hints until the
    // earliest bounded deadline (at most 1s); stdin/SIGWINCH still wake us.
    // Omitting the shared fd during cooldown prevents a hot source spin.
    (void)terminal_wait.wait(
        STDIN_FILENO, wait_timeout ? -1 : activity_descriptor, wait_timeout);
  }

  if (workspace)
    workspace->close_activity();
  else
    scratch->session.close_activity();
  return 0;
}

int run_eval_command(const std::string &source) {
  std::vector<Cell> cells;
  Cell cell;
  cell.id = amber::notebook::allocate_cell_id();
  cell.source = source;
  cells.push_back(std::move(cell));
  const EvalView view = evaluate_prefix(cells, 0);
  if (!view.ok) {
    std::cerr << view.error << "\n";
    return 1;
  }
  for (const std::string &line : output_detail_lines(view.output_events, 120)) {
    std::cout << line << "\n";
  }
  std::cout << "=> " << view.result << "\n";
  if (view.watch_event_count != 0U) {
    std::cout << "watch events = " << view.watch_event_count << " [epoch "
              << view.watch_epoch << "]\n";
  }
  for (const LocalView &local : view.locals) {
    std::cout << local.name << " = " << local.value << " [" << local.role
              << "]";
    if (local.watched) {
      std::cout << " watch#" << local.watch_cell_id << " r"
                << local.watch_revision;
    }
    std::cout << "\n";
  }
  return 0;
}

void print_module_exports(const CompileResult &compiled) {
  for (const auto &entry : compiled.module.exports) {
    const std::string name = entry.symbol_id < compiled.module.symbols.size()
                                 ? compiled.module.symbols[entry.symbol_id]
                                 : "<invalid>";
    const std::string kind =
        entry.target_kind_str_id < compiled.module.strings.size()
            ? compiled.module.strings[entry.target_kind_str_id]
            : "<invalid>";
    std::cout << "export " << name << " [" << kind << "]\n";
  }
}

int check_project_module(const amber::notebook::LoadedProject &project,
                         const amber::notebook::LoadedProjectModule &module) {
  (void)project;
  const CompileResult compiled =
      compile_source_text(module.source, module.path, module.id);
  if (!compiled.ok) {
    std::cerr << compiled.error << "\n";
    return 1;
  }
  std::cout << "module " << module.id
            << " compiles; source was not executed or installed in sheets\n";
  print_module_exports(compiled);
  return 0;
}

int run_project_module(const amber::notebook::LoadedProject &project,
                       const amber::notebook::LoadedProjectModule &module) {
  Session session;
  load_project_module_into_session(&session, project, module);
  evaluate_from_with_progress(&session, 0U, true, false, {});
  const Cell &cell = session.cells.front();
  for (const std::string &line : output_detail_lines(cell.output_events, 120))
    std::cout << line << "\n";
  if (!cell.ok) {
    std::cerr << cell.error << "\n";
    return 1;
  }
  std::cout << "=> " << cell.result << "\n";
  std::cout << "module ran in an isolated runtime; exports were not installed "
               "in sheets\n";
  return 0;
}

int run_project_sheet(const amber::notebook::LoadedProject &project,
                       const std::string &sheet_id,
                       const std::vector<amber::runtime::RuntimeCapabilityGrant> &grants = {}) {
  Session session;
  load_project_into_session(&session, project, sheet_id);
  session.runtime_capability_grants = grants;
  const bool applied =
      apply_project_environment(&session, project, true, false, {});
  for (const std::string &line :
       output_detail_lines(session.environment_output, 120))
    std::cout << line << "\n";
  if (!applied) {
    std::cerr << (session.environment_error.empty() ? session.status
                                                   : session.environment_error)
              << "\n";
    return 1;
  }
  bool ok = true;
  for (const Cell &cell : session.cells) {
    for (const std::string &line : output_detail_lines(cell.output_events, 120))
      std::cout << line << "\n";
    if (cell.ok)
      std::cout << "cell " << cell.id << " => " << cell.result << "\n";
    else {
      std::cerr << "cell " << cell.id << ": " << cell.error << "\n";
      ok = false;
    }
  }
  return ok ? 0 : 1;
}

void usage(std::ostream &out) {
  out << "usage:\n";
  out << "  iamber\n";
  out << "  iamber --eval <source>\n";
  out << "  iamber --eval-file <file>\n";
  out << "  iamber --new-project <directory.amberbook>  (create, without "
         "running)\n";
  out << "  iamber --project <directory.amberbook> [--sheet <id>]\n";
  out << "  iamber --project <directory.amberbook> --module <id>\n";
  out << "  iamber --project <directory.amberbook> --check-module <id>\n";
  out << "  iamber --project <directory.amberbook> --run-module <id>\n";
  out << "  iamber --project <directory.amberbook> --run-sheet [<id>]\n";
  out << "  iamber --project-info <directory.amberbook>\n";
  out << "  Prefix notebook commands with --grant <cap[=target]> (repeatable),\n"
         "  e.g. iamber --grant process.spawn\n";
  out << "  In a project: F7/F8 switches sheet/module tabs without evaluation.\n";
  out << "  Ctrl-S / F4 saves the active tab; quitting warns about unsaved "
         "edits.\n";
  out << "  F6 applies bundled modules to the sheet and reevaluates Watch "
         "cells; Manual cells stay stale.\n";
}

} // namespace

#ifndef AMBER_IAMBER_TESTING
int main(int argc, char **argv) {
  try {
    std::vector<amber::runtime::RuntimeCapabilityGrant> grants;
    std::vector<char *> arguments{argv[0]};
    int first = 1;
    while (first < argc && std::string(argv[first]) == "--grant") {
      if (first + 1 == argc) throw std::runtime_error("--grant requires a capability");
      amber::runtime::RuntimeCapabilityGrant grant;
      amber::capability::CapabilityDiagnostic diagnostic;
      if (!amber::capability::parse_cli_grant(argv[first + 1], &grant, &diagnostic))
        throw std::runtime_error(diagnostic.message);
      grants.push_back(std::move(grant));
      first += 2;
    }
    arguments.insert(arguments.end(), argv + first, argv + argc);
    argc = static_cast<int>(arguments.size());
    arguments.push_back(nullptr);
    argv = arguments.data();
    if (!grants.empty() && argc > 1 && std::string(argv[1]).rfind("-", 0) == 0 &&
        std::string(argv[1]) != "--project")
      throw std::runtime_error("--grant applies to interactive notebooks and --project --run-sheet");
    if (argc == 1) {
      return run_curses_console({}, {}, {}, grants);
    }
    if (argc == 3 && std::string(argv[1]) == "--eval") {
      return run_eval_command(argv[2]);
    }
    if (argc == 3 && std::string(argv[1]) == "--eval-file") {
      return run_eval_command(read_file(argv[2]));
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
      usage(std::cout);
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--new-project") {
      const std::filesystem::path path(argv[2]);
      const auto created = amber::notebook::create_project(
          path, amber::notebook::make_project_document(path.stem().string()));
      std::cout << "created " << created.directory.string() << "\n";
      return 0;
    }
    if ((argc == 3 || (argc == 5 && std::string(argv[3]) == "--sheet")) &&
        std::string(argv[1]) == "--project") {
      return run_curses_console(amber::notebook::load_project(argv[2]),
                                argc == 5 ? argv[4] : "", {}, grants);
    }
    if (argc == 5 && std::string(argv[1]) == "--project" &&
        std::string(argv[3]) == "--module") {
      auto project = amber::notebook::load_project(argv[2]);
      auto module = amber::notebook::load_project_module(project, argv[4]);
      return run_curses_console(std::move(project), {}, std::move(module), grants);
    }
    if (argc == 5 && std::string(argv[1]) == "--project" &&
        (std::string(argv[3]) == "--check-module" ||
         std::string(argv[3]) == "--run-module")) {
      if (!grants.empty()) throw std::runtime_error("--grant applies to notebook execution, not standalone module commands");
      const auto project = amber::notebook::load_project(argv[2]);
      const auto module =
          amber::notebook::load_project_module(project, argv[4]);
      return std::string(argv[3]) == "--check-module"
                 ? check_project_module(project, module)
                 : run_project_module(project, module);
    }
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--project" &&
        std::string(argv[3]) == "--run-sheet") {
      return run_project_sheet(amber::notebook::load_project(argv[2]),
                                argc == 5 ? argv[4] : "", grants);
    }
    if (argc == 3 && std::string(argv[1]) == "--project-info") {
      const auto project = amber::notebook::load_project(argv[2]);
      std::cout << project.document.title << " (amber-notebook v1)\n";
      for (const auto &sheet : project.document.sheets) {
        std::cout << "sheet " << sheet.id << ": " << sheet.title << " ("
                  << sheet.cells.size() << " cells)"
                  << (sheet.id == project.document.active_sheet ? " [default]"
                                                                : "")
                  << "\n";
      }
      for (const auto &module : project.document.modules) {
        std::cout << "module " << module.id << ": " << module.path << "\n";
      }
      for (const auto &id : project.document.auto_imports)
        std::cout << "auto import " << id << "\n";
      std::cout << "not evaluated; use F6 Apply or --run-sheet to initialize "
                   "the environment\n";
      return 0;
    }
    if (argc == 2 && std::string(argv[1]).rfind("-", 0) != 0) {
      return run_curses_console(amber::notebook::load_project(argv[1]), {}, {}, grants);
    }
    usage(std::cerr);
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "iamber: " << error.what() << "\n";
    return 1;
  }
}
#endif
