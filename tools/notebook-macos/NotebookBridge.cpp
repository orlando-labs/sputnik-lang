#include "tools/notebook-macos/NotebookBridge.h"
#include "tools/notebook-macos/NotebookExecution.h"
#include "notebook/renderers.h"

#include "notebook/project.h"
#include "tools/iamber/session.h"
#include "tools/iamber/tabs.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct AmberNotebook {
  explicit AmberNotebook(amber::notebook::LoadedProject project)
      : tabs(std::move(project)) {}

  ProjectTabs tabs;
  std::size_t runtime_cursor = 0;
  bool commands_started = false;
  std::unique_ptr<NotebookExecution> execution;
};
struct AmberNotebookControl { std::shared_ptr<NotebookExecutionControl> value; };

namespace {

using TabList = std::vector<std::unique_ptr<ProjectTab>>;

void append_replacement_character(std::string *out) {
  out->append("\xef\xbf\xbd", 3U);
}

// Return a UTF-8 sequence length, or zero for an invalid byte sequence.
std::size_t utf8_sequence_length(std::string_view text, std::size_t offset) {
  const auto byte = [&](std::size_t index) {
    return static_cast<unsigned char>(text[index]);
  };
  if (offset >= text.size())
    return 0U;
  const unsigned char first = byte(offset);
  if (first <= 0x7fU)
    return 1U;
  std::size_t length = 0U;
  if (first >= 0xc2U && first <= 0xdfU)
    length = 2U;
  else if (first >= 0xe0U && first <= 0xefU)
    length = 3U;
  else if (first >= 0xf0U && first <= 0xf4U)
    length = 4U;
  else
    return 0U;
  if (offset + length > text.size())
    return 0U;
  for (std::size_t index = 1U; index < length; ++index) {
    if ((byte(offset + index) & 0xc0U) != 0x80U)
      return 0U;
  }
  const unsigned char second = byte(offset + 1U);
  if ((first == 0xe0U && second < 0xa0U) ||
      (first == 0xedU && second >= 0xa0U) ||
      (first == 0xf0U && second < 0x90U) ||
      (first == 0xf4U && second >= 0x90U))
    return 0U;
  return length;
}

std::string valid_utf8(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t offset = 0; offset < text.size();) {
    const std::size_t length = utf8_sequence_length(text, offset);
    if (length == 0U) {
      append_replacement_character(&result);
      ++offset;
    } else {
      result.append(text.data() + offset, length);
      offset += length;
    }
  }
  return result;
}

void append_json_string(std::string *out, std::string_view text) {
  static constexpr char hex[] = "0123456789abcdef";
  out->push_back('"');
  for (std::size_t offset = 0; offset < text.size();) {
    const unsigned char byte = static_cast<unsigned char>(text[offset]);
    if (byte >= 0x80U) {
      const std::size_t length = utf8_sequence_length(text, offset);
      if (length == 0U) {
        out->append("\\ufffd");
        ++offset;
      } else {
        out->append(text.data() + offset, length);
        offset += length;
      }
      continue;
    }
    ++offset;
    switch (byte) {
    case '"':
      out->append("\\\"");
      break;
    case '\\':
      out->append("\\\\");
      break;
    case '\b':
      out->append("\\b");
      break;
    case '\f':
      out->append("\\f");
      break;
    case '\n':
      out->append("\\n");
      break;
    case '\r':
      out->append("\\r");
      break;
    case '\t':
      out->append("\\t");
      break;
    default:
      if (byte < 0x20U) {
        out->append("\\u00");
        out->push_back(hex[(byte >> 4U) & 0x0fU]);
        out->push_back(hex[byte & 0x0fU]);
      } else {
        out->push_back(static_cast<char>(byte));
      }
      break;
    }
  }
  out->push_back('"');
}

void append_bool(std::string *out, bool value) {
  out->append(value ? "true" : "false");
}

void append_output(std::string *out,
                   const std::vector<amber::runtime::RuntimeTextOutputEvent>
                       &events) {
  out->push_back('[');
  for (std::size_t index = 0; index < events.size(); ++index) {
    if (index != 0U)
      out->push_back(',');
    out->append("{\"stream\":");
    append_json_string(out, events[index].stream);
    out->append(",\"text\":");
    append_json_string(out, events[index].text);
    out->push_back('}');
  }
  out->push_back(']');
}

