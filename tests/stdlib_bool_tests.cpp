#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/stdlib_bool.h"
#include "runtime/vm.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib bool test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-bool-source-test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }
  amber::bytecode::DecodeResult decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(emit_result.module));
  if (!decoded.ok()) {
    std::cerr << amber::bytecode::verify_errors_to_json(decoded.errors);
    std::exit(1);
  }
  return std::move(decoded.module);
}

amber::runtime::ExecutionResult execute_source(const std::string &source) {
  amber::bytecode::BcModule module = compile_source_or_die(source);
  expect(module.init.has_entry_code_id, "source module should have init code");
  return amber::runtime::execute_code(module, module.init.entry_code_id);
}

void expect_boolean(const std::string &source, bool expected,
                    const std::string &message) {
  const auto result = execute_source(source);
  if (!result.ok() && result.fault.has_value()) {
    std::cerr << result.fault->error_name << ": " << result.fault->message
              << "\n";
  }
  expect(result.ok() && result.value.is_bool(), message + " returns Bool");
  expect(result.value.as_bool() == expected, message + " value");
}

void expect_fault(const std::string &source, const std::string &error_name) {
  const auto result = execute_source(source);
  expect(!result.ok() && result.fault.has_value(), source + " must fault");
  expect(result.fault->error_name == error_name, source + " raises " +
                                                     error_name + ", got " +
                                                     result.fault->error_name);
}

void test_tokens_and_normalization() {
  const std::vector<std::pair<std::string, bool>> tokens = {
      {"true", true}, {"t", true},      {"1", true},    {"yes", true},
      {"on", true},   {"false", false}, {"f", false},   {"0", false},
      {"no", false},  {"off", false},   {"null", false}};
  for (const auto &[token, expected] : tokens) {
    expect_boolean("Bool.parse(\"" + token + "\")\n", expected, token);
    std::string upper = token;
    for (char &ch : upper) {
      if (ch >= 'a' && ch <= 'z')
        ch = static_cast<char>(ch - 'a' + 'A');
    }
    expect_boolean("Bool.parse(\" \\t" + upper + "\\r\\n \" )\n", expected,
                   "normalized " + token);
    bool parsed = !expected;
    expect(
        amber::runtime::parse_bool_text("\f\v " + upper + " \v\f", &parsed) &&
            parsed == expected,
        "all ASCII edge whitespace for " + token);
  }
  expect_boolean("Bool.parse(\"TrUe\")\n", true, "mixed case");
  expect_boolean("Bool.parse(\"nUlL\")\n", false, "mixed case null");
  expect_boolean("text = \" y\" + \"es \"\nBool.parse(text)\n", true,
                 "runtime string");
}

void test_invalid_values() {
  for (const std::string &text : {"", " ", "maybe", "flase", "nil", "2", "-1",
                                  "1.0", "true false", "tr ue", "true!"}) {
    expect_fault("Bool.parse(\"" + text + "\")\n", "ValueError");
  }
  expect_fault("Bool.parse(\"true\\nfalse\")\n", "ValueError");
  bool out = true;
  const std::string with_nul("true\0", 5);
  expect(!amber::runtime::parse_bool_text(with_nul, &out) && out,
         "embedded NUL is rejected without changing output");
  for (const std::string &value :
       {"true", "false", "null", "0", "1", "1.0", ":yes", "[]", "{}",
        "Bytes.new(\"true\")"}) {
    expect_fault("Bool.parse(" + value + ")\n", "TypeError");
  }
}

void test_call_contract_and_rescue() {
  expect_fault("Bool.parse()\n", "TypeError");
  expect_fault("Bool.parse(\"true\", \"false\")\n", "TypeError");
  expect_fault("Bool.parse(\"true\", default: false)\n", "TypeError");
  expect_fault("Bool.parse(\"true\"):\n  false\n", "TypeError");
  expect_boolean(
      "try:\n  Bool.parse(\"maybe\")\nrescue ValueError:\n  true\n", true,
      "ValueError rescue");
  expect_boolean("try:\n  Bool.parse(null)\nrescue TypeError:\n  true\n",
                 true, "TypeError rescue");
  expect_boolean("Bool === Bool.parse(\"null\")\n", true, "Bool matcher");
  expect_boolean("kind = Bool\nkind.parse(\"on\")\n", true,
                 "type object alias");
  expect_boolean("\"true\".bool and not \"false\".bool\n", true,
                 "existing conversion properties");
  expect_fault("\"yes\".cast(Bool)\n", "ValueError");
}

} // namespace

int main() {
  test_tokens_and_normalization();
  test_invalid_values();
  test_call_contract_and_rescue();
  std::cout << "stdlib_bool_tests: ok\n";
  return 0;
}
