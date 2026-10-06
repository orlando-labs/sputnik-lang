#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/vm.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib codecs test failed: " << message << "\n";
    std::exit(1);
  }
}

sputnik::bytecode::BcModule compile_source_or_die(const std::string &source) {
  sputnik::lexer::Lexer lexer(source, "<stdlib-codecs-source-test>");
  sputnik::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << sputnik::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  sputnik::parser::Parser parser(lex_result.tokens);
  sputnik::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << sputnik::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  sputnik::binder::BindResult bind_result =
      sputnik::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << sputnik::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }

  sputnik::hir::Program program = sputnik::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  sputnik::bytecode::EmitResult emit_result =
      sputnik::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << sputnik::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }

  sputnik::bytecode::DecodeResult decoded = sputnik::bytecode::deserialize_module(
      sputnik::bytecode::serialize_module(emit_result.module));
  if (!decoded.ok()) {
    std::cerr << sputnik::bytecode::verify_errors_to_json(decoded.errors);
    std::exit(1);
  }
  return std::move(decoded.module);
}

sputnik::runtime::ExecutionResult execute_source(const std::string &source) {
  sputnik::bytecode::BcModule module = compile_source_or_die(source);
  expect(module.init.has_entry_code_id, "source module should have init code");
  return sputnik::runtime::execute_code(module, module.init.entry_code_id);
}

void expect_ok_integer(const sputnik::runtime::ExecutionResult &result,
                       std::int64_t expected, const std::string &message) {
  if (!result.ok() && result.fault.has_value()) {
    std::cerr << "[fault] " << message << ": " << result.fault->error_name
              << " / " << result.fault->message << "\n";
  }
  expect(result.ok(), message + " should succeed");
  expect(result.value.is_integer(), message + " should return Int");
  expect(result.value.as_integer() == expected, message + " value");
}

void expect_fault(const std::string &source, const std::string &error_name,
                  const std::string &message) {
  const sputnik::runtime::ExecutionResult result = execute_source(source);
  expect(!result.ok() && result.fault.has_value(), message + " should fault");
  expect(result.fault->error_name == error_name,
         message + " should fault with " + error_name + ", got " +
             (result.fault.has_value() ? result.fault->error_name : ""));
}

void test_base64_hex_roundtrip() {
  const sputnik::runtime::ExecutionResult result = execute_source(
      "bytes = Bytes.new(\"hello\")\n"
      "a = Base64.encode(bytes)\n"
      "b = Base64.encode(bytes, padding: false)\n"
      "decoded = Base64.decode(\"aGVsbG8=\").to_str()\n"
      "folded = Base64.decode(\"aG Vs\\nbG8\", mode: :lenient).to_str()\n"
      "url = Base64Url.encode(Hex.decode(\"fbff\"))\n"
      "url_hex = Base64Url.decode(\"-_8\").hex()\n"
      "hex = Hex.encode(bytes)\n"
      "hex_round = Hex.decode(\"68 65:6C-6c 6F\", mode: :lenient).to_str()\n"
      "if a == \"aGVsbG8=\" and b == \"aGVsbG8\" and decoded == \"hello\" "
      "and folded == \"hello\" and url == \"-_8\" and url_hex == \"fbff\" "
      "and hex == \"68656c6c6f\" and hex_round == \"hello\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "base64/base64url/hex roundtrip");
}

void test_buffer_and_slice_inputs() {
  const sputnik::runtime::ExecutionResult result = execute_source(
      "buf = io.ByteBuffer(3)\n"
      "buf.put!(0)\n"
      "buf.put!(127)\n"
      "buf.put!(255)\n"
      "buf.flip!()\n"
      "full = Hex.encode(buf)\n"
      "part = Hex.encode(buf.slice(1, 2))\n"
      "if full == buf.bytes().hex() and full == \"007fff\" and part == "
      "\"7fff\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "ByteBuffer and ByteSlice codec inputs");
}

void test_invalid_inputs_fault() {
  expect_fault("Base64.decode(\"a GVs\")\n", "CodecDecodeError",
               "strict base64 whitespace");
  expect_fault("Base64Url.decode(\"+/8=\")\n", "CodecDecodeError",
               "base64url rejects standard alphabet");
  expect_fault("Hex.decode(\"0\")\n", "CodecDecodeError",
               "odd hex length");
  expect_fault("Hex.decode(\"0x00\")\n", "CodecDecodeError",
               "strict hex rejects prefix");
  expect_fault("Base64.encode(\"hello\")\n", "TypeError",
               "encode rejects implicit Str bytes");
}

} // namespace

int main() {
  test_base64_hex_roundtrip();
  test_buffer_and_slice_inputs();
  test_invalid_inputs_fault();

  std::cout << "stdlib_codecs_tests: ok\n";
  return 0;
}
