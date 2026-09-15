#include "bytecode/graph_linker.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using amber::bytecode::BcClass;
using amber::bytecode::BcCode;
using amber::bytecode::BcModule;
using amber::bytecode::CodeKind;
using amber::bytecode::Constant;
using amber::bytecode::ConstantKind;
using amber::bytecode::DepEntry;
using amber::bytecode::ExportEntry;
using amber::bytecode::GraphModule;
using amber::bytecode::Instruction;
using amber::bytecode::Opcode;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "graph linker test failed: " << message << "\n";
    std::exit(1);
  }
}

GraphModule module(const std::string &name,
                   const std::vector<std::string> &dependencies = {}) {
  GraphModule result;
  result.name = name;
  result.path = name + ".am";
  result.module.format_version = {1, 0};
  result.module.language_version = {1, 0};
  for (const std::string &dependency : dependencies) {
    result.module.strings.push_back(dependency);
    DepEntry entry;
    entry.module_name_str_id =
        static_cast<std::uint32_t>(result.module.strings.size() - 1U);
    result.module.dependencies.push_back(entry);
  }
  return result;
}

void add_class_export(GraphModule *module, const std::string &name,
                      const std::string &superclass = {}) {
  module->module.symbols.push_back(name);
  const std::uint32_t class_symbol =
      static_cast<std::uint32_t>(module->module.symbols.size() - 1U);
  const std::uint32_t class_index =
      static_cast<std::uint32_t>(module->module.classes.size());
  module->module.strings.push_back("class");
  const std::uint32_t kind =
      static_cast<std::uint32_t>(module->module.strings.size() - 1U);
  Constant ivars;
  ivars.kind = ConstantKind::KeySet;
  module->module.const_pool.push_back(ivars);
  BcClass klass;
  klass.class_name_sym_id = class_symbol;
  klass.ivar_schema_id = 0;
  if (!superclass.empty()) {
    module->module.symbols.push_back(superclass);
    Constant path;
    path.kind = ConstantKind::Path;
    path.items.push_back(
        static_cast<std::uint32_t>(module->module.symbols.size() - 1U));
    klass.has_superclass_ref = true;
    klass.superclass_ref = static_cast<std::uint32_t>(
        module->module.const_pool.size());
    module->module.const_pool.push_back(path);
  }
  module->module.classes.push_back(klass);
  module->module.exports.push_back(
      {class_symbol, kind, class_index, 1, false, 0});
}

void add_function_export(GraphModule *module, const std::string &name) {
  module->module.symbols.push_back(name);
  const std::uint32_t selector =
      static_cast<std::uint32_t>(module->module.symbols.size() - 1U);
  module->module.strings.push_back("method");
  const std::uint32_t kind =
      static_cast<std::uint32_t>(module->module.strings.size() - 1U);
  Constant signature;
  signature.kind = ConstantKind::KeySet;
  module->module.const_pool.push_back(signature);
  BcCode code;
  code.code_id = 1;
  code.reg_count = 1;
  code.instructions = {{Opcode::LoadNull, {{0, false}}},
                       {Opcode::Return, {{0, false}}}};
  module->module.code_objects.push_back(code);
  module->module.methods.push_back({selector, 0, 0, {}, {}, {}, {}, {}, 1, 0});
  module->module.exports.push_back(
      {selector, kind, 0, 1, false, 0});
}

void expect_valid_output(const amber::bytecode::GraphLinkResult &result) {
  expect(result.ok, "link should succeed");
  const auto decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(result.module));
  expect(decoded.ok(), "linked output should verify after serialization");
}

void test_root_union_and_determinism() {
  const std::vector<GraphModule> modules = {module("b"), module("a")};
  const auto result = amber::bytecode::link_graph(
      modules, std::vector<std::string>{"a", "b"});
  expect_valid_output(result);
  expect(result.init_order == std::vector<std::string>({"a", "b"}),
         "roots should retain deterministic supplied order");
}

