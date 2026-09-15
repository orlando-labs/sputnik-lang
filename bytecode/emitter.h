#pragma once

#include "bytecode/format.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/token.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace amber::bytecode {

struct EmitResult {
  BcModule module;
  std::vector<lexer::Diagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

EmitResult emit_program(const hir::Program &program,
                        const std::string &module_name);

struct NotebookEmitResult {
  std::size_t code_index = 0;
  std::vector<lexer::Diagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

// Persistent emitter for a single in-memory image. Constant, symbol, line,
// and code tables are shared by every appended cell; compiling each cell with
// emit_program() would create module-local IDs that cannot safely be merged.
class NotebookEmitter {
public:
  NotebookEmitter();
  // Seed the persistent runtime name ABI before compiling a replacement
  // notebook image. Existing Values encode string/symbol table indices, so a
  // hot-edit image must retain the active tables as an exact prefix and only
  // append newly interned names.
  NotebookEmitter(std::vector<std::string> string_seed,
                  std::vector<std::string> symbol_seed);
  // Starts a composite generation from an already verified ordinary module.
  // Existing code/classes/init/exports remain in the same immutable image as
  // subsequently appended NotebookCell code; no runtime Values cross worlds.
  explicit NotebookEmitter(BcModule base_module);
  ~NotebookEmitter();
  NotebookEmitter(const NotebookEmitter &) = delete;
  NotebookEmitter &operator=(const NotebookEmitter &) = delete;
  NotebookEmitter(NotebookEmitter &&) noexcept;
  NotebookEmitter &operator=(NotebookEmitter &&) noexcept;

  NotebookEmitResult append_cell(const hir::Procedure &procedure,
                                 std::uint32_t code_id,
                                 const std::string &module_name = {},
                                 const std::vector<hir::Procedure> &blocks = {});
  void reserve_code_ids(const std::vector<std::uint32_t> &ids);
  BcModule &module();
  const BcModule &module() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace amber::bytecode
