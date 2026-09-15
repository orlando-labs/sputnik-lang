#include "tools/iamber/session.h"

#include "bytecode/graph_linker.h"
#include "runtime/stdlib_registry.h"

#include <exception>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

BundledEnvironmentPreparation prepare_bundled_environment(
    const std::vector<BundledModuleSource> &sources) {
  BundledEnvironmentPreparation result;
  std::map<std::string, const BundledModuleSource *> by_id;
  std::vector<std::string> roots;
  std::set<std::string> selected;
  for (const BundledModuleSource &source : sources) {
    if (source.id.empty() || source.path.empty()) {
      result.diagnostics.push_back(
          {source.id, source.path, "bundled module identity is incomplete", {}});
    } else if (!by_id.emplace(source.id, &source).second) {
      result.diagnostics.push_back({source.id, source.path,
                                    "duplicate bundled module id: " + source.id,
                                    {}});
    }
    if (source.auto_import && selected.insert(source.id).second)
      roots.push_back(source.id);
  }
  if (!result.diagnostics.empty())
    return result;
  if (roots.empty()) {
    result.ok = true;
    return result;
  }

  amber::runtime::RuntimeModuleRegistry native_modules;
  amber::runtime::RuntimeDispatchRegistry native_dispatch;
  amber::runtime::RuntimeTypeRegistry native_types;
  amber::runtime::register_builtin_runtime_modules(
      native_modules, native_dispatch, native_types);
  amber::runtime::register_core_prelude_bindings(native_modules);
  amber::runtime::register_legacy_native_type_paths(native_modules);

  // Only compile modules reachable from the selected roots. An unfinished
  // unrelated source buffer is allowed to coexist in the same project.
  std::map<std::string, unsigned> visited;
  std::vector<amber::bytecode::GraphModule> graph_modules;
  std::function<bool(const std::string &)> compile = [&](const std::string &id) {
    const auto found = by_id.find(id);
    if (found == by_id.end()) {
      if (native_modules.has_namespace(id))
        return true;
      result.diagnostics.push_back(
          {id, {}, "missing bundled module dependency: " + id, {}});
      return false;
    }
    const BundledModuleSource &source = *found->second;
    if (visited[id] == 2U)
      return true;
    if (visited[id] == 1U) {
      result.diagnostics.push_back(
          {id, source.path, "bundled module dependency cycle: " + id, {}});
      return false;
    }
    visited[id] = 1U;
    CompileResult compiled;
    try {
      compiled = compile_source_text(source.source, source.path, source.id);
    } catch (const std::exception &error) {
      result.diagnostics.push_back({id, source.path, error.what(), {}});
      return false;
    }
    if (!compiled.ok) {
      result.diagnostics.push_back({id, source.path, std::move(compiled.error),
                                    std::move(compiled.error_ranges)});
      return false;
    }
    for (const amber::bytecode::DepEntry &dep : compiled.module.dependencies) {
      if (!compile(compiled.module.strings.at(dep.module_name_str_id)))
        return false;
    }
    visited[id] = 2U;
    result.modules.push_back({id, source.path, compiled.module});
    graph_modules.push_back(
        {id, source.path, std::move(compiled.module), false});
    return true;
  };
  for (const std::string &root : roots) {
    if (!compile(root))
      return result;
  }

  auto linked = amber::bytecode::link_graph(
      graph_modules, roots, [&](const std::string &name) {
        return native_modules.has_namespace(name);
      });
  if (!linked.ok) {
    for (const auto &error : linked.diagnostics) {
      const auto source = by_id.find(error.module_name);
      result.diagnostics.push_back(
          {error.module_name,
           source == by_id.end() ? std::string{} : source->second->path,
           error.error_name + ": " + error.message, {}});
    }
    return result;
  }

  std::map<std::string, BundledExportBinding> bindings;
  for (const std::string &root : roots) {
    for (const amber::bytecode::GraphExport &entry : linked.exports) {
      if (entry.provider != root)
        continue;
      // Kernel.watch is a notebook intrinsic recognized before ordinary
      // ambient lookups. Do not silently install an unusable override.
      if (entry.public_name == "Kernel") {
        result.diagnostics.push_back(
            {root, by_id.at(root)->path,
             "auto-import export 'Kernel' conflicts with the notebook intrinsic",
             {}});
        continue;
      }
      BundledExportBinding binding{
          entry.public_name, entry.qualified_path, root, entry.kind,
          entry.kind == "class"    ? entry.class_index
          : entry.kind == "method" ? entry.method_index
                                   : entry.code_id};
      const auto previous = bindings.find(binding.name);
      if (previous != bindings.end()) {
        result.diagnostics.push_back(
            {root, by_id.at(root)->path,
             "ambiguous auto-import export '" + binding.name + "' from '" +
                 previous->second.module_id + "' and '" + root + "'",
             {}});
        continue;
      }
      bindings.emplace(binding.name, binding);
      result.exports.push_back(std::move(binding));
    }
  }
  if (!result.diagnostics.empty())
    return result;

  result.image = std::make_shared<const amber::bytecode::BcModule>(
      std::move(linked.module));
  for (const auto &binding : result.exports)
    result.ambient_constant_paths.emplace(binding.name, binding.qualified_path);
  result.ok = true;
  return result;
}
