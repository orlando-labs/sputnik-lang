#include "tools/isputnik/session.h"

#include "bytecode/emitter.h"
#include "bytecode/graph_linker.h"
#include "frontend/binder/binder.h"
#include "frontend/checker/checker.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "frontend/ast/expr.h"
#include "runtime/macro_expander.h"
#include "runtime/value_display.h"
#include "tools/isputnik/result_table.h"
#include "runtime/context.h"
#include "runtime/stdlib_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct SessionBackend {
  std::vector<sputnik::notebook::CellSource> sources;
  std::shared_ptr<const sputnik::bytecode::BcModule> module;
  std::shared_ptr<sputnik::notebook::NotebookKernel> kernel;
  std::unique_ptr<sputnik::runtime::RuntimeWorld> world;
  std::map<sputnik::notebook::CellId, std::uint32_t> code_ids;
  sputnik::notebook::VmNotebookImageManifest manifest;
  sputnik::runtime::RuntimeWatchCursor watch_cursor;
  std::uint32_t next_code_id = 1;
  std::uint64_t generation = 0;
  std::shared_ptr<const sputnik::bytecode::BcModule> environment_image;
  std::map<std::string, std::string> ambient_constant_paths;
  std::map<std::string, std::map<std::string, std::string>> import_paths;
  sputnik::frontend::ImportAliases import_aliases;
  std::vector<BundledModuleSource> bundled_sources;
  // The curses session and its progress callbacks run on the UI thread.
  // These flags prevent recursive pumping there; they are not a cross-thread
  // synchronization primitive.
  bool runtime_pump_active = false;
  bool runtime_pump_pending = false;
};

namespace {

class SessionExecutionScope {
public:
  explicit SessionExecutionScope(Session *session) : session_(session) {
    session_->evaluation_active = true;
  }
  ~SessionExecutionScope() {
    for (Cell &cell : session_->cells)
      cell.running = false;
    session_->evaluation_active = false;
    if (sputnik::runtime::runtime_run_cancel_requested())
      session_->status = "execution cancelled";
  }
  SessionExecutionScope(const SessionExecutionScope &) = delete;
  SessionExecutionScope &operator=(const SessionExecutionScope &) = delete;

private:
  Session *session_;
};

void publish_backend_candidate(SessionBackend *live,
                               SessionBackend *candidate) noexcept {
  if (live == nullptr || candidate == nullptr) {
    return;
  }
  live->sources.swap(candidate->sources);
  live->module.swap(candidate->module);
  live->code_ids.swap(candidate->code_ids);
  live->manifest.cells.swap(candidate->manifest.cells);
  std::swap(live->next_code_id, candidate->next_code_id);
  std::swap(live->generation, candidate->generation);
}

sputnik::lexer::LexResult lex_source(const std::string &source,
                                   const std::string &path) {
  sputnik::lexer::Lexer lexer(source, path);
  return lexer.lex();
}

std::vector<sputnik::lexer::Diagnostic>
effect_diagnostics_only(const sputnik::checker::CheckResult &result) {
  std::vector<sputnik::lexer::Diagnostic> diagnostics;
  for (const sputnik::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.phase == "effects") {
      diagnostics.push_back(diagnostic);
    }
  }
  return diagnostics;
}

std::string diagnostics_to_summary(
    const std::vector<sputnik::lexer::Diagnostic> &diagnostics) {
  std::ostringstream out;
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    const sputnik::lexer::Diagnostic &diagnostic = diagnostics[i];
    if (i != 0U) {
      out << "\n";
    }
    out << diagnostic.code << ": " << diagnostic.message;
    if (!diagnostic.span.file.empty() && diagnostic.span.start.line > 0) {
      out << " at " << diagnostic.span.file << ":" << diagnostic.span.start.line
          << ":" << diagnostic.span.start.col;
    }
  }
  return out.str();
}

std::string verify_errors_to_summary(
    const std::vector<sputnik::bytecode::VerifyError> &errors) {
  std::ostringstream out;
  for (std::size_t i = 0; i < errors.size(); ++i) {
    const sputnik::bytecode::VerifyError &error = errors[i];
    if (i != 0U) {
      out << "\n";
    }
    out << error.code << ": " << error.message;
    if (!error.section.empty()) {
      out << " in " << error.section;
      if (error.offset != 0U) {
        out << " at offset " << error.offset;
      }
    }
  }
  return out.str();
}

std::size_t zero_based_column(std::size_t column) {
  return column == 0U ? 0U : column - 1U;
}

void normalize_source_error_range(SourceErrorRange *range) {
  if (range == nullptr || range->start_line == 0U) {
    return;
  }
  if (range->end_line == 0U || range->end_line < range->start_line) {
    range->end_line = range->start_line;
  }
  if (range->end_line == range->start_line &&
      range->end_column <= range->start_column) {
    range->end_column = range->start_column + 1U;
  }
  if (range->has_offsets && range->end_offset < range->start_offset) {
    range->end_offset = range->start_offset;
  }
}

SourceErrorRange source_error_range_from_span(const sputnik::lexer::Span &span) {
  SourceErrorRange range;
  range.file = span.file;
  range.start_line = span.start.line;
  range.start_column = zero_based_column(span.start.col);
  range.end_line = span.end.line == 0U ? span.start.line : span.end.line;
  range.end_column = span.end.col == 0U ? range.start_column + 1U
                                        : zero_based_column(span.end.col);
  range.start_offset = span.start.offset;
  range.end_offset = span.end.offset;
  range.has_offsets = span.start.offset != 0U || span.end.offset != 0U;
  normalize_source_error_range(&range);
  return range;
}

bool same_source_error_range(const SourceErrorRange &left,
                             const SourceErrorRange &right) {
  return left.file == right.file && left.start_line == right.start_line &&
         left.start_column == right.start_column &&
         left.end_line == right.end_line &&
         left.end_column == right.end_column &&
         left.start_offset == right.start_offset &&
         left.end_offset == right.end_offset &&
         left.whole_line == right.whole_line;
}

void push_unique_source_error_range(std::vector<SourceErrorRange> *ranges,
                                    SourceErrorRange range) {
  if (ranges == nullptr || range.start_line == 0U) {
    return;
  }
  normalize_source_error_range(&range);
  for (const SourceErrorRange &existing : *ranges) {
    if (same_source_error_range(existing, range)) {
      return;
    }
  }
  ranges->push_back(std::move(range));
}

std::vector<SourceErrorRange> source_error_ranges_from_diagnostics(
    const std::vector<sputnik::lexer::Diagnostic> &diagnostics) {
  std::vector<SourceErrorRange> ranges;
  for (const sputnik::lexer::Diagnostic &diagnostic : diagnostics) {
    push_unique_source_error_range(
        &ranges, source_error_range_from_span(diagnostic.span));
  }
  return ranges;
}

std::vector<SourceErrorRange> source_error_ranges_from_loader_result(
    const sputnik::runtime::RuntimeModuleLoadResult &result) {
  std::vector<SourceErrorRange> ranges;
  for (const sputnik::runtime::RuntimeLoaderDiagnostic &diagnostic :
       result.diagnostics) {
    const sputnik::runtime::RuntimeDebugLocation &location = diagnostic.location;
    if (location.line == 0U) {
      continue;
    }
    SourceErrorRange range;
    range.file = location.file;
    range.start_line = location.line;
    range.start_column = zero_based_column(location.column);
    range.end_line = location.line;
    range.end_column = range.start_column + 1U;
    range.whole_line = location.column == 0U;
    push_unique_source_error_range(&ranges, std::move(range));
  }
  return ranges;
}

std::string loader_result_to_summary(
    const sputnik::runtime::RuntimeModuleLoadResult &result) {
  std::ostringstream out;
  if (!result.error_name.empty()) {
    out << result.error_name << ": ";
  }
  out << (result.message.empty() ? "module load failed" : result.message);
  for (const sputnik::runtime::RuntimeLoaderDiagnostic &diagnostic :
       result.diagnostics) {
    if (diagnostic.message.empty() || diagnostic.message == result.message) {
      continue;
    }
    out << "\n";
    if (!diagnostic.error_name.empty()) {
      out << diagnostic.error_name << ": ";
    }
    out << diagnostic.message;
  }
  return out.str();
}

std::string session_header_source() { return "package isputnik.session\n\n"; }

std::string evaluable_source(const Cell &cell) {
  return cell.kind == "code" ? cell.source : std::string{};
}

std::string session_source_until(const std::vector<Cell> &cells,
                                 std::size_t end_index) {
  std::ostringstream out;
  out << session_header_source();
  for (std::size_t i = 0; i <= end_index && i < cells.size(); ++i) {
    const std::string source = evaluable_source(cells[i]);
    out << source;
    if (source.empty() || source.back() != '\n') {
      out << "\n";
    }
    out << "\n";
  }
  return out.str();
}

struct CellSourceMap {
  std::size_t cell_index = 0;
  std::size_t start_line = 1;
  std::size_t source_line_count = 1;
  std::size_t start_offset = 0;
  std::size_t source_end_offset = 0;
};

std::size_t generated_cell_source_line_count(const std::string &source) {
  return static_cast<std::size_t>(
             std::count(source.begin(), source.end(), '\n')) +
         ((source.empty() || source.back() != '\n') ? 1U : 0U);
}

std::size_t generated_cell_source_size(const std::string &source) {
  return source.size() + ((source.empty() || source.back() != '\n') ? 1U : 0U);
}

std::string session_source_for_cell_only(const std::vector<Cell> &cells,
                                         std::size_t index) {
  std::ostringstream out;
  out << session_header_source();
  for (std::size_t i = 0; i < index && i < cells.size(); ++i) {
    const std::string source = evaluable_source(cells[i]);
    const std::size_t blank_lines =
        generated_cell_source_line_count(source) + 1U;
    for (std::size_t line = 0; line < blank_lines; ++line) {
      out << "\n";
    }
  }
  if (index < cells.size()) {
    const std::string source = evaluable_source(cells[index]);
    out << source;
    if (source.empty() || source.back() != '\n') {
      out << "\n";
    }
    out << "\n";
  }
  return out.str();
}

std::vector<CellSourceMap>
cell_source_maps_until(const std::vector<Cell> &cells, std::size_t end_index) {
  std::vector<CellSourceMap> maps;
  const std::string header = session_header_source();
  std::size_t line = 1U + static_cast<std::size_t>(
                              std::count(header.begin(), header.end(), '\n'));
  std::size_t offset = header.size();
  for (std::size_t i = 0; i <= end_index && i < cells.size(); ++i) {
    const std::string source = evaluable_source(cells[i]);
    const std::size_t source_line_count =
        generated_cell_source_line_count(source);
    const std::size_t source_size = generated_cell_source_size(source);
    maps.push_back(
        {i, line, source_line_count, offset, offset + source.size()});
    line += source_line_count + 1U;
    offset += source_size + 1U;
  }
  return maps;
}

std::pair<int, int> line_column_for_offset(const std::string &text,
                                           std::size_t offset) {
  int line = 0;
  int column = 0;
  offset = std::min(offset, text.size());
  for (std::size_t i = 0; i < offset; ++i) {
    if (text[i] == '\n') {
      ++line;
      column = 0;
    } else {
      ++column;
    }
  }
  return {line, column};
}

bool same_code_error_range(const CodeErrorRange &left,
                           const CodeErrorRange &right) {
  return left.start_line == right.start_line &&
         left.start_column == right.start_column &&
         left.end_line == right.end_line &&
         left.end_column == right.end_column &&
         left.whole_line == right.whole_line;
}

void push_unique_cell_error_range(std::vector<CellErrorRange> *ranges,
                                  CellErrorRange range) {
  if (ranges == nullptr) {
    return;
  }
  for (const CellErrorRange &existing : *ranges) {
    if (existing.cell_index == range.cell_index &&
        same_code_error_range(existing.range, range.range)) {
      return;
    }
  }
  ranges->push_back(std::move(range));
}

std::optional<CellErrorRange>
map_source_error_range_to_cell(const std::vector<Cell> &cells,
                               const CellSourceMap &map,
                               const SourceErrorRange &source_range) {
  if (map.cell_index >= cells.size()) {
    return std::nullopt;
  }

  CodeErrorRange local;
  local.whole_line = source_range.whole_line;
  const std::size_t end_line =
      map.start_line + std::max<std::size_t>(1U, map.source_line_count) - 1U;
  if (source_range.start_line >= map.start_line &&
      source_range.start_line <= end_line) {
    local.start_line =
        static_cast<int>(source_range.start_line - map.start_line);
    local.start_column = static_cast<int>(source_range.start_column);
    if (source_range.end_line >= map.start_line &&
        source_range.end_line <= end_line) {
      local.end_line = static_cast<int>(source_range.end_line - map.start_line);
      local.end_column = static_cast<int>(source_range.end_column);
    } else {
      local.end_line = static_cast<int>(map.source_line_count - 1U);
      local.end_column = 0;
      local.whole_line = true;
    }
    return CellErrorRange{map.cell_index, local};
  }

  if (source_range.has_offsets &&
      source_range.start_offset >= map.start_offset &&
      source_range.start_offset <= map.source_end_offset) {
    const std::size_t local_start_offset =
        std::min(source_range.start_offset - map.start_offset,
                 cells[map.cell_index].source.size());
    const std::size_t local_end_offset =
        source_range.end_offset >= map.start_offset
            ? std::min(source_range.end_offset - map.start_offset,
                       cells[map.cell_index].source.size())
            : local_start_offset;
    const auto [start_line, start_column] = line_column_for_offset(
        cells[map.cell_index].source, local_start_offset);
    const auto [end_line, end_column] =
        line_column_for_offset(cells[map.cell_index].source, local_end_offset);
    local.start_line = start_line;
    local.start_column = start_column;
    local.end_line = end_line;
    local.end_column = end_column;
    if (local.end_line == local.start_line &&
        local.end_column <= local.start_column) {
      local.end_column = local.start_column + 1;
    }
    return CellErrorRange{map.cell_index, local};
  }
  return std::nullopt;
}

