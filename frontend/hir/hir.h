#pragma once

#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sputnik::hir {

struct ProcedureLocal {
  std::string slot;
  std::string name;
  std::string role;
  std::string binding_kind;
  lexer::Span span;
};

struct ProcedureCapture {
  std::string slot;
  std::string name;
  std::string source_kind;
  std::string source_slot;
  std::string source_name;
  lexer::Span span;
};

struct Procedure {
  std::string id;
  std::string name;
  std::string kind;
  std::string owner;
  // A call-site block returns through the concrete activation which created
  // it. Standalone lambdas keep ordinary local-return semantics.
  bool nonlocal_return_block = false;
  bool needs_nonlocal_return_target = false;
  lexer::Span span;
  std::unique_ptr<ast::Expr> signature;
  std::vector<std::unique_ptr<ast::Expr>> param_patterns;
  std::vector<ProcedureLocal> locals;
  std::vector<ProcedureCapture> captures;
  std::unique_ptr<ast::Expr> body;
};

struct Program {
  std::unique_ptr<ast::Expr> root;
  std::vector<Procedure> procedures;
};

// Notebook cells have a separate binding boundary from ordinary module
// procedures. The callbacks are keyed by source occurrence for reads because
// the binder predeclares module assignments (`x = x + 1` must still load the
// previous notebook version), and by name for the current cell's published
// writes.
struct NotebookLoweringOptions {
  // Only verified exports of modules linked into this notebook generation.
  const std::map<std::string, std::map<std::string, std::string>> *import_paths = nullptr;
  std::function<std::optional<std::uint32_t>(const std::string &,
                                             const lexer::Span &)>
      external_read_descriptor;
  // A notebook cell can resolve an otherwise-unbound read from an ambient
  // module/export environment.  The compiler supplies the fully qualified
  // constant path (for example, `models.A`) so emission uses LookupConst
  // rather than allocating a notebook slot for that read.
  std::function<std::optional<std::string>(const std::string &,
                                            const lexer::Span &)>
      external_read_constant_path;
  std::function<std::optional<std::uint32_t>(const std::string &)>
      declared_write_descriptor;
};

struct NotebookLoweringResult {
  std::unique_ptr<Procedure> procedure;
  std::vector<Procedure> blocks;
  std::vector<lexer::Diagnostic> diagnostics;

  bool ok() const { return procedure != nullptr && diagnostics.empty(); }
};

Program lower_module(const std::vector<std::unique_ptr<ast::Expr>> &items,
                     const std::string &module_name,
                     const binder::BindGraph &bind_graph);

NotebookLoweringResult
lower_notebook_cell(const std::vector<std::unique_ptr<ast::Expr>> &items,
                    const std::string &module_name,
                    const binder::BindGraph &bind_graph,
                    const NotebookLoweringOptions &options);

std::string program_to_json(const Program &program,
                            const std::string &module_name,
                            const std::string &source_hash);

} // namespace sputnik::hir
