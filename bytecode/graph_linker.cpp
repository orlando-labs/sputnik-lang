#include "bytecode/graph_linker.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amber::bytecode {
namespace {

std::string bc_string_or_empty(const BcModule &module, std::uint32_t id) {
  return id < module.strings.size() ? module.strings[id] : std::string{};
}

std::string bc_symbol_or_empty(const BcModule &module, std::uint32_t id) {
  return id < module.symbols.size() ? module.symbols[id] : std::string{};
}

void add_module_attr(BcModule *module, const std::string &key,
                     const std::string &value) {
  const auto ensure = [module](const std::string &text) {
    for (std::size_t i = 0; i < module->strings.size(); ++i) {
      if (module->strings[i] == text) {
        return static_cast<std::uint32_t>(i);
      }
    }
    module->strings.push_back(text);
    return static_cast<std::uint32_t>(module->strings.size() - 1U);
  };
  module->attrs.push_back({ensure(key), ensure(value)});
}

struct GraphExportRef {
  std::string kind;
  std::string qualified_path;
  std::uint32_t code_id = 0;
  std::uint32_t method_index = 0;
  std::uint32_t class_index = 0;
};

std::vector<std::string> split_tab_fields(const std::string &text) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t tab = text.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(text.substr(start));
      break;
    }
    fields.push_back(text.substr(start, tab - start));
    start = tab + 1U;
  }
  return fields;
}

bool parse_u32_text(const std::string &text, std::uint32_t *out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10U + static_cast<std::uint64_t>(c - '0');
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
  }
  *out = static_cast<std::uint32_t>(value);
  return true;
}

bool integer_k_opcode(Opcode opcode) {
    switch (opcode) {
  case Opcode::IAddK:
  case Opcode::ISubK:
  case Opcode::ILtK:
  case Opcode::IGtK:
  case Opcode::IMulK:
  case Opcode::IDivK:
  case Opcode::IModK:
  case Opcode::IFloorDivK:
  case Opcode::ILeK:
  case Opcode::IGeK:
  case Opcode::IEqK:
  case Opcode::INeK:
  case Opcode::ICmpK:
  case Opcode::IBitAndK:
  case Opcode::IBitOrK:
  case Opcode::IBitXorK:
  case Opcode::IShlK:
  case Opcode::IShrK:
    return true;
  default:
    return false;
  }
}

bool version_less(const Version &left, const Version &right) {
  if (left.major != right.major) {
    return left.major < right.major;
  }
  return left.minor < right.minor;
}

bool version_greater(const Version &left, const Version &right) {
  if (left.major != right.major) {
    return left.major > right.major;
  }
  return left.minor > right.minor;
}

bool abi_hash_is_zero(const std::array<std::uint8_t, 32> &value) {
  return std::all_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte == 0U; });
}

struct GraphModuleState {
  const GraphModule *input = nullptr;
  std::unordered_map<std::uint32_t, std::uint32_t> strings;
  std::unordered_map<std::uint32_t, std::uint32_t> symbols;
  std::unordered_map<std::uint32_t, std::uint32_t> constants;
  std::unordered_map<std::uint32_t, std::uint32_t> code_ids;
  std::unordered_map<std::uint32_t, std::uint32_t> pattern_ids;
  std::unordered_map<std::uint32_t, std::uint32_t> code_pc_offsets;
  std::unordered_map<std::uint32_t, std::uint32_t> code_pc_insert_points;
  std::unordered_map<std::string, std::string> import_alias_paths;
  std::set<std::string> local_class_names;
  std::unordered_map<std::string, std::string> qualified_class_names;
  std::uint32_t method_offset = 0;
  std::uint32_t class_offset = 0;
  std::map<std::string, GraphExportRef> exports;
};

class GraphModuleBuilder {
public:
  explicit GraphModuleBuilder(std::vector<GraphModule> order)
      : order_(std::move(order)) {
    if (!order_.empty()) {
      out_.format_version = order_.front().module.format_version;
      out_.language_version = order_.front().module.language_version;
      for (const GraphModule &module : order_) {
        if (version_greater(module.module.format_version,
                            out_.format_version)) {
          out_.format_version = module.module.format_version;
        }
        if (version_greater(module.module.language_version,
                            out_.language_version)) {
          out_.language_version = module.module.language_version;
        }
      }
    }
  }