std::vector<CellErrorRange>
map_source_error_ranges_to_cells(const std::vector<Cell> &cells,
                                 std::size_t end_index,
                                 const std::vector<SourceErrorRange> &ranges) {
  std::vector<CellErrorRange> mapped;
  const std::vector<CellSourceMap> maps =
      cell_source_maps_until(cells, end_index);
  for (const SourceErrorRange &range : ranges) {
    for (const CellSourceMap &map : maps) {
      std::optional<CellErrorRange> cell_range =
          map_source_error_range_to_cell(cells, map, range);
      if (cell_range.has_value()) {
        push_unique_cell_error_range(&mapped, std::move(*cell_range));
        break;
      }
    }
  }
  return mapped;
}

std::vector<CellErrorRange>
map_direct_source_ranges_to_cell(const std::vector<Cell> &cells,
                                 const std::vector<SourceErrorRange> &ranges) {
  std::vector<CellErrorRange> mapped;
  if (cells.empty()) {
    return mapped;
  }

  const std::string &source = cells.front().source;
  for (const SourceErrorRange &source_range : ranges) {
    CodeErrorRange local;
    local.whole_line = source_range.whole_line;
    // Lexer/runtime line and column values already use their source-level
    // character convention. Prefer them over deriving columns from byte
    // offsets, which would miscount UTF-8 and non-LF line endings.
    if (source_range.start_line != 0U) {
      local.start_line = static_cast<int>(source_range.start_line - 1U);
      local.start_column = static_cast<int>(source_range.start_column);
      local.end_line = static_cast<int>((source_range.end_line == 0U
                                             ? source_range.start_line
                                             : source_range.end_line) -
                                        1U);
      local.end_column = static_cast<int>(source_range.end_column);
    } else if (source_range.has_offsets) {
      const auto [start_line, start_column] = line_column_for_offset(
          source, std::min(source_range.start_offset, source.size()));
      const auto [end_line, end_column] = line_column_for_offset(
          source, std::min(source_range.end_offset, source.size()));
      local.start_line = start_line;
      local.start_column = start_column;
      local.end_line = end_line;
      local.end_column = end_column;
    } else {
      continue;
    }
    if (local.end_line == local.start_line &&
        local.end_column <= local.start_column) {
      local.end_column = local.start_column + 1;
    }
    push_unique_cell_error_range(&mapped, CellErrorRange{0U, local});
  }
  return mapped;
}

std::string first_line(std::string text) {
  const std::size_t newline = text.find('\n');
  if (newline != std::string::npos) {
    text = text.substr(0, newline);
  }
  return text;
}

