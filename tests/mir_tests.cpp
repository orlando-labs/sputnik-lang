#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "optimizer/mir.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "mir test failed: " << message << "\n";
    std::exit(1);
  }
}

sputnik::mir::Module lower_mir_ok(const std::string &source) {
  sputnik::lexer::Lexer lexer(source, "<test>");
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
  sputnik::mir::Module module =
      sputnik::mir::lower_program(program, parse_result.module_name);
  sputnik::mir::ValidationResult validation = sputnik::mir::validate_module(module);
  if (!validation.ok()) {
    std::cerr << sputnik::mir::validation_errors_to_json(validation.errors);
    std::exit(1);
  }
  return module;
}

const sputnik::mir::Function *function_by_name(const sputnik::mir::Module &module,
                                             const std::string &name) {
  for (const sputnik::mir::Function &function : module.functions) {
    if (function.name == name) {
      return &function;
    }
  }
  return nullptr;
}

bool function_contains_op(const sputnik::mir::Function &function,
                          const std::string &op) {
  for (const sputnik::mir::Block &block : function.blocks) {
    for (const sputnik::mir::Instruction &instruction : block.instructions) {
      if (instruction.op == op) {
        return true;
      }
    }
    if (block.has_terminator && block.terminator.op == op) {
      return true;
    }
  }
  return false;
}

bool has_error_code(const std::vector<sputnik::mir::ValidationError> &errors,
                    const std::string &code) {
  for (const sputnik::mir::ValidationError &error : errors) {
    if (error.code == code) {
      return true;
    }
  }
  return false;
}

void test_conditional_chain_and_nonlocal_return() {
  const auto module = lower_mir_ok(
      "def pipeline(xs, enabled):\n"
      "  xs .map if enabled |x|: x + 1 .first()\n"
      "def leave(xs, enabled):\n"
      "  xs\n"
      "    .each if enabled |x|:\n"
      "      return x\n"
      "  0\n");
  const auto *pipeline = function_by_name(module, "pipeline");
  expect(pipeline != nullptr && function_contains_op(*pipeline, "local.store") &&
             function_contains_op(*pipeline, "branch_if") &&
             function_contains_op(*pipeline, "phi") &&
             !function_contains_op(*pipeline, "unsupported"),
         "guard lowers to ordinary SSA store, branch and merge");
  bool found_nonlocal = false;
  for (const auto &function : module.functions) {
    found_nonlocal = found_nonlocal || function_contains_op(function, "return.nonlocal");
  }
  expect(found_nonlocal, "guarded block preserves nonlocal return");
  auto broken = module;
  for (auto &function : broken.functions) {
    for (auto &block : function.blocks) {
      if (block.has_terminator && block.terminator.op == "return.nonlocal") {
        block.terminator.targets.push_back(block.id);
      }
    }
  }
  expect(has_error_code(sputnik::mir::validate_module(broken).errors, "MIR1009"),
         "nonlocal return is still checked for invalid successors");
}

void test_hir_to_mir_if_ssa() {
  const sputnik::mir::Module module = lower_mir_ok("def choose(x):\n"
                                                 "  if x > 0:\n"
                                                 "    x\n"
                                                 "  else:\n"
                                                 "    0\n");
  const sputnik::mir::Function *choose = function_by_name(module, "choose");
  expect(choose != nullptr, "choose function is lowered");
  expect(choose->entry_block == "bb0", "entry block is stable");
  expect(function_contains_op(*choose, "send"), "binary op lowers to send");
  expect(function_contains_op(*choose, "branch_if"), "if lowers to branch_if");
  expect(function_contains_op(*choose, "phi"), "if result lowers to phi");

  const std::string dump = sputnik::mir::module_to_dump(module, "abc123");
  expect(dump.find("sputnik.mir.v1") != std::string::npos,
         "dump includes format");
  expect(dump.find("func @") != std::string::npos, "dump includes function");
  expect(dump.find("source=sha256:abc123") != std::string::npos,
         "dump includes source hash");
}

