#include "frontend/lint/one_liners.h"
namespace sputnik::lint {
namespace {
bool contains(const ast::Expr &expr, const char *kind) {
  if (expr.kind == kind)
    return true;
  if (expr.kind == "AstBlock")
    return false;
  for (const auto &field : expr.node_fields)
    if (field.value && contains(*field.value, kind))
      return true;
  for (const auto &field : expr.list_fields)
    for (const auto &item : field.values)
      if (contains(*item, kind))
        return true;
  return false;
}
void visit(const ast::Expr &expr, std::vector<lexer::Diagnostic> &out) {
  if (expr.kind == "AstInlineIfExpr" && contains(expr, "AstThen") &&
      expr.span.start.line == expr.span.end.line)
    out.push_back(
        {"L1001", "warning", "lint",
         "then sequence inside an inline conditional; use a multiline body",
         expr.span});
  if (expr.kind == "AstThen" && expr.span.start.line == expr.span.end.line) {
    bool conditional = false, nested = false;
    std::size_t count = 0;
    for (const auto &field : expr.list_fields)
      for (const auto &item : field.values) {
        ++count;
        conditional = conditional || contains(*item, "AstInlineIfExpr");
        nested = nested || contains(*item, "AstThen");
      }
    if (conditional)
      out.push_back(
          {"L1001", "warning", "lint",
           "inline conditional combined with then; use a multiline body",
           expr.span});
    if (nested || count > 2)
      out.push_back({"L1002", "warning", "lint",
                     "nested or long then sequence; use one statement per line",
                     expr.span});
  }
  for (const auto &field : expr.node_fields)
    if (field.value)
      visit(*field.value, out);
  for (const auto &field : expr.list_fields)
    for (const auto &item : field.values)
      visit(*item, out);
}
} // namespace
std::vector<lexer::Diagnostic>
one_liners(const std::vector<std::unique_ptr<ast::Expr>> &items) {
  std::vector<lexer::Diagnostic> out;
  for (const auto &item : items)
    visit(*item, out);
  return out;
}
} // namespace sputnik::lint
