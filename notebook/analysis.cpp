#include "notebook/model.h"

#include "frontend/lexer/lexer.h"
#include "frontend/binder/binder.h"
#include "frontend/parser/parser.h"
#include "runtime/macro_expander.h"

#include <cstdint>
#include <utility>

namespace amber::notebook {
namespace {

const ast::Expr *node_field(const ast::Expr &expr, const char *name) {
  for (const ast::NodeField &field : expr.node_fields) {
    if (field.name == name) {
      return field.value.get();
    }
  }
  return nullptr;
}

const ast::ListField *list_field(const ast::Expr &expr, const char *name) {
  for (const ast::ListField &field : expr.list_fields) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

std::string string_field(const ast::Expr &expr, const char *name) {
  for (const ast::StringField &field : expr.string_fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  return {};
}

std::optional<std::string> binding_name(const ast::Expr &expr) {
  if (expr.kind == "AstName") {
    return string_field(expr, "name");
  }
  if (expr.kind == "AstIvar") {
    return "@" + string_field(expr, "name");
  }
  if (expr.kind == "AstCvar") {
    return "@@" + string_field(expr, "name");
  }
  return std::nullopt;
}

bool is_declaration(const ast::Expr &expr) {
  return expr.kind == "AstDefStmt" || expr.kind == "AstClauseDef" ||
         expr.kind == "AstClassDef" || expr.kind == "AstMixinDef" ||
         expr.kind == "AstPropDef" || expr.kind == "AstClassPropDef" ||
         expr.kind == "AstAttrDef";
}

std::string declaration_name(const ast::Expr &expr) {
  return string_field(expr, "name");
}

class DependencyScanner {
public:
  DependencyScanner(CellAnalysis *analysis, const binder::BindGraph &graph)
      : analysis_(analysis), graph_(graph) {}

  void scan(const ast::Expr &expr) {
    if (analysis_ == nullptr) {
      return;
    }
    if (expr.kind == "AstBlock") {
      ++block_depth_;
      for (const auto &field : expr.node_fields)
        if (field.value) scan(*field.value);
      for (const auto &field : expr.list_fields)
        for (const auto &child : field.values)
          if (child) scan(*child);
      --block_depth_;
      return;
    }

    // The parser wraps ordinary module statements in AstExprStmt.  It is a
    // transparent node for dependency purposes.
    if (expr.kind == "AstExprStmt") {
      if (const ast::Expr *value = node_field(expr, "expr")) {
        scan(*value);
      }
      return;
    }

    // Imports bind names in this cell's module scope. They do not read a
    // preceding notebook slot or publish an initialized notebook value.
    // Leave their constant-path resolution to the binder and HIR lowerer.
    if (expr.kind == "AstImportStmt") {
      if (string_field(expr, "import_kind") == "module") {
        auto alias = string_field(expr, "alias");
        if (alias.empty()) {
          const auto path = string_field(expr, "module_path");
          const auto dot = path.rfind('.');
          alias = dot == std::string::npos ? path : path.substr(dot + 1);
        }
        if (!alias.empty()) local_bindings_.insert(alias);
      } else if (const auto *names = list_field(expr, "names")) {
        for (const auto &name : names->values) {
          if (name) local_bindings_.insert(string_field(*name, "local_name"));
        }
      }
      return;
    }

    // Definitions create a top-level binding, but their function/class body
    // executes in another lexical scope and must not become a dependency of
    // this notebook cell.  Signature/default expressions are also intentionally
    // deferred here; the binder/runtime owns their exact evaluation rules.
    if (is_declaration(expr)) {
      const std::string name = declaration_name(expr);
      if (!name.empty()) {
        write(name);
      }
      return;
    }

    if (expr.kind == "AstAssign") {
      const ast::Expr *left = node_field(expr, "left");
      const ast::Expr *right = node_field(expr, "right");
      const std::string op = string_field(expr, "op");

      // The right hand side is evaluated before the target is assigned.  For
      // x = x + 1 this records the old x as an external dependency and then
      // publishes x@current-cell; it therefore cannot form a false self-edge.
      if (op != "=" && left != nullptr) {
        scan(*left);
      }
      if (right != nullptr) {
        scan(*right);
      }
      if (left != nullptr) {
        scan_write_target(*left);
      }
      return;
    }

    if (expr.kind == "AstPatternAssign") {
      if (const ast::Expr *right = node_field(expr, "right")) {
        scan(*right);
      }
      // Pattern assignments deliberately retain their source spelling in the
      // AST.  Lexing that spelling still uses the language lexer (and not a
      // regex), and handles the common tuple/array/star binding forms.  More
      // elaborate pattern semantics can later be exposed by a typed AST node.
      collect_pattern_writes(string_field(expr, "pattern"), expr.span);
      return;
    }

    // Kernel.watch(target) is a compiler intrinsic when Kernel has not been
    // shadowed in this cell. Its base name is not a notebook slot read; only
    // the target expression contributes dependencies. Keeping this decision
    // in the syntax scanner matches HIR lowering and prevents a spurious
    // missing input named "Kernel" in the persistent notebook graph.
    if (expr.kind == "AstPostfixChain") {
      const ast::Expr *base = node_field(expr, "base");
      const ast::ListField *tails = list_field(expr, "tails");
      if (base != nullptr && base->kind == "AstName" &&
          string_field(*base, "name") == "Kernel" &&
          local_bindings_.count("Kernel") == 0U && tails != nullptr &&
          tails->values.size() >= 2U && tails->values[0] != nullptr &&
          tails->values[0]->kind == "AstTailDotMember" &&
          string_field(*tails->values[0], "name") == "watch" &&
          tails->values[1] != nullptr &&
          tails->values[1]->kind == "AstTailCall") {
        if (const ast::ListField *args =
                list_field(*tails->values[1], "args")) {
          for (const std::unique_ptr<ast::Expr> &arg : args->values) {
            if (arg != nullptr) {
              scan(*arg);
            }
          }
        }
        for (std::size_t index = 2U; index < tails->values.size(); ++index) {
          if (tails->values[index] != nullptr) {
            scan(*tails->values[index]);
          }
        }
        return;
      }
    }

    if (std::optional<std::string> name = binding_name(expr)) {
      if (block_depth_ && block_local(expr, *name)) {
        analysis_->local_reads.insert(*name);
        return;
      }
      read(*name, expr.span);
      return;
    }

    // For every other AST node, recurse through both node and list fields.
    // This covers calls, operators, control expressions, interpolated strings,
    // and postfix chains while preserving the AST's source order.
    for (const ast::NodeField &field : expr.node_fields) {
      if (field.value != nullptr) {
        scan(*field.value);
      }
    }
    for (const ast::ListField &field : expr.list_fields) {
      for (const std::unique_ptr<ast::Expr> &child : field.values) {
        if (child != nullptr) {
          scan(*child);
        }
      }
    }
  }

private:
  bool block_local(const ast::Expr &expr, const std::string &name) const {
    for (const auto &ref : graph_.references) {
      if (!ref.resolved || ref.name != name ||
          ref.span.start.offset != expr.span.start.offset ||
          ref.span.end.offset != expr.span.end.offset) continue;
      for (const auto &binding : graph_.bindings) {
        if (binding.id == ref.binding_id && binding.scope_index >= 0)
          return graph_.scopes[binding.scope_index].kind != "module";
      }
    }
    return false;
  }
  void read(const std::string &name, const lexer::Span &span) {
    if (name.empty()) {
      return;
    }
    if (local_bindings_.count(name) != 0U) {
      analysis_->local_reads.insert(name);
    } else {
      analysis_->reads.insert(name);
      analysis_->external_read_refs.push_back({name, span});
    }
  }

  void write(const std::string &name) {
    if (name.empty() || block_depth_) {
      return;
    }
    analysis_->writes.insert(name);
    local_bindings_.insert(name);
  }

  void scan_write_target(const ast::Expr &target) {
    if (std::optional<std::string> name = binding_name(target)) {
      write(*name);
      return;
    }
    // Assignment to a member/index mutates its receiver and reads all of the
    // target expression.  It does not publish a top-level binding.
    scan(target);
  }

  void collect_pattern_writes(const std::string &pattern,
                              const lexer::Span &span) {
    if (pattern.empty()) {
      return;
    }
    lexer::Lexer lexer(pattern,
                       span.file.empty() ? "<notebook-pattern>" : span.file);
    lexer::LexResult result = lexer.lex();
    if (!result.ok()) {
      // The parser has already accepted the pattern.  Do not turn a future
      // pattern syntax extension into a spurious cell parse failure; simply
      // leave its bindings to the pattern binder for now.
      return;
    }
    const lexer::Token *previous = nullptr;
    for (const lexer::Token &token : result.tokens) {
      if (token.kind == lexer::TokenKind::Eof ||
          token.kind == lexer::TokenKind::Newline ||
          token.kind == lexer::TokenKind::Indent ||
          token.kind == lexer::TokenKind::Dedent) {
        continue;
      }
      if (token.kind == lexer::TokenKind::Identifier) {
        // A dotted identifier is a path/member pattern, not a newly bound
        // local.  Keys before ':' are likewise labels rather than bindings.
        bool member = previous != nullptr &&
                      (previous->kind == lexer::TokenKind::Dot ||
                       previous->kind == lexer::TokenKind::ChainDot);
        const lexer::Token *next = nullptr;
        const std::size_t index =
            static_cast<std::size_t>(&token - result.tokens.data());
        if (index + 1U < result.tokens.size()) {
          next = &result.tokens[index + 1U];
        }
        if (!member &&
            (next == nullptr || next->kind != lexer::TokenKind::Colon)) {
          write(token.lexeme);
        }
      }
      previous = &token;
    }
  }

  CellAnalysis *analysis_ = nullptr;
  const binder::BindGraph &graph_;
  unsigned block_depth_ = 0;
  std::set<std::string> local_bindings_;
};

} // namespace

CellAnalysis analyze_cell(const CellSource &cell) {
  CellAnalysis analysis;
  analysis.id = cell.id;
  analysis.file = cell.file.empty() ? "<notebook-cell>" : cell.file;

  lexer::Lexer lexer(cell.source, analysis.file);
  lexer::LexResult lex_result = lexer.lex();
  analysis.diagnostics = std::move(lex_result.diagnostics);
  if (!analysis.diagnostics.empty()) {
    return analysis;
  }

  parser::Parser parser(lex_result.tokens);
  parser::ParseModuleResult parse_result = parser.parse_module_unit();
  analysis.diagnostics = std::move(parse_result.diagnostics);
  if (!analysis.diagnostics.empty()) {
    return analysis;
  }

  const auto expansion = macros::expand_macros(
      parse_result.items, parse_result.module_name, cell.source);
  if (!expansion.ok) {
    analysis.diagnostics.push_back({"AMB_MACRO_EXPANSION", "error", "macro",
                                    expansion.error, {}});
    return analysis;
  }
  ast::expand_quotes(parse_result.items);
  analysis.items = std::move(parse_result.items);
  const auto bound = binder::bind_module(analysis.items, parse_result.module_name);
  DependencyScanner scanner(&analysis, bound.graph);
  for (const std::unique_ptr<ast::Expr> &item : analysis.items) {
    if (item != nullptr) {
      scanner.scan(*item);
    }
  }
  analysis.parsed = true;
  return analysis;
}

} // namespace amber::notebook
