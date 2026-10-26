#include "frontend/templates/templates.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace sputnik::templates {
namespace {
const ast::Expr *node(const ast::Expr &expr, const std::string &name) {
  for (const auto &field : expr.node_fields)
    if (field.name == name)
      return field.value.get();
  return nullptr;
}
const ast::ListField *list(const ast::Expr &expr, const std::string &name) {
  for (const auto &field : expr.list_fields)
    if (field.name == name)
      return &field;
  return nullptr;
}
std::string str(const ast::Expr &expr, const std::string &name) {
  for (const auto &field : expr.string_fields)
    if (field.name == name)
      return field.value;
  return {};
}
const ast::Expr *annotation(const ast::Expr &stmt) {
  const auto *expr = stmt.kind == "AstExprStmt" ? node(stmt, "expr") : &stmt;
  if (!expr || expr->kind != "AstPostfixChain")
    return nullptr;
  const auto *base = node(*expr, "base");
  return base && base->kind == "AstIvar" && str(*base, "name") == "template"
             ? expr
             : nullptr;
}
std::string literal_path(const ast::Expr &expr) {
  const auto *tails = list(expr, "tails");
  if (!tails || tails->values.size() != 1 ||
      tails->values[0]->kind != "AstTailCall")
    return {};
  const auto &call = *tails->values[0];
  const auto *args = list(call, "args");
  const auto *kwargs = list(call, "kwargs");
  if (!args || args->values.size() != 1 || (kwargs && !kwargs->values.empty()))
    return {};
  const auto &arg = *args->values[0];
  if (arg.kind == "AstLiteral" && str(arg, "token") == "STRING") {
    auto text = str(arg, "value");
    if (text.size() >= 2)
      return text.substr(1, text.size() - 2);
  }
  if (arg.kind == "AstStringLiteral") {
    std::string result;
    const auto *parts = list(arg, "parts");
    if (!parts)
      return {};
    for (const auto &part : parts->values) {
      if (part->kind != "AstStringText" && part->kind != "AstStringEscape")
        return {};
      result += str(*part, "value");
    }
    return result;
  }
  return {};
}
std::vector<std::unique_ptr<ast::Expr>> imports(const std::string &module,
                                                const std::string &macro,
                                                const std::string &alias,
                                                const std::string &file) {
  lexer::Lexer lexer("import " + module + " as " + alias + "_provider\nfrom " +
                         module + " import " + macro + " as " + alias + "\n",
                     file);
  auto tokens = lexer.lex();
  parser::Parser parser(tokens.tokens);
  auto parsed = parser.parse_module_unit();
  if (!tokens.ok() || !parsed.ok() || parsed.items.size() != 2 ||
      parsed.items[0]->kind != "AstImportStmt" ||
      parsed.items[1]->kind != "AstImportStmt" ||
      str(*parsed.items[0], "module_path") != module ||
      str(*parsed.items[1], "module_path") != module)
    throw std::runtime_error("invalid source-format provider reference");
  const auto *names = list(*parsed.items[1], "names");
  if (!names || names->values.size() != 1 ||
      str(*names->values[0], "source_name") != macro ||
      str(*names->values[0], "local_name") != alias)
    throw std::runtime_error("invalid source-format macro name");
  return std::move(parsed.items);
}
void reject_unattached(const ast::Expr &expr, PrepareResult &result) {
  if (annotation(expr)) {
    result.diagnostics.push_back(
        {"TEMPLATE_DECLARATION", "error", "templates",
         "@template must directly precede a declaration-only module function",
         expr.span});
    return;
  }
  for (const auto &field : expr.node_fields)
    if (field.value)
      reject_unattached(*field.value, result);
  for (const auto &field : expr.list_fields)
    for (const auto &child : field.values)
      if (child)
        reject_unattached(*child, result);
}
} // namespace