  GraphLinkResult
  build(const std::string &root_module) {
    GraphLinkResult result;
    if (order_.empty()) {
      result.diagnostics.push_back(
          {"BuildError", "native graph is empty", root_module, {}, {}});
      return result;
    }
    std::uint32_t method_offset = 0;
    std::uint32_t class_offset = 0;
    std::uint64_t next_pattern_id = 0;
    for (const GraphModule &module : order_) {
      GraphModuleState state;
      state.input = &module;
      state.method_offset = method_offset;
      state.class_offset = class_offset;
      for (const BcCode &code : module.module.code_objects) {
        if (code.code_id == 0U ||
            !state.code_ids.emplace(code.code_id, next_code_id_++).second) {
          throw std::runtime_error("duplicate or invalid code id in module " +
                                   module.name);
        }
      }
      for (const PatternProgramEntry &pattern : module.module.pattern_programs) {
        if (next_pattern_id > std::numeric_limits<std::uint32_t>::max())
          throw std::overflow_error("merged pattern id space exhausted");
        state.pattern_ids.emplace(pattern.pattern_id,
                                  static_cast<std::uint32_t>(next_pattern_id++));
      }
      states_[module.name] = std::move(state);
      method_offset += static_cast<std::uint32_t>(module.module.methods.size());
      class_offset += static_cast<std::uint32_t>(module.module.classes.size());
    }

    for (const GraphModule &module : order_) {
      GraphModuleState &state = states_.at(module.name);
      copy_module(state);
    }

    synthesize_merged_init();
    // Manifest builds use the same explicit host-policy surface as source
    // builds: a CLI grant is both the package's requested capability and the
    // capability embedded into the generated executable. Without copying the
    // requests into the merged module, RuntimeWorld resolves an empty
    // effective set even though the executable carries the host grants.
    add_module_attr(&out_, "amber.build.graph", "merged-native-v1");
    add_module_attr(&out_, "amber.build.graph.root", root_module);
    add_module_attr(&out_, "amber.build.graph.modules",
                    std::to_string(order_.size()));
    normalize_string_vector(&out_.required_features);
    normalize_string_vector(&out_.optional_features);
    normalize_string_vector(&out_.forbidden_features);
    normalize_string_vector(&out_.replay_metadata.required_event_names);
    normalize_string_vector(&out_.replay_metadata.deterministic_sources);

    DecodeResult decoded = deserialize_module(
        serialize_module(out_));
    if (!decoded.ok()) {
      result.diagnostics.push_back(
          {"BuildError", verify_errors_to_json(decoded.errors),
           root_module, {}, {}});
      return result;
    }

    const auto root_found = states_.find(root_module);
    if (root_found == states_.end()) {
      result.diagnostics.push_back(
          {"BuildError",
           "root module is missing from native graph: " + root_module,
           root_module, {}, {}});
      return result;
    }
    const GraphModuleState &root_state = root_found->second;
    std::uint32_t root_main_code = 0;
    bool has_root_main = false;
    for (std::size_t i = 0; i < root_state.input->module.methods.size(); ++i) {
      const BcMethod &method =
          root_state.input->module.methods[i];
      const std::string selector =
          bc_symbol_or_empty(root_state.input->module, method.selector_sym_id);
      if (selector == "main" && method.params.empty() &&
          (method.flags & (kMethodFlagInstance |
                           kMethodFlagClass |
                           kMethodFlagPropertyGetter |
                           kMethodFlagPropertySetter)) == 0U) {
        const auto id_found = root_state.code_ids.find(method.entry_code_id);
        if (id_found != root_state.code_ids.end()) {
          root_main_code = id_found->second;
          has_root_main = true;
        }
        break;
      }
    }

    result.module = std::move(decoded.module);
    result.init_order.clear();
    for (const GraphModule &module : order_) {
      result.init_order.push_back(module.name);
    }
    result.has_entry_init_code_id = result.module.init.has_entry_code_id;
    result.entry_init_code_id = result.module.init.entry_code_id;
    result.has_entry_main_code_id = has_root_main;
    result.entry_main_code_id = root_main_code;
    result.exports = exports_;
    result.ok = true;
    return result;
  }

private:
  std::uint32_t intern_string(const std::string &text) {
    const auto found = string_ids_.find(text);
    if (found != string_ids_.end()) {
      return found->second;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(out_.strings.size());
    out_.strings.push_back(text);
    string_ids_[text] = id;
    return id;
  }

  std::uint32_t intern_symbol(const std::string &text) {
    const auto found = symbol_ids_.find(text);
    if (found != symbol_ids_.end()) {
      return found->second;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(out_.symbols.size());
    out_.symbols.push_back(text);
    symbol_ids_[text] = id;
    return id;
  }

  std::uint32_t intern_path_constant(const std::string &path) {
    Constant constant;
    constant.kind = ConstantKind::Path;
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = path.find('.', start);
      const std::string segment = end == std::string::npos
                                      ? path.substr(start)
                                      : path.substr(start, end - start);
      if (!segment.empty()) {
        constant.items.push_back(intern_symbol(segment));
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1U;
    }
    if (constant.items.empty()) {
      constant.items.push_back(intern_symbol(path));
    }
    const std::uint32_t id = static_cast<std::uint32_t>(out_.const_pool.size());
    out_.const_pool.push_back(std::move(constant));
    return id;
  }

  std::uint32_t map_string(GraphModuleState &state, std::uint32_t id) {
    const auto found = state.strings.find(id);
    if (found != state.strings.end()) {
      return found->second;
    }
    const std::uint32_t mapped =
        intern_string(bc_string_or_empty(state.input->module, id));
    state.strings[id] = mapped;
    return mapped;
  }

  std::uint32_t map_symbol(GraphModuleState &state, std::uint32_t id) {
    const auto found = state.symbols.find(id);
    if (found != state.symbols.end()) {
      return found->second;
    }
    const std::uint32_t mapped =
        intern_symbol(bc_symbol_or_empty(state.input->module, id));
    state.symbols[id] = mapped;
    return mapped;
  }

  std::uint32_t map_code(GraphModuleState &state,
                         std::uint32_t id) const {
    const auto found = state.code_ids.find(id);
    return found == state.code_ids.end() ? 0U : found->second;
  }

  std::string qualify_import_path(const std::string &path) const {
    std::string best_module;
    std::string best_export;
    for (const auto &[module_name, state] : states_) {
      const std::string prefix = module_name + ".";
      if (path.rfind(prefix, 0) != 0U ||
          module_name.size() <= best_module.size()) {
        continue;
      }
      const std::string export_name = path.substr(prefix.size());
      const auto found = state.exports.find(export_name);
      if (found != state.exports.end()) {
        best_module = module_name;
        best_export = export_name;
      }
    }
    if (!best_module.empty()) {
      return states_.at(best_module).exports.at(best_export).qualified_path;
    }
    return path;
  }

  std::uint32_t map_constant(GraphModuleState &state, std::uint32_t id) {
    const auto found = state.constants.find(id);
    if (found != state.constants.end()) {
      return found->second;
    }
    if (id >= state.input->module.const_pool.size()) {
      return 0;
    }
    Constant constant = state.input->module.const_pool[id];
    if (constant.kind == ConstantKind::Path && !constant.items.empty()) {
      std::vector<std::string> segments;
      segments.reserve(constant.items.size());
      for (const std::uint32_t item : constant.items) {
        const std::string segment =
            bc_symbol_or_empty(state.input->module, item);
        if (segment.empty()) {
          throw std::runtime_error(
              "path constant contains an invalid symbol in module " +
              state.input->name);
        }
        segments.push_back(segment);
      }
      std::string path;
      const auto alias = state.import_alias_paths.find(segments.front());
      if (alias != state.import_alias_paths.end()) {
        path = qualify_import_path(alias->second);
      } else {
        const auto local_class =
            state.qualified_class_names.find(segments.front());
        if (local_class != state.qualified_class_names.end()) {
          path = local_class->second;
        }
      }
      if (!path.empty()) {
        for (std::size_t i = 1; i < segments.size(); ++i) {
          path += "." + segments[i];
        }
        const std::uint32_t mapped = intern_path_constant(path);
        state.constants[id] = mapped;
        return mapped;
      }
    }
    switch (constant.kind) {
    case ConstantKind::SymbolRef:
      constant.ref_id = map_symbol(state, constant.ref_id);
      break;
    case ConstantKind::StringRef:
      constant.ref_id = map_string(state, constant.ref_id);
      break;
    case ConstantKind::CodeRef:
      constant.ref_id = map_code(state, constant.ref_id);
      break;
    case ConstantKind::KeySet:
    case ConstantKind::Path:
      for (std::uint32_t &item : constant.items) {
        item = map_symbol(state, item);
      }
      break;
    case ConstantKind::Null:
    case ConstantKind::Bool:
    case ConstantKind::Integer:
    case ConstantKind::Float:
      break;
    }
    const std::uint32_t mapped =
        static_cast<std::uint32_t>(out_.const_pool.size());
    out_.const_pool.push_back(std::move(constant));
    state.constants[id] = mapped;
    return mapped;
  }

  std::uint32_t class_index(GraphModuleState &state,
                            std::uint32_t old_index) const {
    return state.class_offset + old_index;
  }

  std::uint32_t method_index(GraphModuleState &state,
                             std::uint32_t old_index) const {
    return state.method_offset + old_index;
  }

  std::uint32_t pattern_index(GraphModuleState &state,
                              std::uint32_t old_id) const {
    const auto found = state.pattern_ids.find(old_id);
    if (found == state.pattern_ids.end())
      throw std::runtime_error("missing pattern metadata in module " +
                               state.input->name);
    return found->second;
  }

  static void normalize_string_vector(std::vector<std::string> *values) {
    std::sort(values->begin(), values->end());
    values->erase(std::unique(values->begin(), values->end()), values->end());
  }

  void remap_symbol_operand(GraphModuleState &state,
                            Instruction &insn,
                            std::size_t index) {
    if (index < insn.operands.size()) {
      insn.operands[index].value = map_symbol(
          state, static_cast<std::uint32_t>(insn.operands[index].value));
    }
  }

  void remap_constant_operand(GraphModuleState &state,
                              Instruction &insn,
                              std::size_t index) {
    if (index < insn.operands.size()) {
      insn.operands[index].value = map_constant(
          state, static_cast<std::uint32_t>(insn.operands[index].value));
    }
  }

  void shift_target(Instruction &insn, std::size_t index,
                    std::uint32_t offset) {
    if (offset == 0U || index >= insn.operands.size()) {
      return;
    }
    insn.operands[index].value += offset;
  }

  void remap_instruction(GraphModuleState &state,
                         Instruction &insn,
                         std::uint32_t pc_offset) {
        switch (insn.opcode) {
    case Opcode::LoadK:
      remap_constant_operand(state, insn, 1);
      break;
    case Opcode::LookupConst:
      remap_constant_operand(state, insn, 1);
      break;
    case Opcode::TypeCheck:
      remap_constant_operand(state, insn, 1);
      break;
    case Opcode::MakeClosure:
      if (insn.operands.size() > 1U) {
        insn.operands[1].value =
            map_code(state, static_cast<std::uint32_t>(insn.operands[1].value));
      }
      break;
    case Opcode::LoadIvar:
    case Opcode::LoadCvar:
      remap_symbol_operand(state, insn, 2);
      break;
    case Opcode::StoreIvar:
    case Opcode::StoreCvar:
      remap_symbol_operand(state, insn, 1);
      break;
    case Opcode::MakeMap: {
      if (insn.operands.size() >= 2U) {
        const std::uint32_t count =
            static_cast<std::uint32_t>(insn.operands[1].value) &
            kMapCountMask;
        for (std::uint32_t i = 0; i < count; ++i) {
          remap_symbol_operand(state, insn, 2U + i * 2U);
        }
      }
      break;
    }
    case Opcode::MakeMapSpread: {
      if (insn.operands.size() >= 2U) {
        const std::uint32_t count =
            static_cast<std::uint32_t>(insn.operands[1].value) &
            kMapCountMask;
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::size_t base = 2U + i * 3U;
          if (base + 1U < insn.operands.size() &&
              insn.operands[base].value ==
                  kMapSpreadEntrySymbol) {
            remap_symbol_operand(state, insn, base + 1U);
          }
        }
      }
      break;
    }
    case Opcode::Send:
    case Opcode::SendDyn:
    case Opcode::SendSpread:
    case Opcode::SendDynSpread: {
      const bool spread = insn.opcode == Opcode::SendSpread ||
                          insn.opcode == Opcode::SendDynSpread;
      if (insn.opcode == Opcode::Send || insn.opcode == Opcode::SendSpread) {
        remap_symbol_operand(state, insn, 2);
      }
      if (insn.operands.size() > 3U) {
        const std::uint32_t pos_count =
            static_cast<std::uint32_t>(insn.operands[3].value);
        const std::size_t per_arg = spread ? 2U : 1U;
        const std::size_t kw_index = 4U + pos_count * per_arg;
        if (kw_index < insn.operands.size()) {
          const std::uint32_t kw_count =
              static_cast<std::uint32_t>(insn.operands[kw_index].value);
          for (std::uint32_t i = 0; i < kw_count; ++i) {
            const std::size_t key_index =
                kw_index + 1U + i * (spread ? 3U : 2U) + (spread ? 1U : 0U);
            remap_symbol_operand(state, insn, key_index);
          }
        }
      }
      break;
    }
    case Opcode::CallSpread: {
      if (insn.operands.size() > 2U) {
        const std::uint32_t pos_count =
            static_cast<std::uint32_t>(insn.operands[2].value);
        const std::size_t kw_index = 3U + pos_count * 2U;
        if (kw_index < insn.operands.size()) {
          const std::uint32_t kw_count =
              static_cast<std::uint32_t>(insn.operands[kw_index].value);
          for (std::uint32_t i = 0; i < kw_count; ++i) {
            remap_symbol_operand(state, insn, kw_index + 1U + i * 3U + 1U);
          }
        }
      }
      break;
    }
    case Opcode::Call: {
      if (insn.operands.size() > 2U) {
        const std::uint32_t pos_count =
            static_cast<std::uint32_t>(insn.operands[2].value);
        const std::size_t kw_index = 3U + pos_count;
        if (kw_index < insn.operands.size()) {
          const std::uint32_t kw_count =
              static_cast<std::uint32_t>(insn.operands[kw_index].value);
          for (std::uint32_t i = 0; i < kw_count; ++i) {
            remap_symbol_operand(state, insn, kw_index + 1U + i * 2U);
          }
        }
      }
      break;
    }
    case Opcode::PCheckEq:
      remap_constant_operand(state, insn, 1);
      shift_target(insn, 2, pc_offset);
      break;
    case Opcode::PPrepSeq:
      shift_target(insn, 3, pc_offset);
      break;
    case Opcode::PPrepMap:
      remap_constant_operand(state, insn, 2);
      shift_target(insn, 4, pc_offset);
      break;
    case Opcode::PCheckPin:
    case Opcode::PCheckLenEq:
    case Opcode::PCheckLenGte:
    case Opcode::PTripleEq:
      shift_target(insn, 2, pc_offset);
      break;
    case Opcode::PHasKey:
      remap_symbol_operand(state, insn, 1);
      shift_target(insn, 2, pc_offset);
      break;
    case Opcode::PGetKey:
      remap_symbol_operand(state, insn, 2);
      break;
    case Opcode::Jump:
      shift_target(insn, 0, pc_offset);
      break;
    case Opcode::JumpIfTrue:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
      shift_target(insn, 1, pc_offset);
      break;
    default:
      if (integer_k_opcode(insn.opcode)) {
        remap_constant_operand(state, insn, 2);
      }
      break;
    }
  }

  std::optional<GraphExportRef>
  resolve_import(const std::string &dependency,
                 const std::string &source) const {
    const auto dep_state = states_.find(dependency);
    if (dep_state == states_.end()) {
      return std::nullopt;
    }
    const auto export_found = dep_state->second.exports.find(source);
    if (export_found == dep_state->second.exports.end()) {
      return std::nullopt;
    }
    return export_found->second;
  }

  void index_import_alias_paths(GraphModuleState &state) {
    const std::string prefix = "amber.import.alias:";
    for (const AttrEntry &attr : state.input->module.attrs) {
      const std::string key =
          bc_string_or_empty(state.input->module, attr.key_str_id);
      if (key.rfind(prefix, 0) != 0) {
        continue;
      }
      const std::vector<std::string> key_fields =
          split_tab_fields(key.substr(prefix.size()));
      if (key_fields.size() != 2U) {
        throw std::runtime_error("malformed import alias key in module " +
                                 state.input->name);
      }
      const std::string value =
          bc_string_or_empty(state.input->module, attr.value_str_id);
      const std::vector<std::string> fields = split_tab_fields(value);
      if (fields.empty()) {
        throw std::runtime_error("malformed import alias value in module " +
                                 state.input->name);
      }
      // Every alias must be part of the dependency graph. Only that graph's
      // traversal admits runtime-provided namespaces through the host policy.
      if (fields.size() < 2U || !std::any_of(
              state.input->module.dependencies.begin(),
              state.input->module.dependencies.end(),
              [&](const DepEntry &dependency) {
                return bc_string_or_empty(state.input->module,
                                          dependency.module_name_str_id) ==
                       fields[1];
              })) {
        throw std::runtime_error("import alias has no declared dependency in module " +
                                 state.input->name);
      }
      if (fields[0] == "M" && fields.size() == 3U) {
        state.import_alias_paths[key_fields[1]] = fields[1];
        continue;
      }
      if (fields[0] == "F" && fields.size() == 4U) {
        state.import_alias_paths[key_fields[1]] = fields[1] + "." + fields[2];
        continue;
      }
      throw std::runtime_error("unsupported import alias metadata in module " +
                               state.input->name);
    }
  }

  void index_local_classes(GraphModuleState &state) {
    for (const BcClass &klass : state.input->module.classes) {
      const std::string source_name =
          bc_symbol_or_empty(state.input->module, klass.class_name_sym_id);
      if (source_name.empty()) {
        throw std::runtime_error("class has an invalid name in module " +
                                 state.input->name);
      }
      const std::string prefix = state.input->name + ".";
      const std::string qualified =
          (klass.flags & kClassFlagNativeError) != 0U ||
                  source_name.rfind(prefix, 0) == 0U ? source_name
                                             : prefix + source_name;
      state.local_class_names.insert(source_name);
      state.qualified_class_names[source_name] = qualified;
    }
  }

  std::vector<Instruction>
  import_seed_instructions(GraphModuleState &state) {
    std::vector<Instruction> seeds;
    const std::string prefix = "amber.import.alias:";
    for (const AttrEntry &attr : state.input->module.attrs) {
      const std::string key =
          bc_string_or_empty(state.input->module, attr.key_str_id);
      const std::string value =
          bc_string_or_empty(state.input->module, attr.value_str_id);
      if (key.rfind(prefix, 0) != 0) {
        continue;
      }
      const std::vector<std::string> fields = split_tab_fields(value);
      if (fields.empty()) {
        throw std::runtime_error("malformed import alias value in module " +
                                 state.input->name);
      }
      if (fields[0] == "M") {
        if (fields.size() != 3U) {
          throw std::runtime_error("malformed module import alias in module " +
                                   state.input->name);
        }
        std::uint32_t slot = 0;
        if (!parse_u32_text(fields[2], &slot)) {
          throw std::runtime_error("invalid module import slot in module " +
                                   state.input->name);
        }
        const auto dep_state = states_.find(fields[1]);
        if (dep_state == states_.end()) {
          const std::uint32_t const_id = intern_path_constant(fields[1]);
          seeds.push_back({Opcode::LookupConst,
                           {{slot, false}, {const_id, false}}});
          continue;
        }
        if (!dep_state->second.input->stdlib) {
          throw std::runtime_error(
              "whole-graph native does not support non-stdlib module import "
              "aliases in executable package objects: " +
              key.substr(prefix.size()));
        }
        const std::uint32_t const_id = intern_path_constant(fields[1]);
        seeds.push_back({Opcode::LookupConst,
                         {{slot, false}, {const_id, false}}});
        continue;
      }
      if (fields[0] != "F" || fields.size() != 4U) {
        throw std::runtime_error("unsupported import alias metadata in module " +
                                 state.input->name);
      }
      std::uint32_t slot = 0;
      if (!parse_u32_text(fields[3], &slot)) {
        throw std::runtime_error("invalid named import slot in module " +
                                 state.input->name);
      }
      const std::optional<GraphExportRef> target =
          resolve_import(fields[1], fields[2]);
      if (!target.has_value()) {
        if (states_.find(fields[1]) == states_.end()) {
          const std::uint32_t const_id =
              intern_path_constant(fields[1] + "." + fields[2]);
          seeds.push_back({Opcode::LookupConst,
                           {{slot, false}, {const_id, false}}});
          continue;
        }
        throw std::runtime_error("whole-graph native import `" + fields[2] +
                                 "` from module `" + fields[1] +
                                 "` is not resolved");
      }
      // Resolve the import to the exporting module's ALREADY-BUILT binding
      // (via LookupConst -> module_bindings) rather than rebuilding a closure
      // here. A rebuilt `MakeClosure(code_id, 0 captures)` dropped the callee's
      // upvalues, so an imported function that closes over other module-level
      // bindings ran with no captures ("capture slot out of range"). The
      // exporting module's init runs first (dependency order) and persists the
      // real closure (with captures) under `<module>:<name>`, which the
      // extended lookup_constant recovers. Classes resolve the same way.
      if (target->kind == "method" || target->kind == "code" ||
          target->kind == "class") {
        const std::uint32_t const_id = intern_path_constant(target->qualified_path);
        seeds.push_back({Opcode::LookupConst,
                         {{slot, false}, {const_id, false}}});
      }
    }
    return seeds;
  }

  void shift_instruction_targets_from(BcCode *code,
                                      std::uint32_t from_pc,
                                      std::uint32_t offset) {
    if (offset == 0U) {
      return;
    }
    const auto shift_operand = [&](Instruction &insn,
                                   std::size_t index) {
      if (index >= insn.operands.size() ||
          insn.operands[index].signed_immediate ||
          insn.operands[index].value < 0) {
        return;
      }
      const std::uint32_t target =
          static_cast<std::uint32_t>(insn.operands[index].value);
      if (target >= from_pc) {
        insn.operands[index].value = static_cast<std::int64_t>(target + offset);
      }
    };
    for (Instruction &insn : code->instructions) {
      switch (insn.opcode) {
      case Opcode::Jump:
        shift_operand(insn, 0);
        break;
      case Opcode::JumpIfTrue:
      case Opcode::JumpIfFalse:
      case Opcode::JumpIfNull:
        shift_operand(insn, 1);
        break;
      case Opcode::PPrepSeq:
        shift_operand(insn, 3);
        break;
      case Opcode::PPrepMap:
        shift_operand(insn, 4);
        break;
      case Opcode::PCheckEq:
      case Opcode::PCheckPin:
      case Opcode::PCheckLenEq:
      case Opcode::PCheckLenGte:
      case Opcode::PHasKey:
      case Opcode::PTripleEq:
        shift_operand(insn, 2);
        break;
      default:
        break;
      }
    }
  }

  std::uint32_t
  import_seed_insert_pc(const BcCode &code) const {
    std::set<std::uint32_t> import_slots;
    for (const SlotLayoutEntry &entry : code.local_layout) {
      const std::string binding =
          entry.binding_kind_str_id < out_.strings.size()
              ? out_.strings[entry.binding_kind_str_id]
              : std::string();
      if (binding == "import_alias") {
        import_slots.insert(entry.slot);
      }
    }
    std::uint32_t pc = 0;
    while (pc < code.instructions.size()) {
      const Instruction &insn = code.instructions[pc];
      if (insn.opcode != Opcode::LoadNull ||
          insn.operands.empty() || insn.operands[0].signed_immediate ||
          insn.operands[0].value < 0) {
        break;
      }
      const std::uint32_t slot =
          static_cast<std::uint32_t>(insn.operands[0].value);
      if (import_slots.find(slot) == import_slots.end()) {
        break;
      }
      ++pc;
    }
    return pc;
  }

  void shift_code_pcs(BcCode *code, std::uint32_t offset,
                      std::uint32_t from_pc = 0) {
    if (offset == 0U) {
      return;
    }
    for (HandlerEntry &entry : code->handler_table) {
      if (entry.protected_from >= from_pc) {
        entry.protected_from += offset;
      }
      if (entry.protected_to > from_pc) {
        entry.protected_to += offset;
      }
      if (entry.handler_pc >= from_pc) {
        entry.handler_pc += offset;
      }
    }
    for (CacheSiteEntry &entry : code->call_site_table) {
      if (entry.pc >= from_pc) {
        entry.pc += offset;
      }
    }
    for (CacheSiteEntry &entry : code->ivar_site_table) {
      if (entry.pc >= from_pc) {
        entry.pc += offset;
      }
    }
    for (SafepointEntry &entry : code->safepoint_table) {
      if (entry.pc >= from_pc) {
        entry.pc += offset;
      }
    }
    for (SourceSpanEntry &entry : code->source_spans) {
      if (entry.pc_from >= from_pc) {
        entry.pc_from += offset;
      }
      if (entry.pc_to > from_pc) {
        entry.pc_to += offset;
      }
    }
  }

  BcCode remap_code(GraphModuleState &state,
                                     const BcCode &source,
                                     std::uint32_t pc_offset) {
    BcCode code = source;
    code.code_id = map_code(state, source.code_id);
    for (SlotLayoutEntry &entry : code.local_layout) {
      entry.name_str_id = map_string(state, entry.name_str_id);
      entry.role_str_id = map_string(state, entry.role_str_id);
      entry.binding_kind_str_id = map_string(state, entry.binding_kind_str_id);
    }
    for (CaptureLayoutEntry &entry : code.capture_layout) {
      entry.name_str_id = map_string(state, entry.name_str_id);
      entry.source_kind_str_id = map_string(state, entry.source_kind_str_id);
      entry.source_name_str_id = map_string(state, entry.source_name_str_id);
    }
    for (Instruction &insn : code.instructions) {
      remap_instruction(state, insn, pc_offset);
    }
    for (HandlerEntry &entry : code.handler_table) {
      entry.handler_code_id = map_code(state, entry.handler_code_id);
    }
    for (CacheSiteEntry &entry : code.call_site_table) {
      entry.symbol_id = map_symbol(state, entry.symbol_id);
    }
    for (CacheSiteEntry &entry : code.ivar_site_table) {
      entry.symbol_id = map_symbol(state, entry.symbol_id);
    }
    shift_code_pcs(&code, pc_offset);
    return code;
  }

  void copy_module(GraphModuleState &state) {
    if (!state.input->module.hashes.empty()) {
      throw std::runtime_error("source hash metadata is unsupported when merging module " +
                               state.input->name);
    }
    if (state.input->module.notebook_metadata.has_value()) {
      throw std::runtime_error(
          "notebook metadata is unsupported when merging module " +
          state.input->name);
    }
    out_.profile_flags |= state.input->module.profile_flags;
    out_.required_features.insert(out_.required_features.end(),
                                  state.input->module.required_features.begin(),
                                  state.input->module.required_features.end());
    out_.optional_features.insert(out_.optional_features.end(),
                                  state.input->module.optional_features.begin(),
                                  state.input->module.optional_features.end());
    out_.forbidden_features.insert(
        out_.forbidden_features.end(),
        state.input->module.forbidden_features.begin(),
        state.input->module.forbidden_features.end());
    out_.capabilities.insert(out_.capabilities.end(),
                             state.input->module.capabilities.begin(),
                             state.input->module.capabilities.end());
    out_.observability_sites.insert(
        out_.observability_sites.end(),
        state.input->module.observability_sites.begin(),
        state.input->module.observability_sites.end());
    out_.replay_metadata.required_event_names.insert(
        out_.replay_metadata.required_event_names.end(),
        state.input->module.replay_metadata.required_event_names.begin(),
        state.input->module.replay_metadata.required_event_names.end());
    out_.replay_metadata.deterministic_sources.insert(
        out_.replay_metadata.deterministic_sources.end(),
        state.input->module.replay_metadata.deterministic_sources.begin(),
        state.input->module.replay_metadata.deterministic_sources.end());
    out_.replay_metadata.flags |= state.input->module.replay_metadata.flags;
    out_.effects.insert(out_.effects.end(), state.input->module.effects.begin(),
                        state.input->module.effects.end());
    out_.schemas.insert(out_.schemas.end(), state.input->module.schemas.begin(),
                        state.input->module.schemas.end());
    out_.schema_migrations.insert(out_.schema_migrations.end(),
                                  state.input->module.schema_migrations.begin(),
                                  state.input->module.schema_migrations.end());
    out_.table_plans.insert(out_.table_plans.end(),
                            state.input->module.table_plans.begin(),
                            state.input->module.table_plans.end());
    out_.wasm_components.insert(out_.wasm_components.end(),
                                state.input->module.wasm_components.begin(),
                                state.input->module.wasm_components.end());
    out_.accelerator_kernels.insert(
        out_.accelerator_kernels.end(),
        state.input->module.accelerator_kernels.begin(),
        state.input->module.accelerator_kernels.end());
    out_.agent_symbols.insert(out_.agent_symbols.end(),
                              state.input->module.agent_symbols.begin(),
                              state.input->module.agent_symbols.end());
    out_.agent_patches.insert(out_.agent_patches.end(),
                              state.input->module.agent_patches.begin(),
                              state.input->module.agent_patches.end());
    out_.provenance_records.insert(
        out_.provenance_records.end(),
        state.input->module.provenance_records.begin(),
        state.input->module.provenance_records.end());
    out_.contracts.insert(out_.contracts.end(),
                          state.input->module.contracts.begin(),
                          state.input->module.contracts.end());
    out_.properties.insert(out_.properties.end(),
                           state.input->module.properties.begin(),
                           state.input->module.properties.end());
    out_.privacy_labels.insert(out_.privacy_labels.end(),
                               state.input->module.privacy_labels.begin(),
                               state.input->module.privacy_labels.end());
    out_.privacy_policies.insert(out_.privacy_policies.end(),
                                 state.input->module.privacy_policies.begin(),
                                 state.input->module.privacy_policies.end());
    out_.lineage_nodes.insert(out_.lineage_nodes.end(),
                              state.input->module.lineage_nodes.begin(),
                              state.input->module.lineage_nodes.end());
    out_.workflow_steps.insert(out_.workflow_steps.end(),
                               state.input->module.workflow_steps.begin(),
                               state.input->module.workflow_steps.end());
    out_.workflow_history.insert(out_.workflow_history.end(),
                                 state.input->module.workflow_history.begin(),
                                 state.input->module.workflow_history.end());

    index_import_alias_paths(state);
    index_local_classes(state);

    for (std::uint32_t i = 0; i < state.input->module.const_pool.size(); ++i) {
      map_constant(state, i);
    }

    std::vector<Instruction> import_seeds =
        import_seed_instructions(state);
    for (const BcCode &source_code :
         state.input->module.code_objects) {
      const bool is_init =
          state.input->module.init.has_entry_code_id &&
          source_code.code_id == state.input->module.init.entry_code_id;
      state.code_pc_offsets[source_code.code_id] = 0;
      state.code_pc_insert_points[source_code.code_id] = 0;
      BcCode code = remap_code(state, source_code, 0);
      if (is_init && !import_seeds.empty()) {
        const std::uint32_t insert_pc = import_seed_insert_pc(code);
        const std::uint32_t pc_offset =
            static_cast<std::uint32_t>(import_seeds.size());
        state.code_pc_offsets[source_code.code_id] = pc_offset;
        state.code_pc_insert_points[source_code.code_id] = insert_pc;
        shift_instruction_targets_from(&code, insert_pc, pc_offset);
        shift_code_pcs(&code, pc_offset, insert_pc);
        code.instructions.insert(code.instructions.begin() + insert_pc,
                                 import_seeds.begin(), import_seeds.end());
      }
      out_.code_objects.push_back(std::move(code));
    }

    for (const BcMethod &source :
         state.input->module.methods) {
      BcMethod method = source;
      method.selector_sym_id = map_symbol(state, method.selector_sym_id);
      if ((method.flags & (kMethodFlagInstance |
                           kMethodFlagClass |
                           kMethodFlagPropertyGetter |
                           kMethodFlagPropertySetter)) != 0U) {
        method.owner_dispatch_ref =
            class_index(state, method.owner_dispatch_ref);
      }
      method.signature_blob_id = map_constant(state, method.signature_blob_id);
      for (MethodParamEntry &param : method.params) {
        param.external_name_sym_id =
            map_symbol(state, param.external_name_sym_id);
        param.local_name_str_id = map_string(state, param.local_name_str_id);
      }
      for (std::uint32_t &code_id : method.default_thunk_ids) {
        code_id = map_code(state, code_id);
      }
      for (std::uint32_t &code_id : method.type_hook_ids) {
        code_id = map_code(state, code_id);
      }
      for (ClauseEntry &entry : method.clause_table) {
        entry.pattern_program_id =
            pattern_index(state, entry.pattern_program_id);
        entry.pattern_code_id = map_code(state, entry.pattern_code_id);
        entry.guard_code_id = map_code(state, entry.guard_code_id);
        entry.body_code_id = map_code(state, entry.body_code_id);
      }
      for (AutoAssignEntry &entry : method.auto_assign_desc) {
        entry.local_name_str_id = map_string(state, entry.local_name_str_id);
        entry.target_name_str_id = map_string(state, entry.target_name_str_id);
      }
      method.entry_code_id = map_code(state, method.entry_code_id);
      out_.methods.push_back(std::move(method));
    }

    for (const BcClass &source : state.input->module.classes) {
      BcClass klass = source;
      const std::string source_class_name =
          bc_symbol_or_empty(state.input->module, source.class_name_sym_id);
      const auto class_name = state.qualified_class_names.find(source_class_name);
      klass.class_name_sym_id =
          intern_symbol(class_name == state.qualified_class_names.end()
                            ? source_class_name
                            : class_name->second);
      if (klass.has_superclass_ref) {
        klass.superclass_ref = map_constant(state, klass.superclass_ref);
      }
      klass.ivar_schema_id = map_constant(state, klass.ivar_schema_id);
      klass.method_range_start = method_index(state, klass.method_range_start);
      for (std::uint32_t &ref : klass.direct_include_refs) {
        ref = map_constant(state, ref);
      }
      for (std::uint32_t &ref : klass.direct_extend_refs) {
        ref = map_constant(state, ref);
      }
      if (klass.has_class_init_code_id) {
        klass.class_init_code_id = map_code(state, klass.class_init_code_id);
      }
      out_.classes.push_back(std::move(klass));
    }

    for (const PatternProgramEntry &source :
         state.input->module.pattern_programs) {
      PatternProgramEntry entry = source;
      entry.pattern_id = pattern_index(state, source.pattern_id);
      out_.pattern_programs.push_back(entry);
    }

    for (const ExportEntry &source :
         state.input->module.exports) {
      ExportEntry entry = source;
      entry.symbol_id = map_symbol(state, entry.symbol_id);
      entry.target_kind_str_id = map_string(state, entry.target_kind_str_id);
      entry.reexport_module_name_str_id =
          map_string(state, entry.reexport_module_name_str_id);
      const std::string kind =
          bc_string_or_empty(state.input->module, source.target_kind_str_id);
      const std::string export_name =
          bc_symbol_or_empty(state.input->module, source.symbol_id);
      const bool reexport = source.has_reexport_module_name || kind == "reexport";
      if (!reexport && kind != "method" && kind != "code" && kind != "class") {
        throw std::runtime_error("unsupported export target kind '" + kind +
                                 "' in module " + state.input->name);
      }
      if (export_name.empty() || state.exports.find(export_name) != state.exports.end()) {
        throw std::runtime_error("duplicate or invalid export '" + export_name +
                                 "' in module " + state.input->name);
      }
      GraphExportRef ref;
      ref.kind = kind;
      if (reexport) {
        const std::string dependency = bc_string_or_empty(
            state.input->module, source.reexport_module_name_str_id);
        const std::string original = kind == "reexport"
            ? bc_string_or_empty(state.input->module, source.target_index) : export_name;
        const bool declared = std::any_of(state.input->module.dependencies.begin(),
            state.input->module.dependencies.end(), [&](const DepEntry &dep) {
              return bc_string_or_empty(state.input->module, dep.module_name_str_id) == dependency;
            });
        const auto resolved = declared ? resolve_import(dependency, original) : std::nullopt;
        if (!resolved.has_value()) {
          throw std::runtime_error("unresolved re-export: " + state.input->name +
                                   "." + export_name + " from " + dependency + "." + original);
        }
        ref = *resolved;
        entry.target_kind_str_id = intern_string(ref.kind);
        entry.target_index = ref.kind == "class" ? ref.class_index :
                             ref.kind == "method" ? ref.method_index : ref.code_id;
        entry.has_reexport_module_name = false;
        entry.reexport_module_name_str_id = 0;
      } else if (kind == "method") {
        if (source.target_index >= state.input->module.methods.size()) {
          throw std::runtime_error("method export index is out of range in module " +
                                   state.input->name);
        }
        entry.target_index = method_index(state, source.target_index);
        const BcMethod &method =
            state.input->module.methods[source.target_index];
        ref.method_index = entry.target_index;
        ref.code_id = map_code(state, method.entry_code_id);
        ref.qualified_path =
            state.input->name + "." +
            bc_symbol_or_empty(state.input->module, method.selector_sym_id);
      } else if (kind == "code") {
        if (map_code(state, source.target_index) == 0U) {
          throw std::runtime_error("code export index is out of range in module " +
                                   state.input->name);
        }
        entry.target_index = map_code(state, source.target_index);
        ref.code_id = entry.target_index;
        ref.qualified_path = state.input->name + "." + export_name;
      } else if (kind == "class") {
        if (source.target_index >= state.input->module.classes.size()) {
          throw std::runtime_error("class export index is out of range in module " +
                                   state.input->name);
        }
        entry.target_index = class_index(state, source.target_index);
        ref.class_index = entry.target_index;
        const BcClass &klass = state.input->module.classes[source.target_index];
        const std::string class_name =
            bc_symbol_or_empty(state.input->module, klass.class_name_sym_id);
        const auto qualified = state.qualified_class_names.find(class_name);
        ref.qualified_path = qualified == state.qualified_class_names.end()
                                 ? state.input->name + "." + class_name
                                 : qualified->second;
      }
      out_.exports.push_back(entry);
      state.exports[export_name] = ref;
      exports_.push_back({state.input->name, export_name, ref.qualified_path,
                          ref.kind, ref.code_id, ref.method_index,
                          ref.class_index});
      if ((ref.kind == "method" || ref.kind == "code") &&
          ref.code_id != 0U) {
        add_module_attr(
            &out_, "amber.merged.export:" + state.input->name + "." +
                       export_name,
            ref.kind + "\t" + std::to_string(ref.code_id));
      } else if (ref.kind == "class") {
        add_module_attr(
            &out_, "amber.merged.export:" + state.input->name + "." +
                       export_name,
            "class\t" + std::to_string(ref.class_index));
      }
    }

    for (const LineEntry &source :
         state.input->module.line_table) {
      LineEntry entry = source;
      entry.code_id = map_code(state, source.code_id);
      const auto offset = state.code_pc_offsets.find(source.code_id);
      const auto insert = state.code_pc_insert_points.find(source.code_id);
      const std::uint32_t insert_pc =
          insert == state.code_pc_insert_points.end() ? 0U : insert->second;
      if (offset != state.code_pc_offsets.end() && entry.pc >= insert_pc) {
        entry.pc += offset->second;
      }
      out_.line_table.push_back(entry);
    }
    for (const LocalDebugEntry &source :
         state.input->module.local_debug) {
      LocalDebugEntry entry = source;
      entry.code_id = map_code(state, source.code_id);
      entry.name_str_id = map_string(state, source.name_str_id);
      const auto offset = state.code_pc_offsets.find(source.code_id);
      const auto insert = state.code_pc_insert_points.find(source.code_id);
      const std::uint32_t insert_pc =
          insert == state.code_pc_insert_points.end() ? 0U : insert->second;
      if (offset != state.code_pc_offsets.end()) {
        if (entry.start_pc >= insert_pc) {
          entry.start_pc += offset->second;
        }
        if (entry.end_pc > insert_pc) {
          entry.end_pc += offset->second;
        }
      }
      out_.local_debug.push_back(entry);
    }

    for (const AttrEntry &source : state.input->module.attrs) {
      const std::string raw_key =
          bc_string_or_empty(state.input->module, source.key_str_id);
      const std::string raw_value =
          bc_string_or_empty(state.input->module, source.value_str_id);
      if (raw_key.rfind("amber.import.alias:", 0) == 0) {
        continue;
      }
      std::string key = raw_key;
      if (raw_key.rfind("amber.native.bind:", 0) == 0) {
        std::uint32_t old_code = 0;
        if (parse_u32_text(
                raw_key.substr(std::string("amber.native.bind:").size()),
                &old_code)) {
          key =
              "amber.native.bind:" + std::to_string(map_code(state, old_code));
        }
      }
      out_.attrs.push_back({intern_string(key), intern_string(raw_value)});
    }
    if (state.input->module.init.has_entry_code_id) {
      add_module_attr(&out_,
                      "amber.merged.init:" +
                          std::to_string(map_code(
                              state, state.input->module.init.entry_code_id)),
                      state.input->name);
      init_code_ids_.push_back(
          map_code(state, state.input->module.init.entry_code_id));
    }
  }

  void synthesize_merged_init() {
    const std::uint32_t wrapper_id = next_code_id_++;
    BcCode wrapper;
    wrapper.code_id = wrapper_id;
    wrapper.kind = CodeKind::Module;
    wrapper.reg_count = 2;
    std::uint32_t pc = 0;
    for (const std::uint32_t code_id : init_code_ids_) {
      wrapper.instructions.push_back(
          {Opcode::MakeClosure,
           {{0, false}, {code_id, false}, {0, false}}});
      wrapper.instructions.push_back(
          {Opcode::Call,
           {{1, false},
            {0, false},
            {0, false},
            {0, false},
            {-1, true},
            {static_cast<std::int64_t>(wrapper.call_site_table.size()),
             false}}});
      wrapper.call_site_table.push_back(
          {pc + 1U, static_cast<std::uint32_t>(wrapper.call_site_table.size()),
           intern_symbol("<call>"), 0});
      pc += 2U;
    }
    if (init_code_ids_.empty()) {
      wrapper.instructions.push_back(
          {Opcode::LoadNull, {{1, false}}});
    }
    wrapper.instructions.push_back(
        {Opcode::Return, {{1, false}}});
    out_.code_objects.push_back(std::move(wrapper));
    out_.init = {true, wrapper_id, 0};
    add_module_attr(&out_, "amber.merged.wrapper:" + std::to_string(wrapper_id),
                    "true");
  }

  std::vector<GraphModule> order_;
  BcModule out_;
  std::unordered_map<std::string, GraphModuleState> states_;
  std::unordered_map<std::string, std::uint32_t> string_ids_;
  std::unordered_map<std::string, std::uint32_t> symbol_ids_;
  std::vector<GraphExport> exports_;
  std::vector<std::uint32_t> init_code_ids_;
  std::uint32_t next_code_id_ = 1;
};

} // namespace

