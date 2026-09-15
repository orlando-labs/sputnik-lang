#pragma once

#include "frontend/ast/expr.h"
#include "frontend/lexer/token.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace amber::notebook {

// Cell IDs are document identities, not positions.  A client should allocate
// one once and keep it when the cell is moved, edited, or reloaded.
using CellId = std::uint64_t;

enum class CellMode {
  Manual,
  Watch,
};

struct CellSource {
  CellId id = 0;
  std::string source;
  std::string file;
  CellMode mode = CellMode::Manual;

  bool watch_enabled() const { return mode == CellMode::Watch; }
};

// A binding is versioned by its producing cell.  The name alone is not a
// stable identity in a notebook because editing/reordering creates new
// versions of a top-level binding.
struct BindingKey {
  CellId cell_id = 0;
  std::string name;

  bool operator==(const BindingKey &other) const {
    return cell_id == other.cell_id && name == other.name;
  }
  bool operator!=(const BindingKey &other) const { return !(*this == other); }
  bool operator<(const BindingKey &other) const {
    if (cell_id != other.cell_id) {
      return cell_id < other.cell_id;
    }
    return name < other.name;
  }
};

struct BindingKeyHash {
  std::size_t operator()(const BindingKey &key) const noexcept;
};

struct BindingRead {
  CellId consumer = 0;
  std::string name;
  std::optional<BindingKey> provider;

  bool operator==(const BindingRead &other) const {
    return consumer == other.consumer && name == other.name &&
           provider == other.provider;
  }
};

// A source-ordered external read. The name-only `reads` set is enough for
// dependency edges, but lowering needs the exact occurrence: a binding may be
// predeclared even when its first read precedes its write (`x = x + 1`).
struct ExternalReadRef {
  std::string name;
  lexer::Span span;

  bool operator==(const ExternalReadRef &other) const {
    return name == other.name && span.file == other.span.file &&
           span.start.offset == other.span.start.offset &&
           span.end.offset == other.span.end.offset;
  }
};

struct DependencyEdge {
  // Edges point from the provider to its consumer.  This is also the
  // direction used by execution_order() and dependents_of().
  BindingKey provider;
  CellId consumer = 0;

  bool operator==(const DependencyEdge &other) const {
    return provider == other.provider && consumer == other.consumer;
  }
};

struct CellAnalysis {
  CellId id = 0;
  std::string file;
  bool parsed = false;

  // These are external reads: a name read after an earlier assignment in the
  // same cell is local to that cell and is intentionally absent from reads.
  std::set<std::string> reads;
  std::set<std::string> local_reads;
  std::set<std::string> writes;
  std::vector<ExternalReadRef> external_read_refs;

  std::vector<lexer::Diagnostic> diagnostics;
  std::vector<std::unique_ptr<ast::Expr>> items;

  bool ok() const { return parsed && diagnostics.empty(); }
};

// Parse and collect top-level notebook dependencies from one cell.  The
// frontend lexer/parser are the source of truth; this function does not use a
// regular-expression scanner.
CellAnalysis analyze_cell(const CellSource &cell);

// Convenience allocator for document clients that do not persist their own
// IDs yet.  A generated ID is process-unique and never derived from a cell's
// current position.
CellId allocate_cell_id();
// Reserve persisted identities before allocating new cells. UINT64_MAX is
// reserved as an exhaustion sentinel; zero is never a valid identity.
void reserve_cell_id(CellId id);

} // namespace amber::notebook
