#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "profile/capabilities.h"
#include "runtime/vm.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib uuid test failed: " << message << "\n";
    std::exit(1);
  }
}

sputnik::bytecode::BcModule compile_source_or_die(const std::string &source) {
  sputnik::lexer::Lexer lexer(source, "<stdlib-uuid-source-test>");
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
  expect(decoded.ok(), "compiled module should verify");
  return std::move(decoded.module);
}

sputnik::runtime::ExecutionResult execute_source(const std::string &source,
                                               bool grant_random = false,
                                               bool enforce_replay = false) {
  sputnik::bytecode::BcModule module = compile_source_or_die(source);
  sputnik::runtime::RuntimeWorldOptions options;
  if (grant_random) {
    module.capabilities.push_back(
        sputnik::capability::make_capability("random.secure"));
    options.capability_grants.push_back(
        sputnik::capability::make_capability("random.secure"));
  }
  options.enforce_replay = enforce_replay;
  sputnik::runtime::RuntimeWorld world(module, options);
  return world.execute(module.init.entry_code_id);
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
         message + " should fault with " + error_name);
}

void test_parse_format_equality_and_alias() {
  expect_ok_integer(
      execute_source(
          "a = Uuid.parse(\"550E8400-E29B-41D4-A716-446655440000\")\n"
          "b = UUID.parse(a.to_str)\n"
          "if a == b and Uuid === a and UUID === b and "
          "Uuid === UUID and UUID === Uuid and "
          "a.to_str == \"550e8400-e29b-41d4-a716-446655440000\" and "
          "a.inspect == a.to_str and "
          "a.to_json == \"\\\"550e8400-e29b-41d4-a716-446655440000\\\"\" and "
          "a.version == 4:\n"
          "  42\n"
          "else:\n"
          "  0\n"),
      42, "parse, canonical format, equality, matching, JSON, and UUID alias");
}

void test_v4_v7_bits_and_secure_random_alias() {
  const sputnik::runtime::ExecutionResult raw_v4 =
      execute_source("Uuid.v4()\n", true);
  expect(raw_v4.ok() && raw_v4.value.is_uuid(), "Uuid.v4 returns Uuid");
  const std::shared_ptr<sputnik::runtime::RuntimeUuidValue> v4 =
      raw_v4.value.as_uuid();
  expect(v4 != nullptr && (v4->bytes[6] >> 4U) == 4U,
         "Uuid.v4 sets the version bits");
  expect(v4 != nullptr && (v4->bytes[8] & 0xC0U) == 0x80U,
         "Uuid.v4 sets the RFC variant bits");

  const sputnik::runtime::ExecutionResult raw_v7 =
      execute_source("Uuid.v7()\n", true);
  expect(raw_v7.ok() && raw_v7.value.is_uuid(), "Uuid.v7 returns Uuid");
  const std::shared_ptr<sputnik::runtime::RuntimeUuidValue> v7 =
      raw_v7.value.as_uuid();
  expect(v7 != nullptr && (v7->bytes[6] >> 4U) == 7U,
         "Uuid.v7 sets the version bits");
  expect(v7 != nullptr && (v7->bytes[8] & 0xC0U) == 0x80U,
         "Uuid.v7 sets the RFC variant bits");

  expect_ok_integer(
      execute_source(
          "a = Uuid.v4()\n"
          "b = Uuid.v7()\n"
          "c = SecureRandom.uuid\n"
          "if a.version == 4 and b.version == 7 and c.version == 4 and "
          "a.to_str.length() == 36 and b.to_str.length() == 36 and "
          "c.to_str.length() == 36:\n"
          "  42\n"
          "else:\n"
          "  0\n",
          true),
      42, "v4, v7, and SecureRandom.uuid");
}

void test_faults_and_policy() {
  expect_fault("Uuid.parse(\"not-a-uuid\")\n", "UuidParseError",
               "malformed UUID");
  expect_fault("Uuid.parse(1)\n", "TypeError", "non-string UUID");

  const sputnik::runtime::ExecutionResult denied = execute_source("Uuid.v4()\n");
  expect(!denied.ok() && denied.fault.has_value() &&
             denied.fault->error_name == "CapabilityError",
         "Uuid.v4 requires random.secure capability");

  const sputnik::runtime::ExecutionResult replay =
      execute_source("Uuid.v7()\n", true, true);
  expect(!replay.ok() && replay.fault.has_value() &&
             replay.fault->error_name == "DeterminismError",
         "Uuid.v7 rejects unrecorded wall time in replay mode");
}

} // namespace

int main() {
  test_parse_format_equality_and_alias();
  test_v4_v7_bits_and_secure_random_alias();
  test_faults_and_policy();
  std::cout << "stdlib_uuid_tests: ok\n";
  return 0;
}