void test_transitive_order_and_external_namespace() {
  const std::vector<GraphModule> modules = {
      module("root", {"dep", "native.ns"}), module("dep", {"leaf"}),
      module("leaf")};
  const auto result = amber::bytecode::link_graph(
      modules, "root", [](const std::string &name) {
        return name == "native.ns";
      });
  expect_valid_output(result);
  expect(result.init_order == std::vector<std::string>({"leaf", "dep", "root"}),
         "dependencies should be emitted before their users");
}

void test_dependency_declaration_order_is_preserved() {
  const auto result = amber::bytecode::link_graph(
      {module("root", {"b", "a", "b"}), module("a"), module("b")},
      "root");
  expect_valid_output(result);
  expect(result.init_order == std::vector<std::string>({"b", "a", "root"}),
         "dependency order should preserve declarations and deduplicate");
}

void test_dependency_compatibility_is_enforced() {
  GraphModule root = module("root", {"dep"});
  GraphModule dep = module("dep");
  root.module.dependencies[0].required_format = {1, 1};
  expect(!amber::bytecode::link_graph({root, dep}, "root").ok,
         "required bytecode format must be checked during graph linking");

  root = module("root", {"dep"});
  root.module.dependencies[0].has_abi_requirement = true;
  root.module.dependencies[0].abi_requirement.fill(0xABU);
  dep = module("dep");
  dep.module.abi_hash.fill(0xCDU);
  expect(!amber::bytecode::link_graph({root, dep}, "root").ok,
         "dependency ABI requirements must be checked during graph linking");
}

void test_input_verification_and_file_flags_are_rejected() {
  GraphModule invalid = module("invalid");
  invalid.module.file_flags = 0x2U;
  expect(!amber::bytecode::link_graph({invalid}, "invalid").ok,
         "unsupported file flags must not be silently discarded");

  invalid = module("invalid");
  invalid.module.symbols = {"Invalid"};
  BcClass klass;
  klass.class_name_sym_id = 99;
  invalid.module.classes.push_back(klass);
  expect(!amber::bytecode::link_graph({invalid}, "invalid").ok,
         "invalid reachable bytecode must fail before remapping");
}

void test_pattern_ids_are_module_local_identities() {
  GraphModule left = module("left");
  GraphModule right = module("right");
  GraphModule sparse = module("sparse");
  const std::vector<GraphModule *> modules{&left, &right, &sparse};
  const std::vector<std::uint32_t> ids{
      1U, 0U, std::numeric_limits<std::uint32_t>::max()};
  for (std::size_t i = 0; i < modules.size(); ++i) {
    add_function_export(modules[i], "f");
    modules[i]->module.pattern_programs.push_back({ids[i], 0, 0});
    modules[i]->module.methods[0].clause_table.push_back(
        {ids[i], 1, 1, 1, 0});
  }
  const auto result = amber::bytecode::link_graph(
      {left, right, sparse}, std::vector<std::string>{"left", "right", "sparse"});
  expect_valid_output(result);
  std::set<std::uint32_t> mapped;
  for (std::size_t i = 0; i < modules.size(); ++i) {
    const auto id = result.module.pattern_programs.at(i).pattern_id;
    expect(mapped.insert(id).second &&
               result.module.methods.at(i).clause_table.at(0).pattern_program_id == id,
           "sparse pattern IDs and clause references must be remapped together");
  }
}