PrepareResult prepare(std::vector<std::unique_ptr<ast::Expr>> &items,
                      const std::string &declaring_file,
                      const std::string &declaring_source,
                      const std::vector<SourceFormat> &formats,
                      const SourceLoader &loader) {
  PrepareResult result;
  std::set<std::string> suffixes;
  for (const auto &format : formats) {
    if (format.suffix.empty() || format.suffix.front() != '.' ||
        format.module.empty() || format.macro.empty() ||
        !suffixes.insert(format.suffix).second) {
      result.diagnostics.push_back(
          {"TEMPLATE_FORMAT",
           "error",
           "templates",
           "source formats require unique dot-prefixed suffixes and "
           "module/macro names",
           {declaring_file, {}, {}}});
    }
    result.cache_material += "format\n" + format.suffix + "\n" + format.module +
                             "\n" + format.macro + "\n";
  }
  if (!result.ok())
    return result;
  std::vector<std::unique_ptr<ast::Expr>> added_imports;
  std::size_t serial = 0;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto *call = annotation(*items[i]);
    if (!call)
      continue;
    const auto fail = [&](const std::string &code, const std::string &message) {
      result.diagnostics.push_back(
          {code, "error", "templates", message, call->span});
    };
    const auto path = literal_path(*call);
    if (path.empty() || path.find('\0') != std::string::npos) {
      fail("TEMPLATE_PATH", "@template requires one nonempty literal path");
      continue;
    }
    if (i + 1 >= items.size() || items[i + 1]->kind != "AstDefStmt" ||
        items[i + 1]->span.start.line != items[i]->span.end.line + 1) {
      fail(
          "TEMPLATE_DECLARATION",
          "@template must directly precede a declaration-only module function");
      continue;
    }
    auto &decl = *items[i + 1];
    auto *body = const_cast<ast::ListField *>(list(decl, "body"));
    const bool compile_only =
        std::any_of(decl.bool_fields.begin(), decl.bool_fields.end(),
                    [](const auto &field) {
                      return field.value && (field.name == "is_macro" ||
                                             field.name == "is_native");
                    });
    if (!body || !body->values.empty() || compile_only ||
        !str(decl, "native_binding").empty()) {
      fail("TEMPLATE_DECLARATION",
           "@template requires a function with no body");
      continue;
    }
    const SourceFormat *format = nullptr;
    for (const auto &candidate : formats)
      if (path.size() >= candidate.suffix.size() &&
          path.compare(path.size() - candidate.suffix.size(),
                       candidate.suffix.size(), candidate.suffix) == 0 &&
          (!format || candidate.suffix.size() > format->suffix.size()))
        format = &candidate;
    if (!format) {
      fail("TEMPLATE_FORMAT",
           "no registered source format for '" + path +
               "'; declare source_formats in the build manifest");
      continue;
    }
    result.providers.push_back({*format, call->span});
    try {
      const auto input = loader(declaring_file, path);
      result.sources[input.path] = input.source;
      result.cache_material += "input\n" + input.path + "\n" +
                               lexer::sha256_hex(input.source) + "\n";
      lexer::Lexer lexer(input.source, input.path);
      auto lexed = lexer.lex_template_body();
      if (!lexed.ok()) {
        result.diagnostics.insert(result.diagnostics.end(),
                                  lexed.diagnostics.begin(),
                                  lexed.diagnostics.end());
        continue;
      }
      parser::Parser parser(lexed.tokens);
      auto parsed = parser.parse_template_body();
      if (!parsed.ok()) {
        result.diagnostics.insert(result.diagnostics.end(),
                                  parsed.diagnostics.begin(),
                                  parsed.diagnostics.end());
        continue;
      }
      std::string alias;
      do {
        alias = "__external_template_" + std::to_string(++serial);
      } while (declaring_source.find(alias) != std::string::npos);
      auto provider_imports =
          imports(format->module, format->macro, alias, declaring_file);
      for (auto &entry : provider_imports)
        added_imports.push_back(std::move(entry));
      auto base = ast::make_expr("AstName", parsed.expr->span);
      base->span.end = base->span.start;
      base->string_field("name", alias);
      auto chain = ast::make_expr("AstPostfixChain", parsed.expr->span);
      chain->node_field("base", std::move(base));
      auto tail = ast::make_expr("AstTailCall", parsed.expr->span);
      tail->string_field("call_style", "bare");
      std::vector<std::unique_ptr<ast::Expr>> args;
      args.push_back(std::move(parsed.expr));
      tail->list_field("args", std::move(args));
      std::vector<std::unique_ptr<ast::Expr>> tails;
      tails.push_back(std::move(tail));
      chain->list_field("tails", std::move(tails));
      auto stmt = ast::make_expr("AstExprStmt", chain->span);
      stmt->node_field("expr", std::move(chain));
      body->values.push_back(std::move(stmt));
      // Preserve attachment of another annotation immediately above @template.
      decl.span.start = items[i]->span.start;
      items.erase(items.begin() + i);
      --i;
    } catch (const std::exception &error) {
      fail("TEMPLATE_INPUT",
           "cannot load template '" + path + "': " + error.what());
    }
  }
  for (const auto &item : items)
    if (!annotation(*item))
      reject_unattached(*item, result);
  items.insert(items.begin(), std::make_move_iterator(added_imports.begin()),
               std::make_move_iterator(added_imports.end()));
  return result;
}
} // namespace sputnik::templates