std::vector<std::string> error_lines(const std::string &text) {
  std::vector<std::string> lines;
  std::string line;
  for (char c : text) {
    if (c == '\n') {
      if (!line.empty()) {
        lines.push_back(line);
      }
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  if (!line.empty()) {
    lines.push_back(line);
  }
  return lines;
}

std::vector<sputnik::runtime::RuntimeTextOutputEvent> merge_output_events(
    const std::shared_ptr<sputnik::runtime::RuntimeTextWriter> &stdout_sink,
    const std::shared_ptr<sputnik::runtime::RuntimeTextWriter> &stderr_sink) {
  std::vector<sputnik::runtime::RuntimeTextOutputEvent> events;
  if (stdout_sink != nullptr) {
    const std::vector<sputnik::runtime::RuntimeTextOutputEvent> stdout_events =
        stdout_sink->events();
    events.insert(events.end(), stdout_events.begin(), stdout_events.end());
  }
  if (stderr_sink != nullptr) {
    const std::vector<sputnik::runtime::RuntimeTextOutputEvent> stderr_events =
        stderr_sink->events();
    events.insert(events.end(), stderr_events.begin(), stderr_events.end());
  }
  std::sort(events.begin(), events.end(),
            [](const sputnik::runtime::RuntimeTextOutputEvent &left,
               const sputnik::runtime::RuntimeTextOutputEvent &right) {
              return left.order < right.order;
            });
  return events;
}

std::size_t cell_index_for_source_line(const std::vector<CellSourceMap> &maps,
                                       std::size_t fallback_index,
                                       std::uint32_t source_line) {
  if (source_line == 0U) {
    return fallback_index;
  }
  for (const CellSourceMap &map : maps) {
    const std::size_t end_line =
        map.start_line + std::max<std::size_t>(1U, map.source_line_count) - 1U;
    if (source_line >= map.start_line && source_line <= end_line) {
      return map.cell_index;
    }
  }
  return fallback_index;
}

std::vector<sputnik::runtime::RuntimeTextOutputEvent> output_events_for_cell(
    const std::vector<sputnik::runtime::RuntimeTextOutputEvent> &events,
    const std::vector<Cell> &cells, std::size_t end_index) {
  std::vector<sputnik::runtime::RuntimeTextOutputEvent> filtered;
  const std::vector<CellSourceMap> maps =
      cell_source_maps_until(cells, end_index);
  for (const sputnik::runtime::RuntimeTextOutputEvent &event : events) {
    const std::size_t event_cell =
        event.source.present
            ? cell_index_for_source_line(maps, end_index, event.source.line)
            : end_index;
    if (event_cell == end_index) {
      filtered.push_back(event);
    }
  }
  return filtered;
}

bool should_show_local(const LocalView &local) {
  // Imports lower to constants, not initialized VM locals. In particular the
  // namespace desugaring's __sputnik_ns__ aliases are compiler implementation
  // details and must not appear as broken variables in either notebook host.
  return !local.name.empty() && local.role != "temp" &&
         local.binding_kind != "import_alias";
}

bool same_cell_source(const sputnik::notebook::CellSource &left,
                      const sputnik::notebook::CellSource &right) {
  return left.id == right.id && left.source == right.source &&
         left.file == right.file && left.mode == right.mode;
}

bool same_cell_sources(const std::vector<sputnik::notebook::CellSource> &left,
                       const std::vector<sputnik::notebook::CellSource> &right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (!same_cell_source(left[i], right[i])) {
      return false;
    }
  }
  return true;
}

std::uint32_t allocate_stable_code_id(
    std::map<sputnik::notebook::CellId, std::uint32_t> *code_ids,
    std::uint32_t *next_code_id, sputnik::notebook::CellId cell_id) {
  if (code_ids == nullptr || next_code_id == nullptr || cell_id == 0U) {
    return 0;
  }
  const auto found = code_ids->find(cell_id);
  if (found != code_ids->end()) {
    return found->second;
  }
  while (*next_code_id == 0U || std::any_of(code_ids->begin(), code_ids->end(),
                                            [next_code_id](const auto &entry) {
                                              return entry.second ==
                                                     *next_code_id;
                                            })) {
    if (*next_code_id == std::numeric_limits<std::uint32_t>::max()) {
      return 0;
    }
    ++*next_code_id;
  }
  const std::uint32_t allocated = *next_code_id;
  if (*next_code_id == std::numeric_limits<std::uint32_t>::max()) {
    *next_code_id = 0;
  } else {
    ++*next_code_id;
  }
  code_ids->emplace(cell_id, allocated);
  return allocated;
}

CodeErrorRange code_error_range_from_span(const sputnik::lexer::Span &span) {
  CodeErrorRange range;
  range.start_line =
      span.start.line == 0U ? 0 : static_cast<int>(span.start.line - 1U);
  range.start_column = zero_based_column(span.start.col);
  range.end_line = span.end.line == 0U ? range.start_line
                                       : static_cast<int>(span.end.line - 1U);
  range.end_column = span.end.col == 0U
                         ? range.start_column + 1
                         : static_cast<int>(zero_based_column(span.end.col));
  range.whole_line = span.start.col == 0U;
  if (range.end_line == range.start_line &&
      range.end_column <= range.start_column) {
    range.end_column = range.start_column + 1;
  }
  return range;
}

void append_compile_diagnostics(
    EvalView *view, std::size_t cell_index,
    const std::vector<sputnik::lexer::Diagnostic> &diagnostics) {
  if (view == nullptr) {
    return;
  }
  view->error = diagnostics_to_summary(diagnostics);
  for (const sputnik::lexer::Diagnostic &diagnostic : diagnostics) {
    const CodeErrorRange range = code_error_range_from_span(diagnostic.span);
    if (range.start_line == 0 && diagnostic.span.start.line == 0U) {
      continue;
    }
    push_unique_cell_error_range(&view->error_ranges,
                                 CellErrorRange{cell_index, range});
  }
}

bool has_package_imports(const std::vector<sputnik::notebook::CellSource> &sources) {
  sputnik::runtime::RuntimeModuleRegistry modules;
  sputnik::runtime::RuntimeDispatchRegistry dispatch;
  sputnik::runtime::RuntimeTypeRegistry types;
  sputnik::runtime::register_builtin_runtime_modules(modules, dispatch, types);
  sputnik::runtime::register_core_prelude_bindings(modules);
  sputnik::runtime::register_legacy_native_type_paths(modules);
  for (const auto &source : sources) {
    const auto analysis = sputnik::notebook::analyze_cell(source);
    for (const auto &item : analysis.items) {
      if (!item || item->kind != "AstImportStmt") continue;
      for (const auto &field : item->string_fields)
        if (field.name == "module_path" &&
            (field.value == "notebook.ui" || !modules.has_namespace(field.value))) return true;
    }
  }
  return false;
}

struct BackendBuildResult {
  std::unique_ptr<SessionBackend> backend;
  std::size_t error_index = 0;
  EvalView error_view;
  std::vector<sputnik::runtime::RuntimeTextOutputEvent> environment_output;
  bool ok = false;
};

BackendBuildResult
build_backend(const std::vector<sputnik::notebook::CellSource> &sources,
              const SessionBackend *previous, std::size_t watch_event_capacity,
              SessionActivityNotifier activity_notifier,
              const std::vector<sputnik::runtime::RuntimeCapabilityGrant> &grants,
              const BundledEnvironmentPreparation *environment = nullptr,
              const SessionOutputObserver &output_observer = {},
              const std::function<void(const sputnik::bytecode::BcModule &)> &prepare_runtime = {}) {
  BackendBuildResult result;
  auto candidate = std::make_unique<SessionBackend>();
  candidate->sources = sources;
  if (previous != nullptr) {
    candidate->code_ids = previous->code_ids;
    candidate->next_code_id = previous->next_code_id;
    candidate->generation = previous->generation + 1U;
  } else {
    candidate->generation = 1U;
  }

  std::set<std::string> ambient_names;
  if (environment != nullptr) {
    candidate->environment_image = environment->image;
    candidate->ambient_constant_paths = environment->ambient_constant_paths;
    candidate->import_paths = environment->import_paths;
    candidate->import_aliases = environment->import_aliases;
    for (const auto &entry : candidate->ambient_constant_paths)
      ambient_names.insert(entry.first);
  }
  const sputnik::notebook::DependencyGraph graph(sources, ambient_names);
  std::vector<std::string> string_seed;
  std::vector<std::string> symbol_seed;
  if (previous != nullptr && previous->world != nullptr) {
    // Name ids are part of the Value ABI. Read the world's authoritative
    // tables immediately before compiling the candidate image: a failed VM
    // execution may have interned names after the last successful report.
    const sputnik::runtime::RuntimeNotebookImageNameSeed seed =
        previous->world->notebook_image_name_seed();
    string_seed = seed.strings;
    symbol_seed = seed.symbols;
  }
  sputnik::notebook::NotebookImage image = candidate->environment_image
      ? sputnik::notebook::NotebookImage(*candidate->environment_image)
      : sputnik::notebook::NotebookImage(std::move(string_seed),
                                       std::move(symbol_seed));
  if (candidate->environment_image == nullptr) {
    std::vector<std::uint32_t> reserved;
    for (const auto &source : sources) {
      reserved.push_back(allocate_stable_code_id(
          &candidate->code_ids, &candidate->next_code_id, source.id));
    }
    image.reserve_code_ids(reserved);
  }
  for (std::size_t index = 0; index < sources.size(); ++index) {
    const sputnik::notebook::CellSource &source = sources[index];
    sputnik::notebook::NotebookCompileOptions options;
    if (candidate->environment_image == nullptr) {
      options.code_id = allocate_stable_code_id(
          &candidate->code_ids, &candidate->next_code_id, source.id);
    }
    options.module_name = "isputnik.notebook";
    options.import_paths = candidate->import_paths;
    options.import_aliases = candidate->import_aliases;
    options.ambient_constant_paths.insert(
        candidate->ambient_constant_paths.begin(),
        candidate->ambient_constant_paths.end());
    sputnik::notebook::CellCompileResult compiled =
        sputnik::notebook::compile_cell(source, graph, image, options);
    if (!compiled.ok) {
      result.error_index = index;
      append_compile_diagnostics(&result.error_view, index,
                                 compiled.diagnostics);
      if (result.error_view.error.empty()) {
        result.error_view.error = "notebook cell compilation failed";
      }
      return result;
    }
    candidate->code_ids[source.id] = compiled.code_id;
  }

  auto snapshot = std::make_shared<sputnik::bytecode::BcModule>(*image.snapshot());
  // Like `sputnik run` on source, a notebook has no package manifest of its
  // own: explicit host grants also declare its requested capabilities.
  snapshot->capabilities.insert(snapshot->capabilities.end(), grants.begin(), grants.end());
  std::string image_error;
  candidate->module = verified_notebook_image(*snapshot, &image_error);
  if (candidate->module == nullptr) {
    result.error_view.error = image_error.empty()
                                  ? "notebook image verification failed"
                                  : std::move(image_error);
    return result;
  }
  std::string manifest_error;
  std::optional<sputnik::notebook::VmNotebookImageManifest> manifest =
      sputnik::notebook::vm_notebook_image_manifest(*candidate->module,
                                                  &manifest_error);
  if (!manifest.has_value()) {
    result.error_view.error = manifest_error.empty()
                                  ? "invalid notebook image manifest"
                                  : std::move(manifest_error);
    return result;
  }
  candidate->manifest = std::move(*manifest);
  if (previous == nullptr) {
    if (prepare_runtime) {
      try { prepare_runtime(*candidate->module); }
      catch (const std::exception &error) { result.error_view.error = error.what(); return result; }
    }
    candidate->kernel =
        std::make_shared<sputnik::notebook::NotebookKernel>(sources, ambient_names);
    sputnik::runtime::RuntimeWorldOptions options;
    options.capability_grants = grants;
    options.watch_event_capacity = watch_event_capacity;
    options.watch_activity_notifier = [activity_notifier]() noexcept {
      (void)activity_notifier.notify_runtime();
    };
    const std::shared_ptr<sputnik::notebook::NotebookKernel> kernel =
        candidate->kernel;
    options.external_gc_root_provider = [kernel]() {
      return kernel == nullptr ? std::vector<sputnik::runtime::Value>{}
                               : kernel->gc_roots();
    };
    candidate->world = std::make_unique<sputnik::runtime::RuntimeWorld>(
        candidate->module, std::move(options));
    if (candidate->environment_image && candidate->module->init.has_entry_code_id) {
      const auto stdout_sink =
          sputnik::runtime::RuntimeTextWriter::cell_stream("stdout");
      const auto stderr_sink =
          sputnik::runtime::RuntimeTextWriter::cell_stream("stderr");
      if (output_observer) output_observer(0, stdout_sink, stderr_sink);
      sputnik::runtime::ExecutionResult initialized;
      {
        sputnik::runtime::RuntimeOutputScope output_scope(stdout_sink, stderr_sink);
        initialized = candidate->world->execute(candidate->module->init.entry_code_id);
      }
      result.environment_output = merge_output_events(stdout_sink, stderr_sink);
      if (!initialized.ok()) {
        result.error_view.error = "bundled module initialization failed";
        if (initialized.fault.has_value()) {
          result.error_view.error += ": " + initialized.fault->error_name +
                                     ": " + initialized.fault->message;
        }
        return result;
      }
    }
    candidate->watch_cursor = candidate->world->watch_cursor();
    if (!candidate->kernel->bind_runtime_watch_source(
            candidate->watch_cursor.source)) {
      result.error_view.error = "failed to bind runtime watch stream";
      return result;
    }
  }
  result.backend = std::move(candidate);
  result.ok = true;
  return result;
}

std::size_t cell_index_for_id(const std::vector<Cell> &cells,
                              sputnik::notebook::CellId id) {
  const auto found =
      std::find_if(cells.begin(), cells.end(),
                   [id](const Cell &cell) { return cell.id == id; });
  return found == cells.end()
             ? cells.size()
             : static_cast<std::size_t>(std::distance(cells.begin(), found));
}

std::shared_ptr<const sputnik::runtime::NotebookImageInputs> image_inputs_for_cell(const Session &session) {
  auto result = std::make_shared<sputnik::runtime::NotebookImageInputs>();
  if (session.project_images) *result = *session.project_images;
  for (const auto &cell : session.cells) for (const auto &images : cell.images) {
    const auto found = result->find(images.id);
    if (found == result->end()) continue;
    found->second.images = images.images; found->second.caption = images.caption;
  }
  return result;
}

EvalView evaluate_backend_cell(SessionBackend *backend,
                               sputnik::notebook::CellId id,
                               std::shared_ptr<const sputnik::runtime::NotebookInputSnapshot> project_inputs,
                               std::shared_ptr<const sputnik::runtime::NotebookImageInputs> project_images,
                               std::shared_ptr<const std::vector<sputnik::runtime::NotebookAudioComponent>> project_audio,
                               std::shared_ptr<const sputnik::runtime::NotebookAudioInputs> audio_inputs,
                               const SessionOutputObserver &output_observer) {
  EvalView view;
  if (backend == nullptr || backend->kernel == nullptr ||
      backend->world == nullptr) {
    view.error = "isputnik persistent backend is not initialized";
    return view;
  }
  const auto code_id = backend->code_ids.find(id);
  const sputnik::notebook::VmNotebookCellManifest *manifest =
      backend->manifest.cell_for_id(id);
  if (manifest == nullptr || code_id == backend->code_ids.end() ||
      manifest->code_id != code_id->second) {
    view.error = "notebook cell has no compiled image entry";
    return view;
  }

  sputnik::notebook::NotebookVmCellExecutor adapter(
      backend->world.get(), manifest->code_id, manifest->descriptors,
      manifest->cell_id);
  adapter.set_project_inputs(std::move(project_inputs));
  adapter.set_project_images(std::move(project_images));
  adapter.set_project_audio(std::move(project_audio));
  adapter.set_audio_inputs(std::move(audio_inputs));
  sputnik::notebook::VmCellExecutionReport report;
  const std::shared_ptr<sputnik::runtime::RuntimeTextWriter> stdout_sink =
      sputnik::runtime::RuntimeTextWriter::cell_stream("stdout");
  const std::shared_ptr<sputnik::runtime::RuntimeTextWriter> stderr_sink =
      sputnik::runtime::RuntimeTextWriter::cell_stream("stderr");
  if (output_observer) output_observer(id, stdout_sink, stderr_sink);
  sputnik::notebook::CellRunResult run;
  {
    sputnik::runtime::RuntimeOutputScope output_scope(stdout_sink, stderr_sink);
    if (!adapter.valid()) {
      run.error = adapter.validation_error();
    } else {
      run = backend->kernel->run_cell(id, adapter.as_executor(&report));
    }
  }
  view.output_events = merge_output_events(stdout_sink, stderr_sink);
  view.progress = std::move(report.progress);
  view.charts = std::move(report.charts);
  view.images = std::move(report.images);
  view.audio = std::move(report.audio); view.audio_controls = std::move(report.audio_controls);
  if (!run.ok) {
    view.error = run.error.empty() ? "notebook cell failed" : run.error;
    return view;
  }

  view.ok = true;
  auto preview = sputnik::runtime::runtime_preview_value(
      report.value, sputnik::runtime::RuntimeStringifyMode::Inspect,
      backend->module.get(), &report.runtime_strings, &report.runtime_symbols);
  view.result = std::move(preview.text);
  view.result_format.truncated = preview.truncated;
  view.result_format.table_json = notebook_result_table(report.value, backend->module.get(), &report.runtime_strings, &report.runtime_symbols);
  view.result_format.is_container = report.value.is_list() || report.value.is_map() ||
      report.value.is_tuple() || report.value.is_set();
  if (view.result_format.is_container) {
    auto pretty = sputnik::runtime::runtime_preview_value(
        report.value, sputnik::runtime::RuntimeStringifyMode::Pretty,
        backend->module.get(), &report.runtime_strings, &report.runtime_symbols);
    view.result_format.pretty = std::move(pretty.text);
    view.result_format.pretty_truncated = pretty.truncated;
  }
  view.displays = std::move(report.displays);
  view.watch_epoch = report.watch_epoch;
  view.watch_event_count = report.watch_event_count;
  for (const sputnik::runtime::ExecutionLocal &local : report.locals) {
    LocalView local_view;
    local_view.name = local.name;
    local_view.role = local.role;
    local_view.binding_kind = local.binding_kind;
    local_view.initialized = local.initialized;
    local_view.watched = local.watched;
    local_view.watch_cell_id = local.watch_cell_id;
    local_view.watch_revision = local.watch_revision;
    local_view.value =
        local.initialized
            ? sputnik::runtime::runtime_preview_value(
                  local.value, sputnik::runtime::RuntimeStringifyMode::Inspect,
                  backend->module.get(), &report.runtime_strings,
                  &report.runtime_symbols).text
            : "<uninitialized>";
    local_view.text_value = local.initialized && local.value.is_string()
        ? sputnik::runtime::runtime_preview_value(
              local.value, sputnik::runtime::RuntimeStringifyMode::Display,
              backend->module.get(), &report.runtime_strings, &report.runtime_symbols).text
        : local_view.value;
    if (should_show_local(local_view)) {
      view.locals.push_back(std::move(local_view));
    }
  }
  return view;
}

void mark_blocked_cell(Session *session, std::size_t index,
                       const std::string &message) {
  if (session == nullptr || index >= session->cells.size()) {
    return;
  }
  // Do not route barriers through apply_eval: that helper intentionally
  // clears diagnostics for every cell up to index, which would erase the
  // original compile/runtime error from the failed provider.
  Cell &cell = session->cells[index];
  cell.dirty = true;
  cell.running = false;
  cell.ok = false;
  cell.result = "error";
  cell.result_format = {};
  cell.error = message;
  cell.error_ranges.clear();
  cell.errors.clear();
  cell.selected_error = 0;
}

bool waits_for_unavailable(
    const sputnik::notebook::DependencyGraph &graph, sputnik::notebook::CellId id,
    const std::set<sputnik::notebook::CellId> &unavailable) {
  for (const sputnik::notebook::BindingRead &read : graph.reads_for(id)) {
    if (read.provider.has_value() &&
        unavailable.count(read.provider->cell_id) != 0U) {
      return true;
    }
  }
  return false;
}

struct AutomaticPlanExecutionResult {
  // Manual cells and watch cells blocked by a stale manual dependency are a
  // complete automatic plan: user input, rather than replaying the same
  // runtime event, is required to make further progress. Missing cells,
  // cycles, and execution failures make a conservative stream resync
  // incomplete, so its recovery cursor must not be acknowledged.
  bool complete = true;
};

AutomaticPlanExecutionResult
execute_automatic_plan(Session *session, SessionBackend *backend,
                       const std::vector<sputnik::notebook::EvaluationStep> &plan,
                       bool show_running, const EvaluationProgress &progress) {
  AutomaticPlanExecutionResult execution;
  if (session == nullptr || backend == nullptr) {
    execution.complete = false;
    return execution;
  }
  const sputnik::notebook::DependencyGraph graph(backend->sources);
  std::set<sputnik::notebook::CellId> unavailable;
  for (const sputnik::notebook::EvaluationStep &step : plan) {
    const std::size_t index = cell_index_for_id(session->cells, step.id);
    if (sputnik::runtime::runtime_run_cancel_requested()) {
      if (index < session->cells.size())
        mark_blocked_cell(session, index, "execution cancelled before cell ran");
      execution.complete = false;
      continue;
    }
    if (index >= session->cells.size()) {
      execution.complete = false;
      continue;
    }
    // The scheduler sees only code sources (notebook_sources intentionally
    // filters text), but tolerate a stale/legacy plan containing a text ID.
    // It must never become a runtime root or an error barrier.
    if (session->cells[index].kind == "text") {
      session->cells[index].dirty = false;
      session->cells[index].running = false;
      session->cells[index].ok = false;
      session->cells[index].result = "preserved (read-only text)";
      continue;
    }
    if (step.action == sputnik::notebook::EvaluationAction::KeepManualStale) {
      mark_blocked_cell(session, index, "manual cell is stale");
      unavailable.insert(step.id);
      continue;
    }
    if (step.action ==
        sputnik::notebook::EvaluationAction::BlockedByStaleDependency) {
      mark_blocked_cell(session, index,
                        "watch cell is blocked by a stale manual dependency");
      unavailable.insert(step.id);
      continue;
    }
    if (waits_for_unavailable(graph, step.id, unavailable)) {
      mark_blocked_cell(session, index,
                        "watch cell is blocked by a failed dependency");
      unavailable.insert(step.id);
      execution.complete = false;
      continue;
    }
    if (std::optional<std::string> cycle =
            cyclic_watch_error_for_cell(session->cells, index)) {
      apply_eval(session, index, watch_cycle_eval_view(std::move(*cycle)));
      unavailable.insert(step.id);
      execution.complete = false;
      continue;
    }
    if (show_running) {
      session->cells[index].running = true;
      session->status = "runtime watch cell running";
      if (progress) {
        progress(session, index);
      }
    }
    EvalView view = evaluate_backend_cell(backend, step.id, session->project_inputs, image_inputs_for_cell(*session), session->project_audio, session->project_audio_inputs, session->output_observer);
    const bool ok = view.ok;
    apply_eval(session, index, std::move(view));
    if (!ok) {
      unavailable.insert(step.id);
      execution.complete = false;
    }
  }
  return execution;
}

void evaluate_rebuilt_backend(
    Session *session, std::size_t start, bool force_all, bool show_running,
    const EvaluationProgress &progress,
    const std::vector<sputnik::notebook::CellSource> &sources) {
  if (session == nullptr) {
    return;
  }
  std::vector<sputnik::notebook::CellSource> next_dependency_snapshot = sources;
  const SessionBackend *previous = session->backend.get();
  BackendBuildResult built =
      build_backend(sources, previous, session->runtime_watch_event_capacity,
                    session->activity_notifier(), session->runtime_capability_grants,
                    nullptr, session->output_observer, session->prepare_runtime);
  if (!built.ok) {
    // Compiler diagnostics are initially indexed in the filtered source
    // vector. Translate them back to display-cell indices before publishing;
    // rich-text cells may precede or sit between code cells.
    for (CellErrorRange &range : built.error_view.error_ranges) {
      if (range.cell_index < sources.size()) {
        range.cell_index =
            cell_index_for_id(session->cells, sources[range.cell_index].id);
      }
    }
    const std::size_t error_cell_index =
        built.error_index < sources.size()
            ? cell_index_for_id(session->cells, sources[built.error_index].id)
            : session->cells.size();
    apply_eval(session, error_cell_index, std::move(built.error_view));
    std::set<sputnik::notebook::CellId> unavailable;
    const sputnik::notebook::DependencyGraph graph(sources);
    if (built.error_index < sources.size()) {
      const sputnik::notebook::CellId failed_id = sources[built.error_index].id;
      unavailable.insert(failed_id);

      // The candidate image never reached update_cells(), so the persistent
      // backend still owns the previous graph and slot table. Install the
      // same barrier there explicitly: otherwise historical values from the
      // failed provider and its old consumers remain executable even though
      // their source transition could not be committed.
      if (previous != nullptr && previous->kernel != nullptr) {
        previous->kernel->mark_failure_barrier(failed_id);
      }

      // The rebuilt graph may lose the failed provider's edges because its
      // source has frontend diagnostics. Walk the previous graph as well so
      // every old transitive consumer is blocked, including consumers that no
      // longer have a resolvable edge in the candidate graph.
      if (previous != nullptr) {
        const sputnik::notebook::DependencyGraph old_graph(previous->sources);
        for (const sputnik::notebook::CellId id :
             old_graph.dependents_of(failed_id)) {
          const std::size_t index = cell_index_for_id(session->cells, id);
          if (index < session->cells.size()) {
            mark_blocked_cell(session, index,
                              "watch cell is blocked by a failed dependency");
            unavailable.insert(id);
          }
        }
      }
    }
    for (const sputnik::notebook::CellId id : graph.execution_order()) {
      const std::size_t index = cell_index_for_id(session->cells, id);
      if (index >= session->cells.size() || index == error_cell_index) {
        continue;
      }
      if (waits_for_unavailable(graph, id, unavailable)) {
        mark_blocked_cell(session, index,
                          "watch cell is blocked by a failed dependency");
        unavailable.insert(id);
      }
    }
    session->status = "cell compilation failed";
    return;
  }

  const sputnik::notebook::DependencyGraph graph(sources);
  const sputnik::notebook::CellId changed_id =
      start < session->cells.size() && session->cells[start].kind == "code"
          ? session->cells[start].id
          : 0U;
  const std::set<sputnik::notebook::CellId> changed =
      changed_id != 0U ? std::set<sputnik::notebook::CellId>{changed_id}
                       : std::set<sputnik::notebook::CellId>{};
  const std::size_t evaluation_error_index =
      changed_id != 0U
          ? cell_index_for_id(session->cells, changed_id)
          : [&] {
              const auto first_code = std::find_if(
                  session->cells.begin(), session->cells.end(),
                  [](const Cell &cell) { return cell.kind == "code"; });
              return first_code == session->cells.end()
                         ? session->cells.size()
                         : static_cast<std::size_t>(
                               std::distance(session->cells.begin(), first_code));
            }();
  SessionBackend *active = nullptr;
  std::vector<sputnik::notebook::EvaluationStep> plan;
  if (previous != nullptr) {
    sputnik::notebook::NotebookKernel::PreparedCellUpdate kernel_update =
        previous->kernel->prepare_update_cells(sources, changed, force_all);
    sputnik::runtime::RuntimeNotebookImageInstallResult installed =
        previous->world->install_notebook_image(built.backend->module);
    if (!installed.ok) {
      // Release the kernel execution lock before installing the failure
      // barrier into the still-published graph generation.
      kernel_update.cancel();
      EvalView install_error;
      for (const sputnik::runtime::RuntimeNotebookImageDiagnostic &diagnostic :
           installed.diagnostics) {
        if (!install_error.error.empty()) {
          install_error.error += "\n";
        }
        if (!diagnostic.error_name.empty()) {
          install_error.error += diagnostic.error_name + ": ";
        }
        install_error.error += diagnostic.message;
      }
      if (install_error.error.empty()) {
        install_error.error = "notebook image install failed";
      }
      apply_eval(session, evaluation_error_index, std::move(install_error));
      std::set<sputnik::notebook::CellId> unavailable;
      if (changed_id != 0U) {
        unavailable.insert(changed_id);
      }
      // Keep the old image/kernel active, but expose the same failure barrier
      // that a successful graph transition would have exposed. Include old
      // dependents as well because a rename/removal may erase the current
      // graph edge entirely.
      if (previous != nullptr && changed_id != 0U) {
        const sputnik::notebook::DependencyGraph old_graph(previous->sources);
        previous->kernel->mark_failure_barrier(changed_id);
        const std::vector<sputnik::notebook::CellId> old_dependents =
            old_graph.dependents_of(changed_id);
        for (const sputnik::notebook::CellId id : old_dependents) {
          const std::size_t index = cell_index_for_id(session->cells, id);
          if (index < session->cells.size() && index != start) {
            mark_blocked_cell(session, index,
                              "watch cell is blocked by a failed dependency");
            unavailable.insert(id);
          }
        }
      }
      for (const sputnik::notebook::CellId id : graph.execution_order()) {
        const std::size_t index = cell_index_for_id(session->cells, id);
        if (index < session->cells.size() && index != start &&
            waits_for_unavailable(graph, id, unavailable)) {
          mark_blocked_cell(session, index,
                            "watch cell is blocked by a failed dependency");
          unavailable.insert(id);
        }
      }
      session->status = "notebook image install failed";
      return;
    }

    // Preparation above performed every allocating graph/index operation
    // without mutating the kernel.  The world keeps its RuntimeState and heap
    // across this image install; now publish the matching kernel generation
    // with no candidate allocations, then swap the already-built adapter
    // metadata while the prepared update still owns the execution lock.
    SessionBackend *live = session->backend.get();
    if (!live->kernel->commit_update_cells(kernel_update)) {
      const sputnik::runtime::RuntimeNotebookImageInstallResult rolled_back =
          live->world->install_notebook_image(live->module);
      kernel_update.cancel();
      EvalView commit_error;
      commit_error.error = rolled_back.ok
                               ? "notebook kernel generation commit failed"
                               : "notebook kernel generation commit and image "
                                 "rollback failed";
      apply_eval(session, evaluation_error_index, std::move(commit_error));
      if (rolled_back.ok && changed_id != 0U) {
        live->kernel->mark_failure_barrier(changed_id);
      } else if (!rolled_back.ok) {
        // Never retain a backend whose image and metadata generations are no
        // longer provably aligned.  The next evaluation will build afresh.
        session->backend.reset();
      }
      session->status = "notebook kernel generation commit failed";
      return;
    }
    publish_backend_candidate(live, built.backend.get());
    active = live;
    plan = kernel_update.take_plan();
  } else {
    active = built.backend.get();
    plan = sputnik::notebook::plan_evaluation(graph, sources, changed, force_all);
  }
  std::set<sputnik::notebook::CellId> unavailable;
  std::vector<std::pair<std::size_t, EvalView>> views;
  views.reserve(plan.size());
  for (const sputnik::notebook::EvaluationStep &step : plan) {
    const std::size_t index = cell_index_for_id(session->cells, step.id);
    if (sputnik::runtime::runtime_run_cancel_requested()) {
      if (index < session->cells.size())
        mark_blocked_cell(session, index, "execution cancelled before cell ran");
      continue;
    }
    if (index >= session->cells.size()) {
      continue;
    }
    if (session->cells[index].kind == "text") {
      session->cells[index].dirty = false;
      session->cells[index].running = false;
      session->cells[index].ok = false;
      session->cells[index].result = "preserved (read-only text)";
      continue;
    }
    if (step.action == sputnik::notebook::EvaluationAction::KeepManualStale) {
      mark_blocked_cell(session, index, "manual cell is stale");
      unavailable.insert(step.id);
      continue;
    }
    if (step.action ==
        sputnik::notebook::EvaluationAction::BlockedByStaleDependency) {
      mark_blocked_cell(session, index,
                        "watch cell is blocked by a stale manual dependency");
      unavailable.insert(step.id);
      continue;
    }
    if (waits_for_unavailable(graph, step.id, unavailable)) {
      mark_blocked_cell(session, index,
                        "watch cell is blocked by a failed dependency");
      unavailable.insert(step.id);
      continue;
    }
    if (std::optional<std::string> cycle =
            cyclic_watch_error_for_cell(session->cells, index)) {
      views.emplace_back(index, watch_cycle_eval_view(std::move(*cycle)));
      unavailable.insert(step.id);
      continue;
    }
    if (show_running) {
      session->cells[index].running = true;
      session->status = "cell running";
      if (progress) {
        progress(session, index);
      }
    }
    EvalView view = evaluate_backend_cell(active, step.id, session->project_inputs, image_inputs_for_cell(*session), session->project_audio, session->project_audio_inputs, session->output_observer);
    if (!view.ok) {
      unavailable.insert(step.id);
    }
    views.emplace_back(index, std::move(view));
  }

  if (previous == nullptr) {
    session->backend = std::move(built.backend);
  }
  session->dependency_snapshot.swap(next_dependency_snapshot);
  for (auto &entry : views) {
    apply_eval(session, entry.first, std::move(entry.second));
  }
  session->status =
      force_all ? "all cells evaluated" : "dependent watch cells evaluated";
}

EvalView evaluate_source_for_cell(const std::vector<Cell> &cells,
                                  std::size_t end_index,
                                  const std::string &source, bool *compiled_ok,
                                  const std::string &source_path = "<isputnik>",
                                  const std::string &module_name_override = {},
                                  bool direct_source_mapping = false,
                                  Session *owner = nullptr) {
  EvalView view;
  if (compiled_ok != nullptr) {
    *compiled_ok = false;
  }
  CompileResult compiled =
      compile_source_text(source, source_path, module_name_override);
  if (!compiled.ok) {
    view.error = compiled.error;
    view.error_ranges =
        direct_source_mapping
            ? map_direct_source_ranges_to_cell(cells, compiled.error_ranges)
            : map_source_error_ranges_to_cells(cells, end_index,
                                               compiled.error_ranges);
    return view;
  }
  if (compiled_ok != nullptr) {
    *compiled_ok = true;
  }
  if (!compiled.module.init.has_entry_code_id) {
    view.error = "compiled module has no init entry";
    return view;
  }

  const std::string module_name =
      !module_name_override.empty()
          ? module_name_override
          : (compiled.module_name.empty() ? "isputnik.session"
                                          : compiled.module_name);
  if (owner) {
    std::vector<std::string> imports;
    const auto analysis = sputnik::notebook::analyze_cell({cells[end_index].id, source, source_path});
    for (const auto &item : analysis.items) if (item && item->kind == "AstImportStmt")
      for (const auto &field : item->string_fields) if (field.name == "module_path") imports.push_back(field.value);
    sputnik::frontend::ImportAliases aliases;
    try { aliases = sputnik::frontend::collect_import_aliases(analysis.items); }
    catch (const std::exception &error) { view.error = error.what(); return view; }
    const auto prepared = prepare_bundled_environment(owner->bundled_modules, imports, owner->package_search_directory, aliases);
    if (!prepared.ok) {
      for (const auto &diagnostic : prepared.diagnostics) view.error += diagnostic.message + "\n";
      return view;
    }
    if (!prepared.modules.empty()) {
      std::vector<sputnik::bytecode::GraphModule> modules;
      for (const auto &module : prepared.modules) modules.push_back({module.id, module.path, module.module, false});
      modules.push_back({module_name, source_path, compiled.module, false});
      sputnik::runtime::RuntimeModuleRegistry native_modules;
      sputnik::runtime::RuntimeDispatchRegistry native_dispatch;
      sputnik::runtime::RuntimeTypeRegistry native_types;
      sputnik::runtime::register_builtin_runtime_modules(native_modules, native_dispatch, native_types);
      sputnik::runtime::register_core_prelude_bindings(native_modules);
      sputnik::runtime::register_legacy_native_type_paths(native_modules);
      auto linked = sputnik::bytecode::link_graph(modules, module_name,
          [&](const auto &name) { return native_modules.has_namespace(name); }, prepared.import_aliases);
      if (!linked.ok) {
        for (const auto &diagnostic : linked.diagnostics) view.error += diagnostic.message + "\n";
        return view;
      }
      compiled.module = std::move(linked.module);
    }
  }
  sputnik::runtime::RuntimeModuleLoader loader;
  const std::vector<std::uint8_t> bytes =
      sputnik::bytecode::serialize_module(compiled.module);
  const sputnik::runtime::RuntimeModuleLoadResult added =
      loader.add_serialized_module(module_name, bytes);
  if (!added.ok) {
    view.error = loader_result_to_summary(added);
    const std::vector<SourceErrorRange> ranges =
        source_error_ranges_from_loader_result(added);
    view.error_ranges =
        direct_source_mapping
            ? map_direct_source_ranges_to_cell(cells, ranges)
            : map_source_error_ranges_to_cells(cells, end_index, ranges);
    return view;
  }

  std::shared_ptr<sputnik::runtime::RuntimeTextWriter> stdout_sink =
      sputnik::runtime::RuntimeTextWriter::cell_stream("stdout");
  std::shared_ptr<sputnik::runtime::RuntimeTextWriter> stderr_sink =
      sputnik::runtime::RuntimeTextWriter::cell_stream("stderr");
  sputnik::runtime::RuntimeModuleLoadResult initialized;
  {
    sputnik::runtime::RuntimeOutputScope output_scope(stdout_sink, stderr_sink);
    if (owner) {
      if (owner->output_observer) owner->output_observer(cells[end_index].id, stdout_sink, stderr_sink);
      const auto store = sputnik::runtime::NotebookLiveScope::store();
      if (store) store->reset_cell(cells[end_index].id);
      sputnik::runtime::NotebookLiveScope live_scope(store, cells[end_index].id);
      compiled.module.capabilities.insert(compiled.module.capabilities.end(),
          owner->runtime_capability_grants.begin(), owner->runtime_capability_grants.end());
      std::string error;
      const auto image = verified_notebook_image(compiled.module, &error);
      if (!image) { view.error = error; return view; }
      if (owner->prepare_runtime) owner->prepare_runtime(*image);
      sputnik::runtime::RuntimeWorldOptions options;
      options.capability_grants = owner->runtime_capability_grants;
      // Retain scratch module worlds through the worker's run drain, just
      // like sheet backends. Child tasks and late output keep their owner.
      owner->module_world = std::make_unique<sputnik::runtime::RuntimeWorld>(image, std::move(options));
      const auto report = owner->module_world->execute(image->init.entry_code_id);
      if (!report.ok()) {
        view.error = "ModuleInitError: " + module_name + ": " + report.fault->error_name + ": " + report.fault->message;
        for (const auto &frame : report.fault->trace) if (frame.file == source_path && frame.line > 0) {
          CodeErrorRange range;
          range.start_line = range.end_line = static_cast<int>(frame.line - 1);
          range.start_column = zero_based_column(frame.column); range.end_column = range.start_column + 1;
          view.error_ranges.push_back({end_index, range});
        }
        view.output_events = merge_output_events(stdout_sink, stderr_sink);
        return view;
      }
      initialized.ok = initialized.has_execution_result = true;
      initialized.value = report.value; initialized.locals = report.locals;
      initialized.runtime_strings = report.runtime_strings; initialized.runtime_symbols = report.runtime_symbols;
      initialized.watch_epoch = report.watch_epoch; initialized.watch_events = report.watch_events;
    } else initialized = loader.initialize_module(module_name);
  }
  view.output_events = output_events_for_cell(
      merge_output_events(stdout_sink, stderr_sink), cells, end_index);
  if (!initialized.ok) {
    view.error = loader_result_to_summary(initialized);
    const std::vector<SourceErrorRange> ranges =
        source_error_ranges_from_loader_result(initialized);
    view.error_ranges =
        direct_source_mapping
            ? map_direct_source_ranges_to_cell(cells, ranges)
            : map_source_error_ranges_to_cells(cells, end_index, ranges);
    return view;
  }
  if (!initialized.has_execution_result) {
    view.error = "module init produced no execution result";
    return view;
  }

  view.ok = true;
  auto preview = sputnik::runtime::runtime_preview_value(
      initialized.value, sputnik::runtime::RuntimeStringifyMode::Inspect,
      &compiled.module, &initialized.runtime_strings, &initialized.runtime_symbols);
  view.result = std::move(preview.text);
  view.result_format.truncated = preview.truncated;
  view.result_format.table_json = notebook_result_table(initialized.value, &compiled.module, &initialized.runtime_strings, &initialized.runtime_symbols);
  view.result_format.is_container = initialized.value.is_list() || initialized.value.is_map() ||
      initialized.value.is_tuple() || initialized.value.is_set();
  if (view.result_format.is_container) {
    auto pretty = sputnik::runtime::runtime_preview_value(
        initialized.value, sputnik::runtime::RuntimeStringifyMode::Pretty,
        &compiled.module, &initialized.runtime_strings, &initialized.runtime_symbols);
    view.result_format.pretty = std::move(pretty.text);
    view.result_format.pretty_truncated = pretty.truncated;
  }
  view.watch_epoch = initialized.watch_epoch;
  view.watch_event_count = initialized.watch_events.size();
  for (const sputnik::runtime::ExecutionLocal &local : initialized.locals) {
    LocalView local_view;
    local_view.name = local.name;
    local_view.role = local.role;
    local_view.binding_kind = local.binding_kind;
    local_view.initialized = local.initialized;
    local_view.watched = local.watched;
    local_view.watch_cell_id = local.watch_cell_id;
    local_view.watch_revision = local.watch_revision;
    local_view.value =
        local.initialized
            ? sputnik::runtime::runtime_preview_value(
                  local.value, sputnik::runtime::RuntimeStringifyMode::Inspect,
                  &compiled.module, &initialized.runtime_strings,
                  &initialized.runtime_symbols).text
            : "<uninitialized>";
    local_view.text_value = local.initialized && local.value.is_string()
        ? sputnik::runtime::runtime_preview_value(
              local.value, sputnik::runtime::RuntimeStringifyMode::Display,
              &compiled.module, &initialized.runtime_strings, &initialized.runtime_symbols).text
        : local_view.value;
    if (should_show_local(local_view)) {
      view.locals.push_back(std::move(local_view));
    }
  }
  return view;
}

bool uses_console_definitions(const Session &session) {
  if (!session.console_definitions)
    return false;
  for (const auto &source : notebook_sources(session.cells)) {
    const auto analysis = sputnik::notebook::analyze_cell(source);
    for (const auto &item : analysis.items) {
      if (item && (item->kind == "AstClassDef" || item->kind == "AstMixinDef" ||
                   item->kind == "AstDefStmt" || item->kind == "AstClauseDef"))
        return true;
    }
  }
  return false;
}

// A definition-bearing console buffer is an ordinary source program. Capture
// each cell's last value in a private local, so a Run executes its prefix once
// and still presents separate results/output in the existing cell UI.
bool evaluate_console_cells(
    Session *session, std::size_t start, bool force_all, bool show_running,
    const EvaluationProgress &progress,
    const std::vector<BundledModuleSource> &bundled_sources) {
  SessionExecutionScope execution_scope(session);
  start = std::min(start, session->cells.size() - 1U);
  std::size_t end = force_all ? session->cells.size() - 1U : start;
  if (!force_all) {
    while (end + 1U < session->cells.size() &&
           (session->cells[end + 1U].kind == "text" ||
            session->cells[end + 1U].watch))
      ++end;
  }
  std::vector<Cell> generated = session->cells;
  std::map<std::size_t, std::string> result_names;
  std::map<std::size_t, std::set<std::string>> cell_bindings;
  std::set<std::string> user_bindings;
  std::vector<std::string> imports;
  const auto user_source = session_source_until(session->cells, end);
  for (std::size_t i = 0; i <= end; ++i) {
    if (generated[i].kind != "code")
      continue;
    const auto analysis = sputnik::notebook::analyze_cell(
        {generated[i].id, generated[i].source, "<isputnik>"});
    user_bindings.insert(analysis.writes.begin(), analysis.writes.end());
    cell_bindings.emplace(i, analysis.writes);
    for (const auto &item : analysis.items) {
      if (item && item->kind == "AstImportStmt")
        for (const auto &field : item->string_fields)
          if (field.name == "module_path")
            imports.push_back(field.value);
    }
    auto name = "__isputnik_cell_result_" + std::to_string(i);
    while (user_source.find(name) != std::string::npos)
      name += "_";
    result_names.emplace(i, name);
    generated[i].source += "\n" + name + " = $_\n";
  }
  const auto fail = [&](EvalView view, std::size_t index) {
    const auto message = view.error;
    apply_eval(session, index, std::move(view));
    for (std::size_t i = index + 1U; i < session->cells.size(); ++i)
      if (session->cells[i].kind == "code")
        mark_blocked_cell(session, i,
                          "cell is blocked by a failed source program");
    session->status = "console source run failed: " + message;
    return false;
  };
  sputnik::frontend::ImportAliases aliases;
  try {
    for (std::size_t i = 0; i <= end; ++i) {
      if (generated[i].kind != "code") continue;
      const auto analysis = sputnik::notebook::analyze_cell(
          {generated[i].id, generated[i].source, "<isputnik>"});
      aliases.merge(sputnik::frontend::collect_import_aliases(analysis.items));
    }
    aliases.validate();
  } catch (const std::exception &error) {
    EvalView view;
    view.error = error.what();
    return fail(std::move(view), start);
  }
  const auto prepared = prepare_bundled_environment(bundled_sources, imports, session->package_search_directory, aliases);
  if (!prepared.ok) {
    EvalView view;
    for (const auto &diagnostic : prepared.diagnostics) {
      if (!view.error.empty())
        view.error += "\n";
      view.error += diagnostic.message;
    }
    return fail(std::move(view), start);
  }
  auto source = session_source_until(generated, end);
  for (const auto &binding : prepared.exports)
    if (!user_bindings.count(binding.name))
      source += "from " + binding.module_id + " import " + binding.name + "\n";
  auto compiled = compile_source_text(source, "<isputnik>");
  if (!compiled.ok) {
    EvalView view;
    view.error = compiled.error;
    view.error_ranges =
        map_source_error_ranges_to_cells(generated, end, compiled.error_ranges);
    const auto index = view.error_ranges.empty()
                           ? start
                           : view.error_ranges.front().cell_index;
    return fail(std::move(view), index);
  }
  std::vector<std::uint32_t> initializers;
  if (!prepared.modules.empty()) {
    std::vector<sputnik::bytecode::GraphModule> modules;
    for (const auto &module : prepared.modules)
      modules.push_back({module.id, module.path, module.module, false});
    modules.push_back(
        {compiled.module_name, "<isputnik>", compiled.module, false});
    sputnik::runtime::RuntimeModuleRegistry native_modules;
    sputnik::runtime::RuntimeDispatchRegistry native_dispatch;
    sputnik::runtime::RuntimeTypeRegistry native_types;
    sputnik::runtime::register_builtin_runtime_modules(
        native_modules, native_dispatch, native_types);
    sputnik::runtime::register_core_prelude_bindings(native_modules);
    sputnik::runtime::register_legacy_native_type_paths(native_modules);
    auto linked = sputnik::bytecode::link_graph(
        modules, compiled.module_name,
        [&](const auto &name) { return native_modules.has_namespace(name); }, prepared.import_aliases);
    if (!linked.ok) {
      EvalView view;
      for (const auto &diagnostic : linked.diagnostics)
        view.error += diagnostic.message + "\n";
      return fail(std::move(view), start);
    }
    // Execute the linker-provided module initializers in dependency order,
    // ending with our root. Its report retains the captured cell locals;
    // the merged init wrapper only exposes its own two temporary registers.
    for (const auto &name : linked.init_order)
      for (const auto &attr : linked.module.attrs) {
        const auto &key = linked.module.strings.at(attr.key_str_id);
        if (key.rfind("sputnik.merged.init:", 0) == 0 &&
            linked.module.strings.at(attr.value_str_id) == name)
          initializers.push_back(
              static_cast<std::uint32_t>(std::stoul(key.substr(20))));
      }
    compiled.module = std::move(linked.module);
  } else if (compiled.module.init.has_entry_code_id) {
    initializers.push_back(compiled.module.init.entry_code_id);
  }
  if (initializers.empty()) {
    EvalView view;
    view.error = "console source has no initialization entry";
    return fail(std::move(view), start);
  }
  // Scratch source has no persisted capability manifest. Explicit host
  // grants declare the requests as well as authorizing them, just as they do
  // for the persistent console backend.
  compiled.module.capabilities.insert(
      compiled.module.capabilities.end(),
      session->runtime_capability_grants.begin(),
      session->runtime_capability_grants.end());
  std::string image_error;
  const auto image = verified_notebook_image(compiled.module, &image_error);
  if (!image) {
    EvalView view;
    view.error = std::move(image_error);
    return fail(std::move(view), start);
  }
  if (session->prepare_runtime)
    session->prepare_runtime(*image);
  sputnik::runtime::RuntimeWorldOptions options;
  options.capability_grants = session->runtime_capability_grants;
  sputnik::runtime::RuntimeWorld world(image, std::move(options));
  const auto stdout_sink =
      sputnik::runtime::RuntimeTextWriter::cell_stream("stdout");
  const auto stderr_sink =
      sputnik::runtime::RuntimeTextWriter::cell_stream("stderr");
  if (session->output_observer)
    session->output_observer(session->cells[start].id, stdout_sink,
                             stderr_sink);
  if (show_running) {
    session->cells[start].running = true;
    session->status = "console source running";
    if (progress)
      progress(session, start);
  }
  sputnik::runtime::ExecutionResult report;
  {
    sputnik::runtime::RuntimeOutputScope output_scope(stdout_sink, stderr_sink);
    for (const auto code_id : initializers) {
      report = world.execute(code_id);
      if (!report.ok())
        break;
    }
  }
  const auto output = merge_output_events(stdout_sink, stderr_sink);
  const auto source_maps = cell_source_maps_until(generated, end);
  const auto cell_output = [&](std::size_t index) {
    std::vector<sputnik::runtime::RuntimeTextOutputEvent> events;
    for (const auto &event : output) {
      const auto owner =
          event.source.present && event.source.file == "<isputnik>"
              ? cell_index_for_source_line(source_maps, start,
                                           event.source.line)
              : start;
      if (owner == index)
        events.push_back(event);
    }
    return events;
  };
  if (!report.ok()) {
    EvalView view;
    view.error = report.fault->error_name + ": " + report.fault->message;
    std::vector<SourceErrorRange> ranges;
    std::size_t index = end;
    for (const auto &frame : report.fault->trace) {
      if (frame.file != "<isputnik>" || frame.line == 0U)
        continue;
      SourceErrorRange range;
      range.file = frame.file;
      range.start_line = range.end_line = frame.line;
      range.start_column = zero_based_column(frame.column);
      range.end_column = range.start_column + 1U;
      ranges.push_back(range);
      if (frame.code_id == initializers.back())
        index = cell_index_for_source_line(source_maps, end, frame.line);
    }
    view.error_ranges =
        map_source_error_ranges_to_cells(generated, end, ranges);
    view.output_events = cell_output(index);
    return fail(std::move(view), index);
  }
  session->backend.reset();
  session->bundled_modules = bundled_sources;
  session->environment_stale = false;
  session->environment_error.clear();
  session->dependency_snapshot = notebook_sources(session->cells);
  for (const auto &entry : result_names) {
    const auto index = entry.first;
    const auto &name = entry.second;
    const auto found = std::find_if(
        report.locals.begin(), report.locals.end(), [&](const auto &local) {
          return local.name == name && local.initialized;
        });
    EvalView view;
    if (found == report.locals.end()) {
      view.error = "console cell result is unavailable";
      return fail(std::move(view), index);
    }
    view.ok = true;
    auto preview = sputnik::runtime::runtime_preview_value(
        found->value, sputnik::runtime::RuntimeStringifyMode::Inspect,
        image.get(), &report.runtime_strings, &report.runtime_symbols);
    view.result = std::move(preview.text);
    view.result_format.truncated = preview.truncated;
    view.result_format.table_json = notebook_result_table(found->value, image.get(), &report.runtime_strings, &report.runtime_symbols);
    view.result_format.is_container =
        found->value.is_list() || found->value.is_map() ||
        found->value.is_tuple() || found->value.is_set();
    if (view.result_format.is_container) {
      auto pretty = sputnik::runtime::runtime_preview_value(
          found->value, sputnik::runtime::RuntimeStringifyMode::Pretty,
          image.get(), &report.runtime_strings, &report.runtime_symbols);
      view.result_format.pretty = std::move(pretty.text);
      view.result_format.pretty_truncated = pretty.truncated;
    }
    view.output_events = cell_output(index);
    for (const auto &local : report.locals) {
      if (!cell_bindings.at(index).count(local.name) || !local.initialized)
        continue;
      LocalView visible;
      visible.name = local.name;
      visible.role = local.role;
      visible.binding_kind = local.binding_kind;
      visible.initialized = true;
      visible.value =
          sputnik::runtime::runtime_preview_value(
              local.value, sputnik::runtime::RuntimeStringifyMode::Inspect,
              image.get(), &report.runtime_strings, &report.runtime_symbols)
              .text;
      visible.text_value =
          sputnik::runtime::runtime_preview_value(
              local.value, sputnik::runtime::RuntimeStringifyMode::Display,
              image.get(), &report.runtime_strings, &report.runtime_symbols)
              .text;
      view.locals.push_back(std::move(visible));
    }
    apply_eval(session, index, std::move(view));
  }
  for (std::size_t i = end + 1U; i < session->cells.size(); ++i)
    if (session->cells[i].kind == "code")
      mark_blocked_cell(session, i, "manual cell is stale");
  session->status = "console source evaluated";
  return true;
}

} // namespace