void test_closure_capture_operands_use_parent_slots() {
  const sputnik::mir::Module module = lower_mir_ok("def offsetter(xs, delta):\n"
                                                 "  xs.map: _1 + delta\n");
  const sputnik::mir::Function *offsetter = function_by_name(module, "offsetter");
  expect(offsetter != nullptr, "offsetter function is lowered");
  expect(function_contains_op(*offsetter, "closure.make"),
         "closure creation is represented");
}

void test_validator_rejects_duplicate_ssa_definition() {
  sputnik::mir::Module module;
  sputnik::mir::Function function;
  function.id = "p0";
  function.name = "bad";
  function.kind = "method";
  function.entry_block = "bb0";

  sputnik::mir::Block block;
  block.id = "bb0";
  sputnik::mir::Instruction first;
  first.result = "%v0";
  first.op = "const";
  sputnik::mir::Instruction second = first;
  block.instructions.push_back(first);
  block.instructions.push_back(second);
  block.terminator.op = "return";
  block.terminator.operands.push_back(sputnik::mir::value_operand("%v0"));
  block.has_terminator = true;
  function.blocks.push_back(block);
  module.functions.push_back(function);

  const sputnik::mir::ValidationResult validation =
      sputnik::mir::validate_module(module);
  expect(!validation.ok(), "duplicate SSA definition is rejected");
  expect(has_error_code(validation.errors, "MIR1004"),
         "duplicate definition code is stable");
}

void test_pass_harness_phase_order_and_validation() {
  sputnik::mir::Module module = lower_mir_ok("def one():\n"
                                           "  1\n");

  sputnik::mir::Pass noop;
  noop.name = "noop";
  noop.phase_order = 10;
  noop.invalidates = sputnik::mir::kInvalidatesAnalyses;
  noop.run = [](sputnik::mir::Module &) {};

  sputnik::mir::Pass second;
  second.name = "second";
  second.phase_order = 20;
  second.run = [](sputnik::mir::Module &) {};

  sputnik::mir::PassPipelineResult ok =
      sputnik::mir::run_pass_pipeline(module, {noop, second});
  expect(ok.ok(), "ordered preserving passes run");
  expect(module.pass_log.size() == 2, "pass records are attached to module");

  sputnik::mir::Module order_module = lower_mir_ok("def two():\n"
                                                 "  2\n");
  sputnik::mir::PassPipelineResult order_error =
      sputnik::mir::run_pass_pipeline(order_module, {second, noop});
  expect(!order_error.ok(), "out-of-order passes are rejected");
  expect(has_error_code(order_error.errors, "MIR2001"),
         "phase-order error code is stable");

  sputnik::mir::Module invalid_module = lower_mir_ok("def three():\n"
                                                   "  3\n");
  sputnik::mir::Pass duplicate;
  duplicate.name = "duplicate-result";
  duplicate.phase_order = 30;
  duplicate.run = [](sputnik::mir::Module &edited) {
    for (sputnik::mir::Function &function : edited.functions) {
      if (function.name != "three") {
        continue;
      }
      for (sputnik::mir::Block &block : function.blocks) {
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
          if (!block.instructions[i].result.empty()) {
            sputnik::mir::Instruction copy = block.instructions[i];
            block.instructions.push_back(copy);
            return;
          }
        }
      }
    }
  };
  sputnik::mir::PassPipelineResult invalid =
      sputnik::mir::run_pass_pipeline(invalid_module, {duplicate});
  expect(!invalid.ok(), "post-pass SSA validation runs");
  expect(has_error_code(invalid.errors, "MIR1004"),
         "post-pass duplicate definition is reported");
}

} // namespace

int main() {
  test_conditional_chain_and_nonlocal_return();
  test_hir_to_mir_if_ssa();
  test_closure_capture_operands_use_parent_slots();
  test_validator_rejects_duplicate_ssa_definition();
  test_pass_harness_phase_order_and_validation();
  return 0;
}
