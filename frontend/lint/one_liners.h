#pragma once
#include "frontend/ast/expr.h"
namespace sputnik::lint {
std::vector<lexer::Diagnostic>
one_liners(const std::vector<std::unique_ptr<ast::Expr>> &items);
}