namespace {

GraphLinkDiagnostic graph_error(const std::string &message,
                                const std::string &module = {},
                                const std::string &dependency = {}) {
  return {"BuildError", message, module, dependency, {}};
}

bool validate_reachable_modules(
    const std::vector<GraphModule> &order,
    std::vector<GraphLinkDiagnostic> *diagnostics) {
  bool valid = true;
  for (const GraphModule &module : order) {
    // The linker emits one ordinary merged image. Notebook sidecars and any
    // future file-level flags have no safe merge semantics yet.
    if (module.module.file_flags != 0U) {
      diagnostics->push_back(graph_error(
          "unsupported nonzero file flags in native graph module: " +
              module.name,
          module.name));
      valid = false;
      continue;
    }
    try {
      const DecodeResult decoded =
          deserialize_module(serialize_module(module.module));
      if (!decoded.ok()) {
        diagnostics->push_back(graph_error(
            "native graph module failed bytecode verification: " +
                verify_errors_to_json(decoded.errors),
            module.name));
        valid = false;
      }
    } catch (const std::exception &error) {
      diagnostics->push_back(
          graph_error("native graph module verification failed: " +
                          std::string(error.what()),
                      module.name));
      valid = false;
    }
  }
  return valid;
}

bool validate_reachable_dependencies(
    const std::vector<GraphModule> &order,
    const std::map<std::string, const GraphModule *> &by_name,
    std::vector<GraphLinkDiagnostic> *diagnostics) {
  bool valid = true;
  for (const GraphModule &module : order) {
    for (const DepEntry &dependency : module.module.dependencies) {
      const std::string dependency_name =
          module.module.strings[dependency.module_name_str_id];
      const auto found = by_name.find(dependency_name);
      if (found == by_name.end()) {
        // Missing dependencies have already been admitted as runtime-provided
        // namespaces by the DFS predicate, so there is no bytecode artifact
        // whose compatibility metadata can be checked here.
        continue;
      }
      const BcModule &dep_module = found->second->module;
      if (version_less(dep_module.format_version,
                       dependency.required_format)) {
        diagnostics->push_back(graph_error(
            "module '" + module.name + "' requires module '" +
                dependency_name + "' bytecode format >= " +
                std::to_string(dependency.required_format.major) + "." +
                std::to_string(dependency.required_format.minor) + ", got " +
                std::to_string(dep_module.format_version.major) + "." +
                std::to_string(dep_module.format_version.minor),
            module.name, dependency_name));
        valid = false;
      }
      if (version_less(dep_module.language_version,
                       dependency.min_language_version)) {
        diagnostics->push_back(graph_error(
            "module '" + module.name + "' requires module '" +
                dependency_name + "' language >= " +
                std::to_string(dependency.min_language_version.major) + "." +
                std::to_string(dependency.min_language_version.minor) +
                ", got " + std::to_string(dep_module.language_version.major) +
                "." + std::to_string(dep_module.language_version.minor),
            module.name, dependency_name));
        valid = false;
      }
      if (dependency.has_max_language_version &&
          version_greater(dep_module.language_version,
                          dependency.max_language_version)) {
        diagnostics->push_back(graph_error(
            "module '" + module.name + "' requires module '" +
                dependency_name + "' language <= " +
                std::to_string(dependency.max_language_version.major) + "." +
                std::to_string(dependency.max_language_version.minor) +
                ", got " + std::to_string(dep_module.language_version.major) +
                "." + std::to_string(dep_module.language_version.minor),
            module.name, dependency_name));
        valid = false;
      }
      if (dependency.has_abi_requirement &&
          !abi_hash_is_zero(dependency.abi_requirement) &&
          dependency.abi_requirement != dep_module.abi_hash) {
        diagnostics->push_back(graph_error(
            "module '" + module.name + "' requires module '" +
                dependency_name + "' ABI hash that does not match the linked "
                "module",
            module.name, dependency_name));
        valid = false;
      }
    }
  }
  return valid;
}

} // namespace