Session::Session() = default;
Session::~Session() { activity_.close(); }
Session::Session(Session &&) noexcept = default;
Session &Session::operator=(Session &&) noexcept = default;

SessionActivityNotifier Session::activity_notifier() const noexcept {
  return activity_.notifier().with_wakeup(activity_wakeup_);
}

void Session::set_activity_wakeup(const SessionActivityNotifier &notifier) {
  if (evaluation_active || backend != nullptr)
    throw std::logic_error("attach activity wakeup before starting a session backend");
  activity_wakeup_ = notifier;
}

SessionActivityWaiter Session::activity_waiter() const noexcept {
  return activity_.waiter();
}

void Session::close_activity() noexcept { activity_.close(); }

std::shared_ptr<const sputnik::bytecode::BcModule>
verified_notebook_image(const sputnik::bytecode::BcModule &candidate,
                        std::string *error) {
  try {
    const std::vector<std::uint8_t> bytes =
        sputnik::bytecode::serialize_module(candidate);
    sputnik::bytecode::DecodeResult decoded =
        sputnik::bytecode::deserialize_module(bytes);
    if (!decoded.ok()) {
      const std::string summary = verify_errors_to_summary(decoded.errors);
      if (error != nullptr) {
        *error = summary.empty()
                     ? "notebook image verification failed"
                     : "notebook image verification failed: " + summary;
      }
      return nullptr;
    }
    return std::make_shared<const sputnik::bytecode::BcModule>(
        std::move(decoded.module));
  } catch (const std::exception &exception) {
    if (error != nullptr) {
      *error = std::string("notebook image serialization failed: ") +
               exception.what();
    }
  } catch (...) {
    if (error != nullptr) {
      *error = "notebook image serialization failed with an unknown exception";
    }
  }
  return nullptr;
}