void set_error(char **destination, std::string_view message) noexcept {
  if (destination == nullptr)
    return;
  *destination = nullptr;
  try {
    const std::string safe = valid_utf8(message);
    char *copy = static_cast<char *>(std::malloc(safe.size() + 1U));
    if (copy == nullptr)
      return;
    if (!safe.empty())
      std::memcpy(copy, safe.data(), safe.size());
    copy[safe.size()] = '\0';
    *destination = copy;
  } catch (...) {
    // Error reporting is best effort if even the diagnostic allocation fails.
  }
}

char *copy_c_string(std::string_view value) {
  if (value.size() == std::numeric_limits<std::size_t>::max())
    throw std::bad_alloc();
  char *copy = static_cast<char *>(std::malloc(value.size() + 1U));
  if (copy == nullptr)
    throw std::bad_alloc();
  if (!value.empty())
    std::memcpy(copy, value.data(), value.size());
  copy[value.size()] = '\0';
  return copy;
}

std::string require_text(const char *text, const char *field) {
  if (text == nullptr)
    throw std::invalid_argument(std::string(field) + " is required");
  return text;
}

struct ParsedTabId {
  bool module = false;
  std::string id;
};

ParsedTabId parse_tab_id(const char *raw) {
  const std::string value = require_text(raw, "tab");
  if (value.rfind("sheet:", 0U) == 0U && value.size() > 6U)
    return {false, value.substr(6U)};
  if (value.rfind("module:", 0U) == 0U && value.size() > 7U)
    return {true, value.substr(7U)};
  throw std::invalid_argument("tab must be sheet:<id> or module:<id>");
}

std::size_t find_tab_index(const AmberNotebook &notebook,
                           const ParsedTabId &wanted) {
  const TabList &tabs = notebook.tabs.tabs();
  for (std::size_t index = 0; index < tabs.size(); ++index) {
    const ProjectTab &tab = *tabs[index];
    if (tab.module.has_value() != wanted.module)
      continue;
    if (wanted.module ? tab.module->id == wanted.id
                      : tab.session.project_sheet_id == wanted.id)
      return index;
  }
  throw std::invalid_argument("unknown tab: " +
                              std::string(wanted.module ? "module:" : "sheet:") +
                              wanted.id);
}

std::size_t find_cell_index(const Session &session, const char *raw) {
  const std::string value = require_text(raw, "cell");
  if (value.empty() ||
      !std::all_of(value.begin(), value.end(), [](char ch) {
        return ch >= '0' && ch <= '9';
      }))
    throw std::invalid_argument("cell must be a decimal cell ID");
  amber::notebook::CellId id = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), id);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
      id == 0U || std::to_string(id) != value)
    throw std::invalid_argument("cell must be a canonical decimal cell ID");
  for (std::size_t index = 0; index < session.cells.size(); ++index) {
    if (session.cells[index].id == id)
      return index;
  }
  throw std::invalid_argument("unknown or stale cell ID: " + value);
}

void switch_to_index(AmberNotebook *notebook, std::size_t index) {
  const std::size_t count = notebook->tabs.tabs().size();
  if (count == 0U || index >= count)
    throw std::out_of_range("tab index out of range");
  const std::size_t current = notebook->tabs.selected();
  if (current == index)
    return;
  const std::size_t forward = index >= current ? index - current
                                               : count - current + index;
  if (forward > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::out_of_range("tab count exceeds supported selection range");
  (void)notebook->tabs.switch_by(static_cast<int>(forward));
  if (notebook->tabs.selected() != index)
    throw std::runtime_error("could not select requested tab");
}

class SelectionRestore {
public:
  explicit SelectionRestore(AmberNotebook *notebook)
      : notebook_(notebook), selected_(notebook->tabs.selected()) {}
  ~SelectionRestore() {
    try {
      switch_to_index(notebook_, selected_);
    } catch (...) {
    }
  }
  SelectionRestore(const SelectionRestore &) = delete;
  SelectionRestore &operator=(const SelectionRestore &) = delete;

private:
  AmberNotebook *notebook_;
  std::size_t selected_;
};

std::string tab_id(const ProjectTab &tab) {
  return tab.module.has_value() ? "module:" + tab.module->id
                                : "sheet:" + tab.session.project_sheet_id;
}

std::string tab_title(const ProjectTab &tab,
                      const amber::notebook::LoadedProject &project) {
  if (tab.module.has_value())
    return tab.module->id;
  const auto found = std::find_if(
      project.document.sheets.begin(), project.document.sheets.end(),
      [&](const auto &sheet) { return sheet.id == tab.session.project_sheet_id; });
  return found == project.document.sheets.end() ? tab.session.project_sheet_id
                                                 : found->title;
}

bool module_auto_imported(const ProjectTab &tab,
                          const amber::notebook::LoadedProject &project) {
  if (!tab.module.has_value())
    return false;
  return std::find(project.document.auto_imports.begin(),
                   project.document.auto_imports.end(), tab.module->id) !=
         project.document.auto_imports.end();
}

std::string display_base64(const std::string &bytes) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve((bytes.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const unsigned a = static_cast<unsigned char>(bytes[i]);
    const unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
    const unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
    result += alphabet[a >> 2]; result += alphabet[((a & 3) << 4) | (b >> 4)];
    result += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
    result += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
  }
  return result;
}

void append_progress(std::string *out, const std::vector<amber::runtime::NotebookLiveEvent> &events) {
  out->push_back('[');
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (i) out->push_back(',');
    const auto &p = events[i];
    out->append("{\"id\":"); append_json_string(out, p.id);
    out->append(",\"description\":"); append_json_string(out, p.description);
    out->append(",\"current\":" + std::to_string(p.current));
    out->append(",\"total\":" + std::to_string(p.total));
    out->append(",\"elapsed_ms\":" + std::to_string(p.elapsed_ms));
    out->append(",\"done\":"); append_bool(out, p.done);
    out->push_back('}');
  }
  out->push_back(']');
}

