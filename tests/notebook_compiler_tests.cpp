#include "bytecode/format.h"
#include "notebook/compiler.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using amber::bytecode::BcCode;
using amber::bytecode::CodeKind;
using amber::bytecode::Opcode;
using amber::bytecode::NotebookEmitter;
using amber::notebook::BindingKey;
using amber::notebook::CellId;
using amber::notebook::CellSource;
using amber::notebook::DependencyGraph;
using amber::notebook::NotebookCompileOptions;
using amber::notebook::NotebookImage;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

CellSource cell(CellId id, std::string source, std::string file = "<cell>") {
  CellSource result;
  result.id = id;
  result.source = std::move(source);
  result.file = std::move(file);
  return result;
}

const BcCode &code_for(const NotebookImage &image, std::uint32_t code_id) {
  for (const BcCode &code : image.module().code_objects) {
    if (code.code_id == code_id) {
      return code;
    }
  }
  std::cerr << "FAIL: missing code id " << code_id << "\n";
  std::exit(1);
}

bool has_opcode(const BcCode &code, Opcode opcode) {
  for (const auto &instruction : code.instructions) {
    if (instruction.opcode == opcode) {
      return true;
    }
  }
  return false;
}

amber::hir::Procedure
procedure_with_body(std::unique_ptr<amber::ast::Expr> body) {
  amber::hir::Procedure procedure;
  procedure.id = "notebook-test";
  procedure.name = "notebook-test";
  procedure.kind = "notebook_cell";
  procedure.locals.push_back({"l0", "local", "local", "local", {}});
  procedure.body = std::move(body);
  return procedure;
}

std::unique_ptr<amber::ast::Expr> sequence_with(
    std::unique_ptr<amber::ast::Expr> item) {
  auto sequence =
      std::make_unique<amber::ast::Expr>("HSeq", amber::lexer::Span{});
  std::vector<std::unique_ptr<amber::ast::Expr>> items;
  items.push_back(std::move(item));
  sequence->list_field("items", std::move(items));
  return sequence;
}

std::vector<std::uint8_t> serialized(const NotebookEmitter &emitter) {
  return amber::bytecode::serialize_module(emitter.module());
}

std::string path_constant_text(const amber::bytecode::BcModule &module,
                               std::uint32_t ref_id) {
  expect(ref_id < module.const_pool.size(), "ambient path ref is in range");
  const auto &constant = module.const_pool[ref_id];
  expect(constant.kind == amber::bytecode::ConstantKind::Path,
         "ambient lookup uses a path constant");
  std::string result;
  for (std::size_t index = 0; index < constant.items.size(); ++index) {
    expect(constant.items[index] < module.symbols.size(),
           "ambient path symbol is in range");
    if (index != 0U) {
      result += ".";
    }
    result += module.symbols[constant.items[index]];
  }
  return result;
}