void test_import_metadata_cannot_bypass_dependency_admission() {
  GraphModule root = module("root");
  root.module.strings = {"amber.import.alias:root\tfoo", "F\tmissing\tfoo\t0"};
  root.module.attrs.push_back({0, 1});
  BcCode init;
  init.code_id = 1;
  init.kind = CodeKind::Module;
  init.reg_count = 1;
  init.instructions = {{Opcode::LoadNull, {{0, false}}},
                       {Opcode::Return, {{0, false}}}};
  root.module.code_objects.push_back(init);
  root.module.init = {true, 1, 0};
  expect(!amber::bytecode::link_graph({root}, "root").ok,
         "undeclared alias must not introduce an external namespace");
  root.module.strings.push_back("missing");
  DepEntry dependency;
  dependency.module_name_str_id = 2;
  root.module.dependencies.push_back(dependency);
  expect(!amber::bytecode::link_graph({root}, "root").ok,
         "declaring an alias dependency must not bypass host admission");
  expect_valid_output(amber::bytecode::link_graph(
      {root}, "root", [](const std::string &name) { return name == "missing"; }));
}

void test_pattern_operands_and_targets_are_relocated() {
  GraphModule dependency = module("dep");
  add_function_export(&dependency, "f");

  GraphModule root = module("root", {"dep"});
  root.module.symbols.push_back("key");
  const std::uint32_t key_symbol =
      static_cast<std::uint32_t>(root.module.symbols.size() - 1U);
  Constant literal;
  literal.kind = ConstantKind::Integer;
  literal.int_value = 7;
  root.module.const_pool.push_back(literal);
  Constant keyset;
  keyset.kind = ConstantKind::KeySet;
  root.module.const_pool.push_back(keyset);
  const std::uint32_t keyset_id =
      static_cast<std::uint32_t>(root.module.const_pool.size() - 1U);

  root.module.strings.push_back("amber.import.alias:root\tf");
  const std::uint32_t import_key =
      static_cast<std::uint32_t>(root.module.strings.size() - 1U);
  root.module.strings.push_back("F\tdep\tf\t0");
  const std::uint32_t import_value =
      static_cast<std::uint32_t>(root.module.strings.size() - 1U);
  root.module.strings.push_back("f");
  const std::uint32_t name =
      static_cast<std::uint32_t>(root.module.strings.size() - 1U);
  root.module.strings.push_back("local");
  const std::uint32_t role =
      static_cast<std::uint32_t>(root.module.strings.size() - 1U);
  root.module.strings.push_back("import_alias");
  const std::uint32_t binding =
      static_cast<std::uint32_t>(root.module.strings.size() - 1U);
  root.module.attrs.push_back({import_key, import_value});

  BcCode init;
  init.code_id = 1;
  init.kind = CodeKind::Module;
  init.reg_count = 2;
  init.local_layout.push_back({0, name, role, binding});
  init.instructions = {
      {Opcode::LoadNull, {{0, false}}},
      {Opcode::PPrepSeq, {{1, false}, {0, false}, {0, false}, {10, false}}},
      {Opcode::PCheckPin, {{1, false}, {0, false}, {10, false}}},
      {Opcode::PCheckLenEq, {{1, false}, {0, false}, {10, false}}},
      {Opcode::PCheckLenGte, {{1, false}, {0, false}, {10, false}}},
      {Opcode::PCheckEq, {{1, false}, {0, false}, {10, false}}},
      {Opcode::PHasKey, {{1, false}, {key_symbol, false}, {10, false}}},
      {Opcode::PTripleEq, {{0, false}, {1, false}, {10, false}}},
      {Opcode::PPrepMap,
       {{1, false}, {0, false}, {keyset_id, false}, {0, false}, {10, false}}},
      {Opcode::Return, {{1, false}}},
      {Opcode::LoadNull, {{1, false}}},
      {Opcode::Return, {{1, false}}},
  };
  init.handler_table.push_back({0, 1, 10, 1, 0});
  root.module.code_objects.push_back(init);
  root.module.init = {true, 1, 0};

  const auto result = amber::bytecode::link_graph({dependency, root}, "root");
  expect_valid_output(result);
  const auto code_it = std::find_if(
      result.module.code_objects.begin(), result.module.code_objects.end(),
      [](const BcCode &code) {
        for (const Instruction &instruction : code.instructions) {
          if (instruction.opcode == Opcode::PPrepSeq) {
            return true;
          }
        }
        return false;
      });
  expect(code_it != result.module.code_objects.end(),
         "linked init should retain pattern instructions");
  expect(code_it->handler_table.at(0).protected_to == 1U &&
             code_it->handler_table.at(0).handler_pc == 11U,
         "a half-open handler ending at the insertion point must exclude seeds");
  for (const Instruction &instruction : code_it->instructions) {
    switch (instruction.opcode) {
    case Opcode::PPrepSeq:
      expect(instruction.operands[3].value == 11,
             "P_PREP_SEQ failure target should shift with import seed");
      break;
    case Opcode::PPrepMap:
      expect(instruction.operands[2].value != keyset_id &&
                 result.module.const_pool.at(instruction.operands[2].value).kind ==
                     ConstantKind::KeySet && instruction.operands[4].value == 11,
             "P_PREP_MAP keyset and failure target should be relocated");
      break;
    case Opcode::PCheckPin:
    case Opcode::PCheckLenEq:
    case Opcode::PCheckLenGte:
    case Opcode::PCheckEq:
    case Opcode::PHasKey:
    case Opcode::PTripleEq:
      expect(instruction.operands[2].value == 11,
             "pattern failure target should shift with import seed");
      break;
    default:
      break;
    }
  }
}