void append_displays(std::string *out, const std::vector<amber::runtime::NotebookDisplay> &displays) {
  out->push_back('[');
  for (std::size_t i = 0; i < displays.size(); ++i) {
    if (i) out->push_back(',');
    const auto &display = displays[i];
    out->append("{\"mime\":"); append_json_string(out, display.mime);
    out->append(",\"caption\":"); append_json_string(out, display.caption);
    out->append(",\"width\":" + std::to_string(display.width));
    out->append(",\"height\":" + std::to_string(display.height));
    out->append(",\"data\":"); append_json_string(out, display_base64(display.bytes));
    out->append(",\"plot_scene\":"); append_json_string(out, display.plot_scene);
    out->push_back('}');
  }
  out->push_back(']');
}

void append_cell(std::string *out, const Cell &cell) {
  out->append("{\"id\":");
  append_json_string(out, std::to_string(cell.id));
  out->append(",\"kind\":");
  append_json_string(out, cell.kind);
  out->append(",\"source\":");
  append_json_string(out, cell.source);
  out->append(",\"formatting\":");
  append_json_string(out, cell.formatting);
  out->append(",\"watch\":");
  append_bool(out, cell.watch);
  out->append(",\"dirty\":");
  append_bool(out, cell.dirty);
  out->append(",\"ok\":");
  append_bool(out, cell.ok);
  out->append(",\"result\":");
  append_json_string(out, cell.result);
  out->append(",\"result_format\":{\"pretty\":");
  append_json_string(out, cell.result_format.pretty);
  out->append(",\"is_container\":"); append_bool(out, cell.result_format.is_container);
  out->append(",\"truncated\":"); append_bool(out, cell.result_format.truncated);
  out->append(",\"pretty_truncated\":"); append_bool(out, cell.result_format.pretty_truncated);
  out->append("}");
  out->append(",\"error\":");
  append_json_string(out, cell.error);
  out->append(",\"output\":");
  append_output(out, cell.output_events);
  out->append(",\"displays\":"); append_displays(out, cell.displays);
  out->append(",\"progress\":"); append_progress(out, cell.progress);
  out->append(",\"locals\":[");
  for (std::size_t index = 0; index < cell.locals.size(); ++index) {
    if (index != 0U)
      out->push_back(',');
    const LocalView &local = cell.locals[index];
    out->append("{\"name\":");
    append_json_string(out, local.name);
    out->append(",\"value\":");
    append_json_string(out, local.value);
    out->append(",\"text_value\":");
    append_json_string(out, local.text_value);
    out->append(",\"role\":");
    append_json_string(out, local.role);
    out->append(",\"binding_kind\":");
    append_json_string(out, local.binding_kind);
    out->append(",\"initialized\":");
    append_bool(out, local.initialized);
    out->append(",\"watched\":");
    append_bool(out, local.watched);
    out->push_back('}');
  }
  out->append("]}");
}