void test_literal_assignment_and_external_read() {
  const CellSource first = cell(1, "a = 1 + 2\n", "first.am");
  const CellSource second = cell(2, "b = a + 3\n", "second.am");
  const CellSource repeated = cell(3, "c = a + a\n", "repeated.am");
  const DependencyGraph graph({first, second, repeated});
  NotebookImage image;

  const auto first_result = amber::notebook::compile_cell(first, graph, image);
  expect(first_result.ok, "a=1+2 should compile");
  const auto second_result =
      amber::notebook::compile_cell(second, graph, image);
  expect(second_result.ok, "b=a+3 should compile");
  expect(first_result.code_id != second_result.code_id,
         "cells in one image need unique code IDs");
  expect(!image.module().init.has_entry_code_id,
         "notebook image must not install a module init");

  const BcCode &first_code = code_for(image, first_result.code_id);
  const BcCode &second_code = code_for(image, second_result.code_id);
  expect(first_code.kind == CodeKind::NotebookCell,
         "first cell should be NotebookCell code");
  expect(second_code.kind == CodeKind::NotebookCell,
         "second cell should be NotebookCell code");
  expect(has_opcode(first_code, Opcode::StoreNotebookSlot),
         "declared a write should use StoreNotebookSlot");
  expect(has_opcode(second_code, Opcode::LoadNotebookSlot),
         "external a read should use LoadNotebookSlot");
  expect(has_opcode(second_code, Opcode::StoreNotebookSlot),
         "declared b write should use StoreNotebookSlot");

  const auto a_descriptor = image.descriptor_for(BindingKey{1, "a"});
  const auto b_descriptor = image.descriptor_for(BindingKey{2, "b"});
  expect(a_descriptor.has_value() && b_descriptor.has_value(),
         "image should retain BindingKey descriptor metadata");
  expect(*a_descriptor != *b_descriptor,
         "different BindingKeys need different descriptors");
  expect(second_result.inputs.size() == 1U &&
             second_result.inputs.front().descriptor.key ==
                 BindingKey{1, "a"} &&
             second_result.inputs.front().direction ==
                 amber::notebook::NotebookCellSlotDirection::Input,
         "b=a+3 should expose the preceding a version as input");
  expect(first_result.outputs.size() == 1U &&
             first_result.outputs.front().descriptor.key ==
                 BindingKey{1, "a"} &&
             first_result.outputs.front().direction ==
                 amber::notebook::NotebookCellSlotDirection::Output,
         "producer and consumer should use the same descriptor with different "
         "directions");
  const auto repeated_result =
      amber::notebook::compile_cell(repeated, graph, image);
  expect(repeated_result.ok && repeated_result.inputs.size() == 1U,
         "repeated reads of one provider should expose one input use");
}

void test_compound_assignment_uses_previous_version() {
  const CellSource previous = cell(10, "a = 5\n");
  const CellSource increment = cell(11, "a = a + 1\n");
  const DependencyGraph graph({previous, increment});
  NotebookImage image;
  expect(amber::notebook::compile_cell(previous, graph, image).ok,
         "previous a should compile");
  const auto result = amber::notebook::compile_cell(increment, graph, image);
  expect(result.ok, "a=a+1 should compile");
  const BcCode &code = code_for(image, result.code_id);
  expect(has_opcode(code, Opcode::LoadNotebookSlot),
         "compound assignment must load previous a version");
  expect(has_opcode(code, Opcode::StoreNotebookSlot),
         "compound assignment must publish current a version");
  const auto previous_descriptor = image.descriptor_for(BindingKey{10, "a"});
  const auto current_descriptor = image.descriptor_for(BindingKey{11, "a"});
  expect(previous_descriptor.has_value() && current_descriptor.has_value() &&
             *previous_descriptor != *current_descriptor,
         "input and output versions must not share a descriptor");
}

void test_same_cell_reads_stay_local() {
  const CellSource source = cell(20, "a = 1\nb = a + 1\n");
  const DependencyGraph graph({source});
  NotebookImage image;
  const auto result = amber::notebook::compile_cell(source, graph, image);
  expect(result.ok, "same-cell local read should compile");
  const BcCode &code = code_for(image, result.code_id);
  expect(!has_opcode(code, Opcode::LoadNotebookSlot),
         "same-cell a read must use its local slot");
  expect(result.inputs.empty(), "same-cell read should not create input");
  expect(result.outputs.size() == 2U,
         "both same-cell assignments should be published as outputs");
}

void test_repeated_external_reads_share_one_cell_input() {
  const CellSource provider = cell(21, "a = 4\n");
  const CellSource consumer = cell(22, "b = a + a\n");
  const DependencyGraph graph({provider, consumer});
  NotebookImage image;
  expect(amber::notebook::compile_cell(provider, graph, image).ok,
         "repeated-read provider should compile");
  const auto result = amber::notebook::compile_cell(consumer, graph, image);
  expect(result.ok, "repeated external reads should compile");
  expect(result.inputs.size() == 1U &&
             result.inputs.front().descriptor.key == BindingKey{21, "a"},
         "multiple source occurrences should share one descriptor-table input");
}