CompileResult compile_source_text(const std::string &source,
                                  const std::string &source_path,
                                  const std::string &module_name_override) {
  sputnik::lexer::LexResult lex_result = lex_source(source, source_path);
  if (!lex_result.ok()) {
    return {false,
            {},
            diagnostics_to_summary(lex_result.diagnostics),
            source_error_ranges_from_diagnostics(lex_result.diagnostics),
            {}};
  }

  sputnik::parser::Parser parser(lex_result.tokens);
  sputnik::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    return {false,
            {},
            diagnostics_to_summary(parse_result.diagnostics),
            source_error_ranges_from_diagnostics(parse_result.diagnostics),
            {}};
  }

  if (!module_name_override.empty() && !parse_result.module_name.empty() &&
      parse_result.module_name != module_name_override) {
    sputnik::lexer::Span span;
    for (const std::unique_ptr<sputnik::ast::Expr> &item : parse_result.items) {
      if (item != nullptr && item->kind == "AstPackageDecl") {
        span = item->span;
        break;
      }
    }
    const std::vector<sputnik::lexer::Diagnostic> diagnostics = {{
        "IAM1001",
        "error",
        "isputnik",
        "bundled module id '" + module_name_override +
            "' does not match source package '" + parse_result.module_name +
            "'",
        span,
    }};
    return {false,
            {},
            diagnostics_to_summary(diagnostics),
            source_error_ranges_from_diagnostics(diagnostics),
            {}};
  }
  const std::string module_name =
      module_name_override.empty() ? parse_result.module_name
                                   : module_name_override;

  const auto macro_result = sputnik::macros::expand_macros(
      parse_result.items, module_name, source);
  if (!macro_result.ok) {
    return {false, {}, "macro expansion error: " + macro_result.error, {}, {}};
  }
  sputnik::ast::expand_quotes(parse_result.items);

  sputnik::binder::BindResult bind_result =
      sputnik::binder::bind_module(parse_result.items, module_name);
  if (!bind_result.ok()) {
    return {false,
            {},
            diagnostics_to_summary(bind_result.diagnostics),
            source_error_ranges_from_diagnostics(bind_result.diagnostics),
            {}};
  }
  const std::vector<sputnik::lexer::Diagnostic> unresolved_name_diagnostics =
      sputnik::binder::unresolved_name_diagnostics(parse_result.items,
                                                 bind_result.graph);
  if (!unresolved_name_diagnostics.empty()) {
    return {false,
            {},
            diagnostics_to_summary(unresolved_name_diagnostics),
            source_error_ranges_from_diagnostics(unresolved_name_diagnostics),
            {}};
  }

  sputnik::checker::CheckResult check_result = sputnik::checker::check_module(
      parse_result.items, module_name, bind_result.graph);
  const std::vector<sputnik::lexer::Diagnostic> effect_diagnostics =
      effect_diagnostics_only(check_result);
  if (!effect_diagnostics.empty()) {
    return {false,
            {},
            diagnostics_to_summary(effect_diagnostics),
            source_error_ranges_from_diagnostics(effect_diagnostics),
            {}};
  }

  sputnik::hir::Program program = sputnik::hir::lower_module(
      parse_result.items, module_name, bind_result.graph);
  sputnik::bytecode::EmitResult emit_result =
      sputnik::bytecode::emit_program(program, module_name);
  if (!emit_result.ok()) {
    return {false,
            {},
            diagnostics_to_summary(emit_result.diagnostics),
            source_error_ranges_from_diagnostics(emit_result.diagnostics),
            {}};
  }
  emit_result.module.effects = check_result.effect_summaries;
  const std::vector<std::uint8_t> bytes =
      sputnik::bytecode::serialize_module(emit_result.module);
  sputnik::bytecode::DecodeResult decode_result =
      sputnik::bytecode::deserialize_module(bytes);
  if (!decode_result.ok()) {
    return {false, {}, verify_errors_to_summary(decode_result.errors), {}, {}};
  }
  return {true, std::move(decode_result.module), {}, {}, module_name};
}