std::string snapshot_json(const AmberNotebook &notebook) {
  const amber::notebook::LoadedProject &project = notebook.tabs.project();
  const TabList &tabs = notebook.tabs.tabs();
  std::string out;
  out.reserve(2048U);
  out.append("{\"title\":");
  append_json_string(&out, project.document.title);
  out.append(",\"path\":");
  append_json_string(&out, project.directory.string());
  out.append(",\"selected_tab\":");
  append_json_string(&out, tab_id(*tabs[notebook.tabs.selected()]));
  out.append(",\"modified_count\":");
  out.append(std::to_string(notebook.tabs.modified_count()));
  out.append(",\"environment_generation\":");
  out.append(std::to_string(notebook.tabs.environment_generation()));
  out += ",\"dependencies\":[";
  for (std::size_t i = 0; i < project.document.dependencies.size(); ++i) {
    if (i) out += ',';
    out += "{\"path\":"; append_json_string(&out, project.document.dependencies[i].path);
    out += ",\"auto_import\":"; out += project.document.dependencies[i].auto_import ? "true" : "false";
    out += ",\"native\":"; out += project.document.dependencies[i].extra.count("native_library") ? "true}" : "false}";
  }
  out += ']';
  out.append(",\"inputs\":[");
  for (std::size_t i = 0; i < project.document.inputs.size(); ++i) {
    const auto &input = project.document.inputs[i]; if (i) out += ',';
    out += "{\"id\":"; append_json_string(&out, input.id);
    out += ",\"title\":"; append_json_string(&out, input.title);
    out += ",\"type\":"; append_json_string(&out, input.type);
    out += ",\"value_json\":";
    append_json_string(&out, amber::notebook::serialize_project_input_value(notebook.tabs.inputs().at(input.id)));
    if (input.minimum) out += ",\"minimum\":" + amber::notebook::serialize_project_input_value(*input.minimum);
    if (input.maximum) out += ",\"maximum\":" + amber::notebook::serialize_project_input_value(*input.maximum);
    out += '}';
  }
  out += "],\"boards\":[";
  for (std::size_t i = 0; i < project.document.boards.size(); ++i) {
    if (i) out += ',';
    out += amber::notebook::serialize_project_board(project.document.boards[i]);
  }
  out += ']';
  out.append(",\"tabs\":[");
  for (std::size_t index = 0; index < tabs.size(); ++index) {
    if (index != 0U)
      out.push_back(',');
    const ProjectTab &tab = *tabs[index];
    const Session &session = tab.session;
    out.append("{\"id\":");
    append_json_string(&out, tab_id(tab));
    out.append(",\"kind\":");
    append_json_string(&out, tab.module.has_value() ? "module" : "sheet");
    out.append(",\"title\":");
    append_json_string(&out, tab_title(tab, project));
    out.append(",\"modified\":");
    append_bool(&out, notebook.tabs.modified(index));
    out.append(",\"environment_stale\":");
    append_bool(&out, session.environment_stale);
    out.append(",\"auto_import\":");
    append_bool(&out, module_auto_imported(tab, project));
    out.append(",\"status\":");
    append_json_string(&out, session.status);
    out.append(",\"environment_error\":");
    append_json_string(&out, session.environment_error);
    out.append(",\"environment_output\":");
    append_output(&out, session.environment_output);
    out.append(",\"selected_cell\":");
    if (session.selected < session.cells.size())
      append_json_string(&out,
                         std::to_string(session.cells[session.selected].id));
    else
      append_json_string(&out, "");
    out.append(",\"cells\":[");
    for (std::size_t cell_index = 0; cell_index < session.cells.size();
         ++cell_index) {
      if (cell_index != 0U)
        out.push_back(',');
      append_cell(&out, session.cells[cell_index]);
    }
    out.append("]}");
  }
  out.append("]}");
  return out;
}

void clear_errors_from(Session *session, std::size_t index) {
  for (std::size_t cursor = index; cursor < session->cells.size(); ++cursor) {
    Cell &cell = session->cells[cursor];
    cell.error_ranges.clear();
    cell.errors.clear();
    cell.selected_error = 0U;
  }
}

void mark_source_edit(Session *session, std::size_t index,
                      const std::string &source) {
  Cell &cell = session->cells[index];
  cell.source = source;
  cell.cursor = source.size();
  cell.dirty = true;
  session->document_dirty = true;
  clear_errors_from(session, index);
  session->status = "cell edited";
}