void test_failures_are_explicit() {
  expect(!amber::bytecode::link_graph({module("root", {"missing"})}, "root")
              .ok,
         "missing dependency should fail");
  expect(!amber::bytecode::link_graph(
                   {module("a", {"b"}), module("b", {"a"})}, "a")
              .ok,
         "dependency cycle should fail");
  expect(!amber::bytecode::link_graph({module("a"), module("a")}, "a").ok,
         "duplicate module should fail");
  auto reexport = module("reexport");
  reexport.module.symbols = {"X"};
  reexport.module.strings = {"reexport", "dep"};
  reexport.module.exports.push_back({0, 0, 0, 1, true, 1});
  expect(!amber::bytecode::link_graph({std::move(reexport)}, "reexport").ok,
         "re-export should be rejected explicitly");
}

void test_class_paths_are_module_qualified() {
  GraphModule base = module("base");
  add_class_export(&base, "Base");
  GraphModule child = module("child", {"base"});
  add_class_export(&child, "Base");
  add_class_export(&child, "Child", "Base");
  const auto result = amber::bytecode::link_graph({base, child}, "child");
  expect_valid_output(result);
  expect(result.module.classes.size() == 3U, "all classes should be linked");
  expect(result.module.symbols[result.module.classes[0].class_name_sym_id] ==
             "base.Base",
         "dependency class should be qualified");
  expect(result.module.symbols[result.module.classes[2].class_name_sym_id] ==
             "child.Child",
         "root class should be qualified");
  bool found_child_export = false;
  for (const auto &exported : result.exports) {
    if (exported.provider == "child" && exported.public_name == "Child") {
      found_child_export = exported.qualified_path == "child.Child";
    }
  }
  expect(found_child_export, "class export should expose its live qualified path");
  const auto &super = result.module.const_pool[
      result.module.classes[2].superclass_ref];
  expect(super.items.size() == 2U &&
             result.module.symbols.at(super.items[0]) == "child" &&
             result.module.symbols.at(super.items[1]) == "Base",
         "intra-module superclass lookup should be qualified");
}

} // namespace

int main() {
  test_root_union_and_determinism();
  test_transitive_order_and_external_namespace();
  test_dependency_declaration_order_is_preserved();
  test_dependency_compatibility_is_enforced();
  test_input_verification_and_file_flags_are_rejected();
  test_pattern_ids_are_module_local_identities();
  test_import_metadata_cannot_bypass_dependency_admission();
  test_pattern_operands_and_targets_are_relocated();
  test_failures_are_explicit();
  test_class_paths_are_module_qualified();
  std::cout << "graph_linker_tests: ok\n";
  return 0;
}