GraphLinkResult link_graph(const std::vector<GraphModule> &modules,
                           const std::vector<std::string> &roots,
                           ExternalNamespacePredicate is_external_namespace) {
  GraphLinkResult result;
  if (roots.empty()) {
    result.diagnostics.push_back(graph_error("native graph has no roots"));
    return result;
  }

  std::map<std::string, const GraphModule *> by_name;
  for (const GraphModule &module : modules) {
    if (module.name.empty()) {
      result.diagnostics.push_back(graph_error("native graph contains a module with an empty name"));
      continue;
    }
    if (!by_name.emplace(module.name, &module).second) {
      result.diagnostics.push_back(
          graph_error("duplicate module in native graph: " + module.name,
                      module.name));
    }
  }
  if (!result.diagnostics.empty()) {
    return result;
  }

  std::set<std::string> visiting;
  std::set<std::string> visited;
  std::set<std::string> seen_roots;
  std::vector<GraphModule> order;
  std::function<bool(const std::string &, const std::string &)> visit =
      [&](const std::string &name, const std::string &from) {
        if (visited.count(name) != 0U) {
          return true;
        }
        if (!visiting.insert(name).second) {
          result.diagnostics.push_back(
              graph_error("module dependency cycle in native graph: " + name,
                          from, name));
          return false;
        }
        const auto found = by_name.find(name);
        if (found == by_name.end()) {
          visiting.erase(name);
          if (is_external_namespace && is_external_namespace(name)) {
            visited.insert(name);
            return true;
          }
          result.diagnostics.push_back(
              graph_error("module dependency is missing from native graph: " +
                              name,
                          from, name));
          return false;
        }
        const GraphModule &module = *found->second;
        std::vector<std::string> dependencies;
        for (const DepEntry &dep : module.module.dependencies) {
          if (dep.module_name_str_id >= module.module.strings.size()) {
            result.diagnostics.push_back(
                graph_error("dependency name index is out of range", name));
            visiting.erase(name);
            return false;
          }
          const std::string dependency =
              module.module.strings[dep.module_name_str_id];
          if (dependency.empty()) {
            result.diagnostics.push_back(
                graph_error("module has an empty dependency name", name));
            visiting.erase(name);
            return false;
          }
          dependencies.push_back(dependency);
        }
        std::set<std::string> seen_dependencies;
        dependencies.erase(
            std::remove_if(dependencies.begin(), dependencies.end(),
                           [&seen_dependencies](const std::string &dependency) {
                             return !seen_dependencies.insert(dependency).second;
                           }),
            dependencies.end());
        for (const std::string &dependency : dependencies) {
          if (!visit(dependency, name)) {
            visiting.erase(name);
            return false;
          }
        }
        visiting.erase(name);
        visited.insert(name);
        order.push_back(module);
        return true;
      };

  for (const std::string &root : roots) {
    if (root.empty()) {
      result.diagnostics.push_back(graph_error("native graph has an empty root name"));
      continue;
    }
    if (!seen_roots.insert(root).second) {
      result.diagnostics.push_back(
          graph_error("duplicate native graph root: " + root, root));
      continue;
    }
    if (by_name.find(root) == by_name.end()) {
      result.diagnostics.push_back(
          graph_error("root module is missing from native graph: " + root,
                      root));
      continue;
    }
    if (!visit(root, root)) {
      continue;
    }
  }
  if (!result.diagnostics.empty()) {
    return result;
  }

  if (!validate_reachable_modules(order, &result.diagnostics) ||
      !validate_reachable_dependencies(order, by_name, &result.diagnostics)) {
    return result;
  }

  try {
    return GraphModuleBuilder(std::move(order)).build(roots.front());
  } catch (const std::exception &error) {
    result.diagnostics.push_back(graph_error(error.what(), roots.front()));
    return result;
  }
}

GraphLinkResult link_graph(const std::vector<GraphModule> &modules,
                           const std::string &root,
                           ExternalNamespacePredicate is_external_namespace) {
  return link_graph(modules, std::vector<std::string>{root},
                    std::move(is_external_namespace));
}

} // namespace amber::bytecode