void validate_editable_source(std::string_view source,
                              std::string_view description) {
  for (std::size_t offset = 0; offset < source.size();) {
    if (source[offset] == '\0')
      throw std::runtime_error(std::string(description) +
                               " contains an embedded NUL byte");
    const std::size_t length = utf8_sequence_length(source, offset);
    if (length == 0U)
      throw std::runtime_error(std::string(description) + " is not valid UTF-8");
    offset += length;
  }
}

void validate_project_sources(const AmberNotebook &notebook) {
  for (const auto &tab : notebook.tabs.tabs()) {
    for (const Cell &cell : tab->session.cells) {
      if (cell.kind == "code" || cell.kind == "text") {
        validate_editable_source(cell.source,
                                 tab_id(*tab) + " cell " +
                                     std::to_string(cell.id) + " source");
      }
    }
  }
}

void require_sheet_cell_editable(const ProjectTab &tab, const Cell &cell,
                                 std::string_view operation) {
  if (tab.module.has_value())
    throw std::invalid_argument(std::string(operation) +
                                " is not supported in a module editor");
  if (cell.kind != "code" && cell.kind != "text")
    throw std::invalid_argument("opaque cells are read-only");
}

void move_cell(Session *session, std::size_t index, bool up) {
  if (session->module_editor)
    throw std::invalid_argument("module editor has one source buffer");
  if (session->cells[index].kind != "code" && session->cells[index].kind != "text")
    throw std::invalid_argument("opaque cells cannot be moved");
  const std::size_t next = up ? (index == 0U ? 0U : index - 1U)
                              : std::min(index + 1U, session->cells.size() - 1U);
  if (next == index)
    throw std::invalid_argument(up ? "cell is already first" : "cell is already last");
  const bool code_order_changed = session->cells[index].kind == "code" &&
                                  session->cells[next].kind == "code";
  std::swap(session->cells[index], session->cells[next]);
  session->selected = next;
  session->document_dirty = true;
  for (Cell &cell : session->cells) {
    if (code_order_changed && cell.kind == "code")
      cell.dirty = true;
    if (code_order_changed) {
      cell.error_ranges.clear();
      cell.errors.clear();
      cell.selected_error = 0U;
    }
  }
  session->status = "cell moved";
}