void test_ambient_export_read_uses_qualified_lookup() {
  const CellSource source = cell(25, "answer = A\n", "ambient.am");
  const DependencyGraph graph({source});
  NotebookImage image;
  NotebookCompileOptions options;
  options.ambient_constant_paths.emplace("A", "models.A");

  const auto result =
      amber::notebook::compile_cell(source, graph, image, options);
  expect(result.ok,
         result.diagnostics.empty()
             ? "ambient bundled export should compile"
             : "ambient bundled export should compile: " +
                   result.diagnostics.front().code + " " +
                   result.diagnostics.front().message);
  expect(result.inputs.empty(), "ambient export must not create an input slot");
  const BcCode &code = code_for(image, result.code_id);
  expect(has_opcode(code, Opcode::LookupConst),
         "ambient export should lower to LookupConst");
  expect(!has_opcode(code, Opcode::LoadNotebookSlot),
         "ambient export must not lower to LoadNotebookSlot");
  for (const auto &instruction : code.instructions) {
    if (instruction.opcode != Opcode::LookupConst) {
      continue;
    }
    expect(instruction.operands.size() == 2U,
           "LookupConst should carry destination and path operands");
    expect(path_constant_text(
               image.module(),
               static_cast<std::uint32_t>(instruction.operands[1].value)) ==
               "models.A",
           "ambient export should preserve its qualified path");
    return;
  }
  expect(false, "ambient export LookupConst instruction was not found");
}

void test_native_prelude_reads_and_shadowing() {
  const CellSource source = cell(601, "answer = system\n");
  const DependencyGraph graph({source});
  expect(graph.missing_names(source.id).empty(),
         "native prelude names must not be missing notebook inputs");
  NotebookImage image;
  const auto result = amber::notebook::compile_cell(source, graph, image);
  expect(result.ok && result.inputs.empty(),
         "native prelude must compile without a notebook slot");
  expect(has_opcode(code_for(image, result.code_id), Opcode::LookupConst),
         "native prelude must lower to a runtime constant lookup");

  const CellSource provider = cell(600, "system = 7\n");
  const DependencyGraph shadowed({provider, source});
  expect(shadowed.provider_for(source.id, "system") ==
             std::optional<BindingKey>(BindingKey{provider.id, "system"}),
         "a preceding notebook value must shadow the native prelude");
  NotebookImage shadowed_image;
  expect(amber::notebook::compile_cell(provider, shadowed, shadowed_image).ok,
         "native-prelude shadow provider should compile");
  const auto shadowed_result =
      amber::notebook::compile_cell(source, shadowed, shadowed_image);
  expect(shadowed_result.ok && shadowed_result.inputs.size() == 1U &&
             has_opcode(code_for(shadowed_image, shadowed_result.code_id),
                        Opcode::LoadNotebookSlot),
         "a shadowed prelude must read the preceding notebook slot");
}

void test_notebook_provider_shadows_ambient_export() {
  const CellSource provider = cell(26, "A = 7\n", "provider.am");
  const CellSource consumer = cell(27, "answer = A\n", "shadow.am");
  const DependencyGraph graph({provider, consumer});
  NotebookImage image;
  NotebookCompileOptions options;
  options.ambient_constant_paths.emplace("A", "models.A");

  expect(amber::notebook::compile_cell(provider, graph, image).ok,
         "notebook provider should compile before ambient shadow test");
  const auto result =
      amber::notebook::compile_cell(consumer, graph, image, options);
  expect(result.ok, "provider-shadowed ambient read should compile");
  expect(result.inputs.size() == 1U &&
             result.inputs.front().descriptor.key == BindingKey{26, "A"},
         "preceding notebook provider should remain the input source");
  const BcCode &code = code_for(image, result.code_id);
  expect(has_opcode(code, Opcode::LoadNotebookSlot),
         "notebook provider should lower to LoadNotebookSlot");
  expect(!has_opcode(code, Opcode::LookupConst),
         "notebook provider must shadow ambient LookupConst");
}