std::vector<sputnik::notebook::CellSource>
notebook_sources(const std::vector<Cell> &cells) {
  std::vector<sputnik::notebook::CellSource> sources;
  sources.reserve(cells.size());
  for (const Cell &cell : cells) {
    // Rich-text cells are display-only inputs to this backend.  Omitting them
    // entirely keeps the installed graph/image byte-for-byte stable when text
    // is edited, moved, or reformatted; stable CellIds preserve every code
    // cell's runtime identity and output association.
    if (cell.kind != "code") {
      continue;
    }
    sputnik::notebook::CellSource source;
    source.id = cell.id;
    source.source = cell.source;
    source.file = "<isputnik-cell-" + std::to_string(cell.id) + ">";
    source.mode = cell.watch ? sputnik::notebook::CellMode::Watch
                             : sputnik::notebook::CellMode::Manual;
    sources.push_back(std::move(source));
  }
  return sources;
}

sputnik::notebook::DependencyGraph
dependency_graph_for_cells(const std::vector<Cell> &cells) {
  return sputnik::notebook::DependencyGraph(notebook_sources(cells));
}

std::optional<std::string>
cyclic_watch_error_for_cell(const std::vector<Cell> &cells, std::size_t index) {
  if (index >= cells.size() || !cells[index].watch) {
    return std::nullopt;
  }
  const sputnik::notebook::DependencyGraph graph =
      dependency_graph_for_cells(cells);
  const sputnik::notebook::CellId id = cells[index].id;
  for (const std::vector<sputnik::notebook::CellId> &cycle : graph.cycles()) {
    if (std::find(cycle.begin(), cycle.end(), id) != cycle.end()) {
      std::ostringstream out;
      out << "cyclic watch dependency: cell " << (index + 1U)
          << " participates in a dependency cycle";
      return out.str();
    }
  }
  return std::nullopt;
}

EvalView watch_cycle_eval_view(std::string message) {
  EvalView view;
  view.error = std::move(message);
  return view;
}

EvalView evaluate_prefix(const std::vector<Cell> &cells,
                         std::size_t end_index) {
  if (end_index > 0U && end_index < cells.size()) {
    bool compiled_ok = false;
    EvalView isolated = evaluate_source_for_cell(
        cells, end_index, session_source_for_cell_only(cells, end_index),
        &compiled_ok);
    if (compiled_ok) {
      return isolated;
    }
  }
  return evaluate_source_for_cell(
      cells, end_index, session_source_until(cells, end_index), nullptr);
}

void clear_error_ranges_until(Session *session, std::size_t end_index) {
  if (session == nullptr || session->cells.empty()) {
    return;
  }
  end_index = std::min(end_index, session->cells.size() - 1U);
  for (std::size_t i = 0; i <= end_index; ++i) {
    session->cells[i].error_ranges.clear();
    session->cells[i].errors.clear();
    session->cells[i].selected_error = 0;
  }
}