void command_impl(AmberNotebook *notebook, const char *action_raw,
                  const char *tab_raw, const char *cell_raw,
                  const char *text_raw) {
  if (notebook == nullptr)
    throw std::invalid_argument("notebook handle is null");
  const std::string action = require_text(action_raw, "action");
  notebook->commands_started = true;
  if (notebook->execution && action == "set_input")
    throw std::runtime_error("Project input execution is not yet supported in worker preview.");
  if (action == "add_dependency" || action == "remove_dependency") {
    const auto path = require_text(text_raw, "dependency path");
    if (action == "add_dependency") notebook->tabs.add_dependency(path);
    else notebook->tabs.remove_dependency(path);
    return;
  }
  if (action == "set_input") {
    (void)notebook->tabs.set_input(require_text(cell_raw, "input id"), require_text(text_raw, "value JSON"));
    return;
  }
  if (action == "save_board") {
    notebook->tabs.save_board(amber::notebook::parse_project_board(require_text(text_raw, "board JSON")));
    return;
  }
  if (action == "new_input") {
    notebook->tabs.create_input(amber::notebook::parse_project_input(require_text(text_raw, "input JSON")));
    return;
  }

  if (action == "new_sheet") {
    notebook->tabs.create_sheet(require_text(text_raw, "text"));
    return;
  }
  if (action == "new_module") {
    notebook->tabs.create_module(require_text(text_raw, "text"));
    return;
  }
  if (action == "save_all") {
    SelectionRestore restore(notebook);
    const std::size_t original = notebook->tabs.selected();
    const std::size_t count = notebook->tabs.tabs().size();
    for (std::size_t offset = 0; offset < count; ++offset) {
      const std::size_t index = (original + offset) % count;
      if (!notebook->tabs.modified(index))
        continue;
      switch_to_index(notebook, index);
      notebook->tabs.save_active();
    }
    return;
  }

  const ParsedTabId target = parse_tab_id(tab_raw);
  const std::size_t tab_index = find_tab_index(*notebook, target);
  ProjectTab &tab = *notebook->tabs.tabs()[tab_index];
  Session &session = tab.session;

  if (action == "trust_native") {
    if (!notebook->execution) throw std::runtime_error("Native package execution requires an isolated worker.");
    notebook->execution->native_trusted = true;
    return;
  }

  if (notebook->execution && (action == "run" || action == "run_all" || action == "apply" || action == "restart_worker")) {
    if (action == "restart_worker") { notebook->execution->restart(tab); return; }
    const auto index = action == "run" ? find_cell_index(session, cell_raw) : 0;
    if (action == "run" && session.cells[index].kind != "code")
      throw std::invalid_argument("Run applies only to code cells");
    notebook->execution->run(notebook->tabs, tab, action, index);
    return;
  }

  if (action == "select") {
    const std::optional<std::size_t> selected_cell =
        cell_raw == nullptr ? std::nullopt
                            : std::optional<std::size_t>(
                                  find_cell_index(session, cell_raw));
    switch_to_index(notebook, tab_index);
    if (selected_cell.has_value())
      session.selected = *selected_cell;
    return;
  }

  if (action == "add_cell" || action == "add_text") {
    if (tab.module.has_value())
      throw std::invalid_argument("module editor has one source buffer");
    std::size_t insert_at = 0U;
    if (session.cells.empty()) {
      if (cell_raw != nullptr && cell_raw[0] != '\0')
        throw std::invalid_argument("empty sheet has no cell anchor");
    } else {
      if (cell_raw == nullptr || cell_raw[0] == '\0')
        throw std::invalid_argument("add_cell requires an after-cell ID");
      insert_at = find_cell_index(session, cell_raw) + 1U;
    }
    Cell added;
    added.id = amber::notebook::allocate_cell_id();
    if (action == "add_text") {
      added.kind = "text";
      added.watch = false;
      added.dirty = false;
      added.result.clear();
    }
    session.cells.insert(session.cells.begin() +
                             static_cast<std::ptrdiff_t>(insert_at),
                         std::move(added));
    session.selected = insert_at;
    session.document_dirty = true;
    if (action == "add_cell")
      clear_errors_from(&session, 0U);
    session.status = action == "add_text" ? "new text block" : "new cell";
    return;
  }

  if (action == "source" || action == "rich_source" || action == "watch" || action == "run" ||
      action == "delete_cell" || action == "move_up" ||
      action == "move_down") {
    const std::size_t cell_index = find_cell_index(session, cell_raw);
    Cell &cell = session.cells[cell_index];
    if (action == "rich_source") {
      if (tab.module.has_value() || cell.kind != "text")
        throw std::invalid_argument("rich_source requires a text block");
      auto content = amber::notebook::parse_project_text_content(require_text(text_raw, "text"));
      validate_editable_source(content.source, "text block");
      // Publish plain source and its matching attribute ranges together.
      cell.source.swap(content.source);
      cell.formatting.swap(content.formatting);
      session.document_dirty = true;
      session.status = "text block edited";
      return;
    }
    if (action == "source") {
      if (cell.kind == "text")
        throw std::invalid_argument("text blocks require a rich_source payload");
      const std::string source = require_text(text_raw, "text");
      validate_editable_source(source, "source edit");
      if (tab.module.has_value()) {
        if (session.cells.size() != 1U)
          throw std::runtime_error("module editor must contain one source cell");
        mark_source_edit(&session, cell_index, source);
      } else {
        require_sheet_cell_editable(tab, cell, action);
        mark_source_edit(&session, cell_index, source);
      }
      return;
    }
    if (action == "watch") {
      require_sheet_cell_editable(tab, cell, action);
      if (cell.kind != "code")
        throw std::invalid_argument("Watch applies only to code cells");
      const std::string setting = require_text(text_raw, "text");
      if (setting != "on" && setting != "off")
        throw std::invalid_argument("watch text must be on or off");
      cell.watch = setting == "on";
      session.document_dirty = true;
      session.status = cell.watch ? "object.watch enabled"
                                  : "object.watch disabled";
      return;
    }
    if (action == "run") {
      if (cell.kind != "code")
        throw std::invalid_argument("Run applies only to code cells");
      struct SelectedCellRestore {
        Session &session;
        std::size_t selected;
        ~SelectedCellRestore() { session.selected = selected; }
      } restore{session, session.selected};
      session.selected = cell_index;
      evaluate_from_with_progress(&session, cell_index, false, false, {});
      return;
    }
    if (action == "delete_cell") {
      require_sheet_cell_editable(tab, cell, action);
      const std::size_t code_count = static_cast<std::size_t>(std::count_if(
          session.cells.begin(), session.cells.end(),
          [](const Cell &candidate) { return candidate.kind == "code"; }));
      const bool removed_code = cell.kind == "code";
      if (removed_code && code_count == 1U) {
        Cell replacement;
        replacement.id = amber::notebook::allocate_cell_id();
        session.cells[cell_index] = std::move(replacement);
        session.selected = cell_index;
      } else {
        session.cells.erase(session.cells.begin() +
                            static_cast<std::ptrdiff_t>(cell_index));
        if (session.selected > cell_index)
          --session.selected;
        else if (session.selected == cell_index)
          session.selected = session.cells.empty() ? 0U : std::min(cell_index, session.cells.size() - 1U);
      }
      session.document_dirty = true;
      if (removed_code)
        clear_errors_from(&session, 0U);
      session.status = "cell deleted";
      return;
    }
    if (action == "move_up" || action == "move_down") {
      move_cell(&session, cell_index, action == "move_up");
      return;
    }
  }

  if (action == "run_all") {
    if (cell_raw != nullptr)
      (void)find_cell_index(session, cell_raw);
    evaluate_from_with_progress(&session, 0U, true, false, {});
    return;
  }
  if (action == "apply") {
    if (target.module)
      throw std::invalid_argument("Apply requires a sheet tab");
    if (cell_raw != nullptr)
      (void)find_cell_index(session, cell_raw);
    SelectionRestore restore(notebook);
    switch_to_index(notebook, tab_index);
    (void)notebook->tabs.apply_active(false, false, {});
    return;
  }
  if (action == "save") {
    if (cell_raw != nullptr)
      (void)find_cell_index(session, cell_raw);
    SelectionRestore restore(notebook);
    switch_to_index(notebook, tab_index);
    notebook->tabs.save_active();
    return;
  }
  if (action == "auto_import") {
    if (!target.module)
      throw std::invalid_argument("auto_import requires a module tab");
    const std::string setting = require_text(text_raw, "text");
    if (setting != "on" && setting != "off")
      throw std::invalid_argument("auto_import text must be on or off");
    SelectionRestore restore(notebook);
    switch_to_index(notebook, tab_index);
    (void)notebook->tabs.set_auto_import(target.id, setting == "on");
    return;
  }
  throw std::invalid_argument("unknown notebook action: " + action);
}