void test_malformed_ambient_path_is_diagnostic() {
  const CellSource source = cell(28, "answer = A\n", "bad-ambient.am");
  const DependencyGraph graph({source});
  NotebookImage image;
  NotebookCompileOptions options;
  options.ambient_constant_paths.emplace("A", "models..A");

  const auto result =
      amber::notebook::compile_cell(source, graph, image, options);
  expect(!result.ok && !result.diagnostics.empty(),
         "malformed ambient path should be rejected");
  expect(result.diagnostics.front().code == "NB1006",
         "malformed ambient path should use NB1006");
  expect(image.descriptors().empty(),
         "malformed ambient path must not mutate notebook descriptors");
}

void test_duplicate_code_ids_and_diagnostics() {
  const CellSource first = cell(30, "a = 1\n", "first.am");
  const CellSource second = cell(31, "b = 2\n", "second.am");
  const DependencyGraph graph({first, second});
  NotebookImage image;
  NotebookCompileOptions fixed_id;
  fixed_id.code_id = 77;
  expect(amber::notebook::compile_cell(first, graph, image, fixed_id).ok,
         "explicit code id should compile once");
  const std::size_t code_count = image.module().code_objects.size();
  const auto duplicate =
      amber::notebook::compile_cell(second, graph, image, fixed_id);
  expect(!duplicate.ok && image.module().code_objects.size() == code_count,
         "duplicate code id must not append another code object");
  expect(!image.descriptor_for(BindingKey{31, "b"}).has_value(),
         "failed compile must roll back descriptor sidecar allocations");

  const CellSource missing = cell(40, "answer = unknown + 1\n", "missing.am");
  const DependencyGraph missing_graph({missing});
  NotebookImage missing_image;
  const auto missing_result =
      amber::notebook::compile_cell(missing, missing_graph, missing_image);
  expect(!missing_result.ok && !missing_result.diagnostics.empty(),
         "missing notebook provider should be a compile diagnostic");
  expect(missing_result.diagnostics.front().span.file == "missing.am",
         "diagnostic should preserve CellSource file");
  expect(missing_result.diagnostics.front().span.start.offset > 0,
         "diagnostic should preserve the source occurrence span");
}

void test_definitions_are_rejected_deterministically() {
  const CellSource source = cell(50, "def f():\n  1\n", "defs.am");
  const DependencyGraph graph({source});
  NotebookImage image;
  const auto result = amber::notebook::compile_cell(source, graph, image);
  expect(!result.ok && !result.diagnostics.empty(),
         "definitions should be rejected by the first cell profile");
  expect(result.diagnostics.front().code == "NB1002",
         "unsupported definition should use a notebook diagnostic");
  expect(image.descriptors().empty(),
         "rejected notebook profile constructs must not mutate the image");
}

void test_replacement_image_preserves_runtime_name_prefix() {
  const std::vector<std::string> strings{"runtime-old", "runtime-dynamic"};
  const std::vector<std::string> symbols{"old-selector"};
  NotebookImage image(strings, symbols);
  const CellSource source = cell(60, "message = \"notebook-new\"\n");
  const DependencyGraph graph({source});
  const auto result = amber::notebook::compile_cell(source, graph, image);
  expect(result.ok, "seeded replacement image should compile");
  expect(image.module().strings.size() > strings.size(),
         "replacement image should append newly interned strings");
  expect(std::equal(strings.begin(), strings.end(),
                    image.module().strings.begin()),
         "active runtime strings must remain the exact image prefix");
  expect(image.module().symbols.size() >= symbols.size() &&
             std::equal(symbols.begin(), symbols.end(),
                        image.module().symbols.begin()),
         "active runtime symbols must remain the exact image prefix");
}

