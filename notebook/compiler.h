#pragma once

#include "bytecode/emitter.h"
#include "frontend/lexer/token.h"
#include "notebook/dependency_graph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace amber::notebook {

enum class NotebookCellSlotDirection { Input, Output };

// Descriptor identity is deliberately separate from code_id identity. A
// descriptor refers to a versioned BindingKey and is consumed by the runtime
// slot callbacks, while code_id identifies a code object in one image.
struct NotebookImageDescriptor {
  std::uint32_t id = 0;
  BindingKey key;
  std::string name;
};

// Direction belongs to one cell use, not to the globally interned descriptor:
// a producer uses the descriptor as an output while every consumer uses the
// same BindingKey descriptor as an input.
struct CellSlotUse {
  NotebookImageDescriptor descriptor;
  NotebookCellSlotDirection direction = NotebookCellSlotDirection::Input;
};

struct NotebookCompileOptions;
struct CellCompileResult;

// Build owner for one session image. Compile cells into this object before
// taking snapshot(). A RuntimeWorld constructed from a snapshot owns an
// immutable BcModule; later edits to this builder are not visible to it.
class NotebookImage {
public:
  NotebookImage() = default;
  NotebookImage(std::vector<std::string> string_seed,
                std::vector<std::string> symbol_seed);
  // Starts a fresh composite generation from one already verified ordinary
  // module image. Runtime hot-install still accepts notebook-only images;
  // hosts using this constructor must create a fresh RuntimeWorld generation.
  explicit NotebookImage(bytecode::BcModule base_module);
  NotebookImage(const NotebookImage &) = delete;
  NotebookImage &operator=(const NotebookImage &) = delete;
  NotebookImage(NotebookImage &&) noexcept = default;
  NotebookImage &operator=(NotebookImage &&) noexcept = default;

  const bytecode::BcModule &module() const { return emitter_.module(); }
  std::shared_ptr<const bytecode::BcModule> snapshot() const;
  void reserve_code_ids(const std::vector<std::uint32_t> &ids);

  const std::vector<NotebookImageDescriptor> &descriptors() const {
    return descriptors_;
  }
  std::optional<std::uint32_t> descriptor_for(const BindingKey &key) const;

private:
  std::uint32_t intern_descriptor(const BindingKey &key,
                                  const std::string &name);
  std::uint32_t allocate_code_id();
  bool code_id_in_use(std::uint32_t code_id) const;
  void discard_code(std::uint32_t code_id);

  bytecode::NotebookEmitter emitter_;
  std::vector<NotebookImageDescriptor> descriptors_;
  std::unordered_map<BindingKey, std::uint32_t, BindingKeyHash> descriptor_ids_;
  std::vector<bytecode::NotebookCellEntry> cell_entries_;
  std::uint32_t next_descriptor_id_ = 0;
  std::uint32_t next_code_id_ = 1;

  friend CellCompileResult compile_cell(const CellSource &,
                                        const DependencyGraph &,
                                        NotebookImage &,
                                        const NotebookCompileOptions &);
};

struct NotebookCompileOptions {
  // If omitted, the image allocator reserves a fresh id. Explicit IDs are
  // useful when replacing a cell in a pre-existing image, but must be unique
  // among the image's current code objects.
  std::optional<std::uint32_t> code_id;
  std::string module_name;

  // Names exported by a bundled/project module environment.  An unresolved
  // read whose name is present here lowers to LookupConst on the mapped
  // qualified path (for example, `A` -> `models.A`) and is not represented as
  // a notebook input slot.  A preceding notebook provider always wins over
  // this map.
  std::unordered_map<std::string, std::string> ambient_constant_paths;
  std::map<std::string, std::map<std::string, std::string>> import_paths;
};

struct CellCompileResult {
  bool ok = false;
  std::uint32_t code_id = 0;
  std::size_t code_index = 0;
  std::vector<lexer::Diagnostic> diagnostics;
  std::vector<CellSlotUse> inputs;
  std::vector<CellSlotUse> outputs;
};

CellCompileResult compile_cell(const CellSource &cell,
                               const DependencyGraph &graph,
                               NotebookImage &image,
                               const NotebookCompileOptions &options = {});

} // namespace amber::notebook