void report_current_exception(char **error) noexcept {
  try {
    throw;
  } catch (const std::exception &exception) {
    set_error(error, exception.what());
  } catch (...) {
    set_error(error, "unknown native notebook error");
  }
}

} // namespace

extern "C" {

AmberNotebook *amber_notebook_open(const char *path, int create,
                                  const char *title, char **error) {
  if (error != nullptr)
    *error = nullptr;
  try {
    amber::notebook::register_builtin_renderers();
    if (path == nullptr || path[0] == '\0')
      throw std::invalid_argument("project path is required");
    amber::notebook::LoadedProject project;
    if (create != 0) {
      std::string project_title = title == nullptr ? "Amber Notebook" : title;
      if (project_title.empty())
        project_title = "Amber Notebook";
      project = amber::notebook::create_project(
          path, amber::notebook::make_project_document(std::move(project_title)));
    } else {
      project = amber::notebook::load_project(path);
    }
    auto notebook = std::make_unique<AmberNotebook>(std::move(project));
    validate_project_sources(*notebook);
    return notebook.release();
  } catch (...) {
    report_current_exception(error);
    return nullptr;
  }
}

void amber_notebook_destroy(AmberNotebook *notebook) {
  try {
    delete notebook;
  } catch (...) {
  }
}

void amber_notebook_string_free(char *value) {
  try {
    std::free(value);
  } catch (...) {
  }
}

AmberNotebookControl *amber_notebook_enable_worker(AmberNotebook *notebook, const char *executable, char **error) {
  if (error) *error = nullptr;
  try {
    if (!notebook || notebook->commands_started || notebook->execution)
      throw std::runtime_error("Worker preview must be configured once immediately after open.");
    const auto path = require_text(executable, "worker executable");
    if (path.empty() || path.front() != '/') throw std::runtime_error("Worker executable must be an absolute path.");
    auto execution = std::make_unique<NotebookExecution>(path);
    auto control = std::make_unique<AmberNotebookControl>(AmberNotebookControl{execution->control});
    notebook->execution = std::move(execution);
    return control.release();
  } catch (...) { report_current_exception(error); return nullptr; }
}
void amber_notebook_control_destroy(AmberNotebookControl *control) { delete control; }
int amber_notebook_control_phase(AmberNotebookControl *control) {
  return control ? control->value->phase() : 0;
}
int amber_notebook_control_stop(AmberNotebookControl *control, int force) {
  try { return control && control->value->stop(force != 0) ? 1 : 0; }
  catch (...) { return 0; }
}

char *amber_notebook_control_live(AmberNotebookControl *control) {
  try {
    if (!control) return nullptr;
    const auto value = control->value->take_live();
    if (!value) return nullptr;
    std::string json = "{\"cells\":[";
    for (std::size_t i = 0; i < value->cells.size(); ++i) {
      if (i) json += ',';
      const auto &cell = value->cells[i];
      json += "{\"id\":"; append_json_string(&json, std::to_string(cell.id));
      json += ",\"progress\":"; append_progress(&json, cell.progress);
      json += ",\"displays\":"; append_displays(&json, cell.displays);
      json += '}';
    }
    json += "]}";
    return copy_c_string(json);
  } catch (...) { return nullptr; }
}

char *amber_notebook_snapshot(AmberNotebook *notebook, char **error) {
  if (error != nullptr)
    *error = nullptr;
  try {
    if (notebook == nullptr)
      throw std::invalid_argument("notebook handle is null");
    return copy_c_string(snapshot_json(*notebook));
  } catch (...) {
    report_current_exception(error);
    return nullptr;
  }
}

int amber_notebook_command(AmberNotebook *notebook, const char *action,
                           const char *tab, const char *cell,
                           const char *text, char **error) {
  if (error != nullptr)
    *error = nullptr;
  try {
    command_impl(notebook, action, tab, cell, text);
    return 1;
  } catch (...) {
    report_current_exception(error);
    return 0;
  }
}

int amber_notebook_activity_descriptor(AmberNotebook *notebook,
                                        char **error) {
  if (error != nullptr)
    *error = nullptr;
  try {
    if (notebook == nullptr)
      throw std::invalid_argument("notebook handle is null");
    return notebook->tabs.activity_waiter().poll_descriptor();
  } catch (...) {
    report_current_exception(error);
    return -1;
  }
}

int amber_notebook_pump(AmberNotebook *notebook, int *next_delay_ms,
                        char **error) {
  if (error != nullptr)
    *error = nullptr;
  if (next_delay_ms != nullptr)
    *next_delay_ms = -1;
  try {
    if (notebook == nullptr)
      throw std::invalid_argument("notebook handle is null");

    if (notebook->execution) return 1; // No hidden local evaluation in worker mode.
    const SessionActivityResult hints =
        notebook->tabs.activity_waiter().wait(std::chrono::milliseconds(0));
    if (hints.shutdown)
      throw std::runtime_error("notebook activity mailbox is closed");
    if (hints.runtime_ready || hints.input_ready) {
      for (const auto &tab : notebook->tabs.tabs()) {
        const SessionActivityResult local =
            tab->activity.wait(std::chrono::milliseconds(0));
        if (local.runtime_ready)
          tab->runtime_dispatch.notify_runtime();
      }
    }

    const std::size_t count = notebook->tabs.tabs().size();
    const auto now = RuntimePumpDispatch::Clock::now();
    for (std::size_t offset = 0; offset < count; ++offset) {
      const std::size_t index = (notebook->runtime_cursor + offset) % count;
      ProjectTab &tab = *notebook->tabs.tabs()[index];
      if (!tab.runtime_dispatch.due(now))
        continue;
      const RuntimeEventPumpResult result =
          pump_runtime_events_detailed(&tab.session, false, {});
      tab.runtime_dispatch.complete(result, RuntimePumpDispatch::Clock::now());
      notebook->runtime_cursor = (index + 1U) % count;
      break;
    }

    std::optional<std::chrono::milliseconds> next;
    const auto next_now = RuntimePumpDispatch::Clock::now();
    for (const auto &tab : notebook->tabs.tabs()) {
      const auto delay = tab->runtime_dispatch.wait_timeout(next_now);
      if (delay.has_value() && (!next.has_value() || *delay < *next))
        next = delay;
    }
    if (next_delay_ms != nullptr && next.has_value()) {
      const auto bounded = std::min<std::int64_t>(
          std::max<std::int64_t>(0, next->count()),
          std::numeric_limits<int>::max());
      *next_delay_ms = static_cast<int>(bounded);
    }
    return 1;
  } catch (...) {
    report_current_exception(error);
    return 0;
  }
}

} // extern "C"