void test_composite_image_preserves_ordinary_base_module() {
  amber::bytecode::BcModule base;
  base.format_version = {1, 0};
  base.language_version = {1, 0};
  base.strings = {"base-string"};
  base.symbols = {"base-symbol"};
  BcCode init;
  init.code_id = 1;
  init.kind = CodeKind::Module;
  init.reg_count = 1;
  init.instructions = {{Opcode::LoadNull, {{0, false}}},
                       {Opcode::Return, {{0, false}}}};
  base.code_objects.push_back(init);
  base.init = {true, 1, 0};

  NotebookImage image(std::move(base));
  const CellSource source = cell(65, "answer = A\n", "composite.am");
  const DependencyGraph graph({source});
  NotebookCompileOptions options;
  options.ambient_constant_paths.emplace("A", "models.A");
  const auto compiled =
      amber::notebook::compile_cell(source, graph, image, options);
  expect(compiled.ok && compiled.code_id != 1U,
         "composite cell should compile without colliding with base code ids");

  const auto snapshot = image.snapshot();
  expect(snapshot->init.has_entry_code_id && snapshot->init.entry_code_id == 1U &&
             snapshot->code_objects.size() == 2U &&
             snapshot->code_objects.front().kind == CodeKind::Module &&
             snapshot->code_objects.back().kind == CodeKind::NotebookCell &&
             snapshot->strings.front() == "base-string" &&
             snapshot->symbols.front() == "base-symbol",
         "composite snapshot should preserve the complete ordinary base as an "
         "index-stable prefix");
  const auto decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(*snapshot));
  expect(decoded.ok(),
         decoded.errors.empty()
             ? "composite notebook snapshot should verify"
             : amber::bytecode::verify_errors_to_json(decoded.errors));
}

void test_snapshot_contains_self_describing_cell_manifest() {
  const CellSource provider = cell(70, "a = 4\n");
  const CellSource consumer = cell(71, "b = a + a\n");
  const DependencyGraph graph({provider, consumer});
  NotebookImage image;
  const auto provider_result =
      amber::notebook::compile_cell(provider, graph, image);
  const auto consumer_result =
      amber::notebook::compile_cell(consumer, graph, image);
  expect(provider_result.ok && consumer_result.ok,
         "manifest source cells should compile");

  const auto snapshot = image.snapshot();
  expect((snapshot->file_flags & amber::bytecode::kFileFlagNotebookOnly) != 0U,
         "notebook snapshot should carry the notebook-only file flag");
  expect(snapshot->notebook_metadata.has_value(),
         "notebook snapshot should contain NBMD metadata");
  const auto &metadata = *snapshot->notebook_metadata;
  expect(metadata.cells.size() == 2U,
         "snapshot should describe every compiled notebook cell");
  expect(metadata.cells[0].cell_id == provider.id &&
             metadata.cells[0].code_id == provider_result.code_id &&
             metadata.cells[0].input_descriptor_ids.empty() &&
             metadata.cells[0].output_descriptor_ids.size() == 1U,
         "provider manifest entry should preserve identity and outputs");
  expect(metadata.cells[1].cell_id == consumer.id &&
             metadata.cells[1].code_id == consumer_result.code_id &&
             metadata.cells[1].input_descriptor_ids.size() == 1U &&
             metadata.cells[1].output_descriptor_ids.size() == 1U,
         "consumer manifest entry should preserve identity and slot uses");
  expect(metadata.descriptors.size() == image.descriptors().size(),
         "snapshot should serialize every interned descriptor");
  for (const auto &descriptor : metadata.descriptors) {
    expect(descriptor.name_str_id < snapshot->strings.size() &&
               !snapshot->strings[descriptor.name_str_id].empty(),
           "descriptor should reference a non-empty STRS binding name");
  }
}