std::string error_message_for_index(const std::vector<std::string> &messages,
                                    std::size_t index,
                                    const std::string &fallback) {
  if (messages.empty()) {
    return fallback.empty() ? "error" : first_line(fallback);
  }
  return messages[std::min(index, messages.size() - 1U)];
}

void push_cell_error(Cell *cell, std::string message,
                     const CodeErrorRange *range) {
  if (cell == nullptr) {
    return;
  }
  CellErrorView error;
  error.message = std::move(message);
  if (range != nullptr) {
    error.range = *range;
    error.has_range = true;
  }
  cell->errors.push_back(std::move(error));
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

void clamp_selected_error(Cell *cell) {
  if (cell == nullptr) {
    return;
  }
  if (cell->errors.empty()) {
    cell->selected_error = 0;
    return;
  }
  cell->selected_error =
      std::min(cell->selected_error, cell->errors.size() - 1U);
}

bool selected_cell_has_errors(const Session *session) {
  return session != nullptr && session->selected < session->cells.size() &&
         !session->cells[session->selected].errors.empty();
}

void focus_selected_error(Session *session) {
  if (!selected_cell_has_errors(session)) {
    return;
  }
  Cell &cell = session->cells[session->selected];
  clamp_selected_error(&cell);
  const CellErrorView &error = cell.errors[cell.selected_error];
  if (error.has_range) {
    const int line = std::max(0, error.range.start_line);
    const int column = std::max(0, error.range.start_column);
    cell.cursor = offset_for_line_column(cell.source, line, column);
    session->preferred_column = column;
    if (line < session->editor_scroll) {
      session->editor_scroll = line;
    }
  }
}

void apply_eval(Session *session, std::size_t index, EvalView view) {
  if (session == nullptr || index >= session->cells.size()) {
    return;
  }
  if (session->execution_finished) session->execution_finished(session->cells[index].id);
  const std::vector<std::string> messages = error_lines(view.error);
  clear_error_ranges_until(session, index);
  for (std::size_t i = 0; i < view.error_ranges.size(); ++i) {
    const CellErrorRange &range = view.error_ranges[i];
    if (range.cell_index < session->cells.size()) {
      session->cells[range.cell_index].error_ranges.push_back(range.range);
      push_cell_error(&session->cells[range.cell_index],
                      error_message_for_index(messages, i, view.error),
                      &range.range);
    }
  }
  const std::size_t unmatched_start =
      view.error_ranges.empty()
          ? 0U
          : std::min(messages.size(), view.error_ranges.size());
  for (std::size_t i = unmatched_start; i < messages.size(); ++i) {
    push_cell_error(&session->cells[index], messages[i], nullptr);
  }
  if (!view.ok && messages.empty() && view.error_ranges.empty()) {
    push_cell_error(&session->cells[index], view.error, nullptr);
  }

  Cell *cell = &session->cells[index];
  cell->dirty = false;
  cell->running = false;
  cell->ok = view.ok;
  cell->result_format = view.ok ? std::move(view.result_format) : ResultFormat{};
  cell->locals = std::move(view.locals);
  cell->output_events = std::move(view.output_events);
  cell->progress = std::move(view.progress);
  cell->charts = std::move(view.charts);
  cell->images = std::move(view.images);
  if (view.ok || !view.audio.empty()) cell->audio = std::move(view.audio);
  cell->audio_controls = std::move(view.audio_controls);
  cell->watch_epoch = view.watch_epoch;
  cell->watch_event_count = view.watch_event_count;
  if (view.ok) {
    cell->displays = std::move(view.displays);
    cell->result = std::move(view.result);
    cell->error.clear();
  } else {
    cell->result = "error";
    cell->error = std::move(view.error);
    if (index == session->selected && !cell->errors.empty()) {
      focus_selected_error(session);
    }
  }
}

void evaluate_from_with_progress(Session *session, std::size_t start,
                                 bool force_all, bool show_running,
                                 const EvaluationProgress &progress) {
  if (session == nullptr || session->cells.empty()) {
    return;
  }
  if (session->evaluation_active) {
    session->status = "evaluation is already running";
    return;
  }
  if (session->environment_stale) {
    session->status = "environment changed; F6 Apply this sheet";
    return;
  }
  if (session->module_editor) {
    SessionExecutionScope execution_scope(session);
    if (session->cells.size() != 1U) {
      session->status = "module editor requires exactly one source buffer";
      return;
    }
    if (show_running && progress)
      progress(session, 0U);
    EvalView view = evaluate_source_for_cell(
        session->cells, 0U, session->cells.front().source, nullptr,
        session->project_module_path.empty() ? "<isputnik-module>"
                                             : session->project_module_path,
        session->project_module_id, true, session);
    const bool ok = view.ok;
    apply_eval(session, 0U, std::move(view));
    session->status =
        ok ? "module run ok in isolated runtime; not installed in sheets"
           : "module run failed; previous sheet environment unchanged";
    return;
  }
  if (!session->execution_block_reason.empty()) {
    session->status = "evaluation blocked: " + session->execution_block_reason;
    return;
  }
  if (start < session->cells.size() && session->cells[start].kind == "text" &&
      !force_all) {
    // A direct Run on a rich-text cell is a no-op. In particular, do not
    // construct/apply a bundled environment merely because this is the first
    // action taken on a text-only sheet.
    session->cells[start].dirty = false;
    session->cells[start].running = false;
    session->cells[start].result = "preserved (read-only text)";
    session->status = "text cell is inert; nothing to run";
    return;
  }
  // An unknown kind could be an executable provider (UI inputs, module cell,
  // etc.). Silently skipping it would give downstream code a false context.
  for (const auto &cell : session->cells) {
    if (cell.kind != "code" && cell.kind != "text") {
      session->status =
          "evaluation blocked: unsupported cell kind " + cell.kind;
      return;
    }
  }
  // A deferred runtime cursor may still have events even when the newly
  // installed/evaluated source emits none. Recheck it on the next owner turn,
  // never from this scope or its progress callback.
  struct ActivityRecheck {
    SessionActivityNotifier notifier;
    ~ActivityRecheck() { (void)notifier.notify_runtime(); }
  } recheck{session->activity_notifier()};
  start = std::min(start, session->cells.size() - 1U);
  for (Cell &cell : session->cells) {
    if (cell.id == 0) {
      cell.id = sputnik::notebook::allocate_cell_id();
    }
  }

  if (uses_console_definitions(*session)) {
    evaluate_console_cells(session, start, force_all, show_running, progress,
                           session->bundled_modules);
    return;
  }

  const std::vector<sputnik::notebook::CellSource> sources =
      notebook_sources(session->cells);
  const bool composite = !session->bundled_modules.empty() || has_package_imports(sources) ||
      (session->backend != nullptr && session->backend->environment_image);
  if (composite && (session->backend == nullptr ||
                   !same_cell_sources(session->backend->sources, sources))) {
    if (!apply_bundled_environment(session, session->bundled_modules, force_all,
                                   show_running, progress))
      return;
    // Apply has already run Watch cells. An explicitly selected Manual cell
    // is the only additional root authorized by this evaluation command.
    if (force_all || session->cells[start].watch)
      return;
  }
  SessionExecutionScope execution_scope(session);
  if (session->backend == nullptr ||
      !same_cell_sources(session->backend->sources, sources)) {
    evaluate_rebuilt_backend(session, start, force_all, show_running, progress,
                             sources);
    return;
  }

  if (sources.empty()) {
    session->dependency_snapshot = sources;
    session->status = force_all ? "all cells evaluated"
                                : "text cell is inert; nothing to run";
    return;
  }
  sputnik::notebook::CellId evaluation_root = session->cells[start].id;
  if (session->cells[start].kind == "text") {
    const auto first_code = std::find_if(
        session->cells.begin(), session->cells.end(),
        [](const Cell &cell) { return cell.kind == "code"; });
    if (first_code == session->cells.end()) {
      session->status = force_all ? "all cells evaluated"
                                  : "text cell is inert; nothing to run";
      return;
    }
    evaluation_root = first_code->id;
  }
  const sputnik::notebook::DependencyGraph graph(sources);
  const std::vector<sputnik::notebook::EvaluationStep> plan =
      session->backend->kernel->update_cells(sources, evaluation_root,
                                             force_all);
  session->dependency_snapshot = sources;
  std::set<sputnik::notebook::CellId> unavailable;
  for (const sputnik::notebook::EvaluationStep &step : plan) {
    const sputnik::notebook::CellId id = step.id;
    const auto found =
        std::find_if(session->cells.begin(), session->cells.end(),
                     [id](const Cell &cell) { return cell.id == id; });
    if (found == session->cells.end()) {
      continue;
    }
    const std::size_t i =
        static_cast<std::size_t>(std::distance(session->cells.begin(), found));
    if (sputnik::runtime::runtime_run_cancel_requested()) {
      mark_blocked_cell(session, i, "execution cancelled before cell ran");
      continue;
    }
    if (step.action == sputnik::notebook::EvaluationAction::KeepManualStale) {
      mark_blocked_cell(session, i, "manual cell is stale");
      unavailable.insert(id);
      continue;
    }
    if (step.action ==
        sputnik::notebook::EvaluationAction::BlockedByStaleDependency) {
      mark_blocked_cell(session, i,
                        "watch cell is blocked by a stale manual dependency");
      unavailable.insert(id);
      continue;
    }
    if (waits_for_unavailable(graph, id, unavailable)) {
      mark_blocked_cell(session, i,
                        "watch cell is blocked by a failed dependency");
      unavailable.insert(id);
      continue;
    }
    if (std::optional<std::string> cycle =
            cyclic_watch_error_for_cell(session->cells, i)) {
      apply_eval(session, i, watch_cycle_eval_view(std::move(*cycle)));
      unavailable.insert(id);
      continue;
    }
    if (show_running) {
      session->cells[i].running = true;
      session->status = "cell running";
      if (progress) {
        progress(session, i);
      }
    }
    EvalView view = evaluate_backend_cell(session->backend.get(), id, session->project_inputs, image_inputs_for_cell(*session), session->project_audio, session->project_audio_inputs, session->output_observer);
    const bool ok = view.ok;
    apply_eval(session, i, std::move(view));
    if (!ok) {
      unavailable.insert(id);
    }
  }
  session->status =
      force_all ? "all cells evaluated" : "dependent watch cells evaluated";
}

bool apply_bundled_environment(
    Session *session, const std::vector<BundledModuleSource> &bundled_sources,
    bool force_all, bool show_running, const EvaluationProgress &progress) {
  if (session == nullptr || session->module_editor)
    return false;
  if (session->environment_apply_active) {
    session->status = "environment Apply is already running";
    return false;
  }
  if (session->evaluation_active) {
    session->status = "environment Apply deferred until evaluation completes";
    return false;
  }
  if (session->backend != nullptr && session->backend->runtime_pump_active) {
    session->status = "environment Apply deferred until runtime watch evaluation completes";
    return false;
  }
  if (!session->execution_block_reason.empty() ||
      std::any_of(session->cells.begin(), session->cells.end(),
                  [](const Cell &cell) {
                    return cell.kind != "code" && cell.kind != "text";
                  })) {
    session->status = "environment Apply blocked by unsupported sheet content";
    return false;
  }
  if (uses_console_definitions(*session))
    return evaluate_console_cells(session, 0U, force_all, show_running, progress,
                                  bundled_sources);
  struct ApplyScope {
    Session *session;
    explicit ApplyScope(Session *value) : session(value) {
      session->environment_apply_active = true;
    }
    ~ApplyScope() { session->environment_apply_active = false; }
  } scope(session);
  SessionExecutionScope execution_scope(session);

  // Stage document/display state as well as the image. A failed compilation
  // or module init must not erase successful cell results in the old world.
  std::unique_ptr<SessionBackend> candidate;
  std::vector<Cell> next_cells;
  std::vector<sputnik::notebook::CellSource> next_sources;
  std::vector<BundledModuleSource> next_bundled;
  std::vector<sputnik::runtime::RuntimeTextOutputEvent> next_output;
  std::vector<sputnik::notebook::EvaluationStep> plan;
  try {
    next_cells = session->cells;
    next_bundled = bundled_sources;
    for (Cell &cell : next_cells) {
      if (cell.id == 0U)
        cell.id = sputnik::notebook::allocate_cell_id();
      if (cell.kind == "text") {
        // Text is inert and read-only in isputnik. Applying the code
        // environment must not make it look unevaluated or dirty.
        cell.ok = false;
        cell.running = false;
        cell.dirty = false;
        cell.result = "preserved (read-only text)";
        cell.error.clear();
        cell.locals.clear();
        cell.displays.clear();
        cell.output_events.clear();
        cell.error_ranges.clear();
        cell.errors.clear();
        cell.selected_error = 0U;
        cell.watch_epoch = 0U;
        cell.watch_event_count = 0U;
        continue;
      }
      cell.ok = false;
      cell.running = false;
      cell.dirty = true;
      cell.result = "not evaluated";
      cell.result_format = {};
      cell.error.clear();
      cell.locals.clear();
      // Figure bytes are immutable host output, not handles into the old
      // world. Retain the last complete batch until a successful cell replaces
      // it, including when a source edit requires rebuilding bundled modules.
      // `ok = false` / dirty above makes the UI label this snapshot stale.
      cell.output_events.clear();
      cell.error_ranges.clear();
      cell.errors.clear();
      cell.selected_error = 0U;
      cell.watch_epoch = 0U;
      cell.watch_event_count = 0U;
    }
    next_sources = notebook_sources(next_cells);
    std::vector<std::string> explicit_imports;
    sputnik::frontend::ImportAliases aliases;
    // Imports are compile-time, cell-local declarations. Native imports keep
    // their existing behavior; source modules become roots of this generation.
    for (const auto &source : next_sources) {
      const auto analysis = sputnik::notebook::analyze_cell(source);
      aliases.merge(sputnik::frontend::collect_import_aliases(analysis.items));
      for (const auto &item : analysis.items) {
        if (item && item->kind == "AstImportStmt") {
          for (const auto &field : item->string_fields)
            if (field.name == "module_path") explicit_imports.push_back(field.value);
        }
      }
    }
    const BundledEnvironmentPreparation prepared =
        prepare_bundled_environment(next_bundled, explicit_imports, session->package_search_directory, aliases);
    if (!prepared.ok) {
      std::string message;
      for (const auto &error : prepared.diagnostics) {
        if (!message.empty())
          message += "\n";
        if (!error.path.empty())
          message += error.path + ": ";
        message += error.message;
      }
      session->environment_error = message.empty()
          ? "environment preparation failed" : std::move(message);
      session->status = "environment Apply failed: " + session->environment_error;
      return false;
    }
    BackendBuildResult built = build_backend(
        next_sources, nullptr, session->runtime_watch_event_capacity,
        session->activity_notifier(), session->runtime_capability_grants, &prepared,
        session->output_observer, session->prepare_runtime);
    if (!built.ok) {
      session->environment_error = std::move(built.error_view.error);
      session->environment_output = std::move(built.environment_output);
      session->status = "environment Apply failed: " + session->environment_error;
      return false;
    }
    candidate = std::move(built.backend);
    candidate->bundled_sources = next_bundled;
    if (session->backend != nullptr && session->backend->generation ==
            std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("notebook environment generation exhausted");
    candidate->generation = session->backend == nullptr
        ? 1U : session->backend->generation + 1U;
    next_output = std::move(built.environment_output);
    // Every slot belonged to the previous world. Treat every current cell as
    // an automatic root, retaining the Manual stale barrier unless Run All
    // was explicitly requested.
    std::set<sputnik::notebook::CellId> roots;
    std::set<std::string> ambient_names;
    for (const auto &source : next_sources)
      roots.insert(source.id);
    for (const auto &binding : candidate->ambient_constant_paths)
      ambient_names.insert(binding.first);
    const sputnik::notebook::DependencyGraph graph(next_sources, ambient_names);
    plan = sputnik::notebook::plan_automatic_evaluation(
        graph, next_sources, roots, force_all);
  } catch (const std::exception &error) {
    session->environment_error = error.what();
    session->status = "environment Apply failed: " + session->environment_error;
    return false;
  }

  session->backend.swap(candidate);
  session->cells.swap(next_cells);
  session->bundled_modules.swap(next_bundled);
  session->dependency_snapshot.swap(next_sources);
  session->environment_output.swap(next_output);
  session->environment_error.clear();
  session->environment_stale = false;
  // Module initialization is the commit boundary. A cell fault or a host
  // callback failure after this point cannot undo cells already executed.
  try {
    const auto execution = execute_automatic_plan(
        session, session->backend.get(), plan, show_running, progress);
    session->status = execution.complete
        ? (force_all ? "environment applied; all cells evaluated"
                     : "environment applied; Watch cells evaluated; Manual cells stay stale")
        : "environment applied; some cells failed";
  } catch (const std::exception &error) {
    session->status = std::string("environment applied; evaluation interrupted: ") +
                      error.what();
  } catch (...) {
    session->status = "environment applied; evaluation interrupted by host callback";
  }
  (void)session->activity_notifier().notify_runtime();
  return true;
}

void evaluate_from(Session *session, std::size_t start, bool force_all,
                   bool edit_mode, bool show_running) {
  (void)edit_mode;
  evaluate_from_with_progress(session, start, force_all, show_running, {});
}

void invalidate_project_input_changes(Session *session, const std::set<std::string> &keys) {
  if (!session || keys.empty()) return;
  if (session->evaluation_active) throw std::runtime_error("cannot change inputs during evaluation");
  // The desktop host has presentation snapshots but no in-process kernel.
  // Its isolated worker owns the precise dependency read set. Conservatively
  // mark outputs stale here until that worker accepts the new input snapshot.
  if (!session->backend) {
    for (auto &cell : session->cells) if (cell.kind == "code") cell.dirty = true;
    return;
  }
  for (const auto &step : session->backend->kernel->plan_input_changes(keys)) {
    const auto index = cell_index_for_id(session->cells, step.id);
    if (index < session->cells.size()) session->cells[index].dirty = true;
  }
}

void apply_project_input_changes(Session *session, const std::set<std::string> &keys) {
  if (!session || session->module_editor || keys.empty() || !session->backend) return;
  if (session->evaluation_active) throw std::runtime_error("cannot change inputs during evaluation");
  SessionExecutionScope execution_scope(session);
  const auto plan = session->backend->kernel->plan_input_changes(keys);
  if (session->environment_stale || !session->execution_block_reason.empty() ||
      !same_cell_sources(session->backend->sources, notebook_sources(session->cells))) {
    for (auto &cell : session->cells) if (cell.kind == "code") cell.dirty = true;
    session->status = "inputs changed; run edited sources explicitly";
    return;
  }
  const auto executed = execute_automatic_plan(session, session->backend.get(), plan, false, {});
  session->status = executed.complete ? "project inputs updated; Watch consumers evaluated" : "project input evaluation failed";
}

bool has_pending_runtime_events(const Session &session) {
  if (!session.backend || !session.backend->world) return false;
  const auto polled = session.backend->world->poll_watch_events(
      session.backend->watch_cursor, 1);
  return polled.status != sputnik::runtime::RuntimeWatchPollStatus::Ok ||
         !polled.events.empty();
}

RuntimeEventPumpResult
pump_runtime_events_detailed(Session *session, bool show_running,
                             const EvaluationProgress &progress) {
  RuntimeEventPumpResult result;
  if (session != nullptr && session->evaluation_active) {
    if (session->backend != nullptr && session->backend->runtime_pump_active)
      session->backend->runtime_pump_pending = true;
    result.disposition = RuntimeEventPumpDisposition::Busy;
    return result;
  }
  if (session == nullptr || session->backend == nullptr ||
      session->backend->world == nullptr ||
      session->backend->kernel == nullptr) {
    result.error = "runtime watch backend is unavailable";
    return result;
  }
  if (session->environment_stale || !session->execution_block_reason.empty() ||
      std::any_of(session->cells.begin(), session->cells.end(),
                  [](const Cell &cell) {
                    return cell.kind != "code" && cell.kind != "text";
                  })) {
    result.disposition = RuntimeEventPumpDisposition::DeferredByEdits;
    return result;
  }
  SessionBackend *backend = session->backend.get();
  if (backend->runtime_pump_active) {
    backend->runtime_pump_pending = true;
    result.disposition = RuntimeEventPumpDisposition::Busy;
    return result;
  }
  if (!same_cell_sources(backend->sources, notebook_sources(session->cells))) {
    result.disposition = RuntimeEventPumpDisposition::DeferredByEdits;
    return result;
  }
  result.disposition = RuntimeEventPumpDisposition::NoWork;
  SessionExecutionScope execution_scope(session);

  struct PumpScope {
    explicit PumpScope(SessionBackend *backend) : backend(backend) {
      backend->runtime_pump_active = true;
    }
    ~PumpScope() { backend->runtime_pump_active = false; }
    SessionBackend *backend;
  } scope(backend);

  constexpr std::size_t kMaxEventsPerBatch = 256U;
  constexpr std::size_t kMaxDrainRounds = 8U;
  for (std::size_t round = 0; round < kMaxDrainRounds; ++round) {
    ++result.rounds;
    backend->runtime_pump_pending = false;
    sputnik::runtime::RuntimeWatchPollResult polled;
    try {
      polled = backend->world->poll_watch_events(backend->watch_cursor,
                                                 kMaxEventsPerBatch);
    } catch (const std::exception &error) {
      session->status =
          std::string("runtime watch poll failed: ") + error.what();
      result.disposition = RuntimeEventPumpDisposition::Failed;
      result.cursor_retained = true;
      result.error = session->status;
      break;
    } catch (...) {
      session->status = "runtime watch poll failed";
      result.disposition = RuntimeEventPumpDisposition::Failed;
      result.cursor_retained = true;
      result.error = session->status;
      break;
    }
    if (polled.status ==
        sputnik::runtime::RuntimeWatchPollStatus::InvalidCursor) {
      session->status = "runtime watch cursor is invalid";
      result.disposition = RuntimeEventPumpDisposition::InvalidCursor;
      result.cursor_retained = true;
      result.error = session->status;
      break;
    }
    const bool source_changed =
        polled.status == sputnik::runtime::RuntimeWatchPollStatus::SourceChanged;
    const bool resync =
        polled.status == sputnik::runtime::RuntimeWatchPollStatus::Overflow ||
        source_changed;
    if (!resync && polled.events.empty()) {
      if (!backend->runtime_pump_pending) {
        break;
      }
      continue;
    }
    result.observed_events += polled.events.size();

    try {
      std::vector<sputnik::notebook::EvaluationStep> plan;
      sputnik::runtime::RuntimeWatchCursor acknowledgement = polled.next_cursor;
      if (source_changed) {
        // Runtime dependency IDs only have meaning in their originating
        // world. Drop those snapshots and foreign-owned slot values before
        // rebuilding conservatively from the retained sources/static graph.
        if (!backend->kernel->reset_runtime_watch_source(polled.source)) {
          session->status = "runtime watch source reset failed";
          result.disposition = RuntimeEventPumpDisposition::Failed;
          result.cursor_retained = true;
          result.error = session->status;
          break;
        }
        plan = backend->kernel->plan_runtime_resync();
      } else if (resync) {
        plan = backend->kernel->plan_runtime_resync();
      } else {
        const sputnik::notebook::RuntimeEventBatchPlanResult batch =
            backend->kernel->plan_runtime_event_batch(polled);
        if (!batch.accepted()) {
          session->status =
              batch.error.empty()
                  ? "runtime watch event batch was rejected"
                  : "runtime watch event batch was rejected: " + batch.error;
          result.disposition = RuntimeEventPumpDisposition::Failed;
          result.cursor_retained = true;
          result.error = session->status;
          break;
        }
        if (!batch.acknowledgement_cursor.has_value()) {
          session->status =
              "runtime watch event batch has no acknowledgement cursor";
          result.disposition = RuntimeEventPumpDisposition::Failed;
          result.cursor_retained = true;
          result.error = session->status;
          break;
        }
        plan = batch.plan;
        acknowledgement = *batch.acknowledgement_cursor;
      }
      const AutomaticPlanExecutionResult execution = execute_automatic_plan(
          session, backend, plan, show_running, progress);
      if (!execution.complete) {
        result.execution_complete = false;
      }

      if (resync && !execution.complete) {
        // Re-polling the old cursor will reproduce the gap/source-change and
        // retry a full conservative resync. A normal accepted event batch is
        // acknowledged after its plan has been processed even if a cell
        // failed: the error and stale barrier are already published, and
        // replaying the event forever would not repair user code.
        session->status = "runtime watch stream resync is incomplete";
        result.disposition = RuntimeEventPumpDisposition::RetryRequired;
        result.cursor_retained = true;
        result.error = session->status;
        break;
      }

      // Polling is read-only. Publish the successor only after invalidation
      // and execution have handled the complete batch; events emitted during
      // this run have later epochs and remain pending for the next round.
      backend->watch_cursor = acknowledgement;
      ++result.acknowledged_batches;
      if (resync) {
        ++result.resynchronized_batches;
      }
      result.disposition = RuntimeEventPumpDisposition::Acknowledged;
      if (resync) {
        session->status = "runtime watch stream resynchronized";
      } else if (!plan.empty()) {
        session->status = execution.complete
                              ? "runtime watch cells evaluated"
                              : "runtime watch cell evaluation failed";
      }
    } catch (const std::exception &error) {
      session->status =
          std::string("runtime watch event handling failed: ") + error.what();
      result.disposition = RuntimeEventPumpDisposition::Failed;
      result.execution_complete = false;
      result.cursor_retained = true;
      result.error = session->status;
      break;
    } catch (...) {
      session->status = "runtime watch event handling failed";
      result.disposition = RuntimeEventPumpDisposition::Failed;
      result.execution_complete = false;
      result.cursor_retained = true;
      result.error = session->status;
      break;
    }
  }
  result.drain_limit_reached =
      result.rounds == kMaxDrainRounds &&
      result.disposition == RuntimeEventPumpDisposition::Acknowledged;
  if (result.drain_limit_reached) {
    (void)session->activity_notifier().notify_runtime();
  }
  return result;
}

bool pump_runtime_events(Session *session, bool show_running,
                         const EvaluationProgress &progress) {
  return pump_runtime_events_detailed(session, show_running, progress)
      .handled();
}