void test_explicit_code_id_reserves_try_rescue_helpers() {
  const CellSource source = cell(80, "try:\n"
                                      "  1\n"
                                      "rescue:\n"
                                      "  2\n"
                                      "ensure:\n"
                                      "  3\n",
                                 "try-rescue.am");
  const DependencyGraph graph({source});
  NotebookImage image;
  NotebookCompileOptions options;
  options.code_id = 1;
  const auto result =
      amber::notebook::compile_cell(source, graph, image, options);
  expect(result.ok,
         "try/rescue/ensure cell with explicit code id should compile");

  std::set<std::uint32_t> code_ids;
  for (const BcCode &code : image.module().code_objects) {
    expect(code_ids.insert(code.code_id).second,
           "notebook cell and try/rescue/ensure helper code ids must be "
           "unique");
  }
  expect(code_ids.count(result.code_id) == 1U && result.code_id == 1U,
         "explicit notebook cell code id should be retained");
  expect(code_ids.size() >= 3U,
         "try/rescue/ensure emission should install helper code objects");

  const auto snapshot = image.snapshot();
  const auto decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(*snapshot));
  expect(decoded.ok(),
         "serialized notebook image with reserved helper ids should verify");
  std::set<std::uint32_t> decoded_code_ids;
  for (const BcCode &code : decoded.module.code_objects) {
    expect(decoded_code_ids.insert(code.code_id).second,
           "deserialized notebook helper code ids must remain unique");
  }
}

void test_failed_notebook_emission_rolls_back_full_emitter_state() {
  NotebookEmitter emitter({"seed"}, {"seed-selector"});
  const std::vector<std::uint8_t> before = serialized(emitter);

  // HStringify without an operand diagnoses after the emitter has already
  // interned procedure locals and the fallback empty string/constant.
  auto malformed = std::make_unique<amber::ast::Expr>("HStringify",
                                                       amber::lexer::Span{});
  const auto failed = emitter.append_cell(
      procedure_with_body(sequence_with(std::move(malformed))), 77);
  expect(!failed.ok(),
         "malformed notebook procedure should emit a diagnostic");
  expect(serialized(emitter) == before,
         "failed emission must roll back module and every emitter side table");

  auto valid_const = std::make_unique<amber::ast::Expr>("HConstStr",
                                                        amber::lexer::Span{});
  valid_const->string_field("value", "ok");
  const auto successful = emitter.append_cell(
      procedure_with_body(sequence_with(std::move(valid_const))), 77);
  expect(successful.ok(), "the next notebook emission should succeed");

  NotebookEmitter fresh({"seed"}, {"seed-selector"});
  auto fresh_const = std::make_unique<amber::ast::Expr>("HConstStr",
                                                        amber::lexer::Span{});
  fresh_const->string_field("value", "ok");
  const auto fresh_result = fresh.append_cell(
      procedure_with_body(sequence_with(std::move(fresh_const))), 77);
  expect(fresh_result.ok(), "fresh notebook emission should succeed");
  expect(serialized(emitter) == serialized(fresh),
         "a post-failure emission must receive the same IDs as a fresh image");
}

} // namespace

int main() {
  test_literal_assignment_and_external_read();
  test_compound_assignment_uses_previous_version();
  test_same_cell_reads_stay_local();
  test_repeated_external_reads_share_one_cell_input();
  test_ambient_export_read_uses_qualified_lookup();
  test_native_prelude_reads_and_shadowing();
  test_notebook_provider_shadows_ambient_export();
  test_malformed_ambient_path_is_diagnostic();
  test_duplicate_code_ids_and_diagnostics();
  test_definitions_are_rejected_deterministically();
  test_replacement_image_preserves_runtime_name_prefix();
  test_composite_image_preserves_ordinary_base_module();
  test_snapshot_contains_self_describing_cell_manifest();
  test_explicit_code_id_reserves_try_rescue_helpers();
  test_failed_notebook_emission_rolls_back_full_emitter_state();
  std::cout << "notebook_compiler_tests ok\n";
  return 0;
}
