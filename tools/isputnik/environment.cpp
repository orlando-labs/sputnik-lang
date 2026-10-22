#include "frontend/foreign_import.h"
#include "tools/isputnik/session.h"

#include "bytecode/graph_linker.h"
#include "runtime/stdlib_registry.h"
#include "package/store.h"

#include <exception>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

BundledEnvironmentPreparation prepare_bundled_environment(
    const std::vector<BundledModuleSource> &sources,
    const std::vector<std::string> &explicit_imports,
    const std::string &package_search_directory,
    const sputnik::frontend::ImportAliases &aliases) {
  BundledEnvironmentPreparation result;
  result.import_aliases = aliases;
  try { aliases.validate(); }
  catch (const std::exception &error) {
    result.diagnostics.push_back({{}, {}, error.what(), {}});
    return result;
  }
  std::map<std::string, const BundledModuleSource *> by_id;
  std::vector<std::string> roots;
  std::set<std::string> selected;
  for (const BundledModuleSource &source : sources) {
    if (!source.load_error.empty()) {
      result.diagnostics.push_back({source.id, source.path, source.load_error, {}});
    } else if (source.id.empty() || source.path.empty()) {
      result.diagnostics.push_back(
          {source.id, source.path, "bundled module identity is incomplete", {}});
    } else if (!by_id.emplace(source.id, &source).second) {
      result.diagnostics.push_back({source.id, source.path,
                                    "duplicate bundled module id: " + source.id,
                                    {}});
    }
    const auto root = aliases.resolve(source.id);
    if (source.auto_import && selected.insert(root).second)
      roots.push_back(root);
  }
  if (!result.diagnostics.empty())
    return result;

  sputnik::runtime::RuntimeModuleRegistry native_modules;
  sputnik::runtime::RuntimeDispatchRegistry native_dispatch;
  sputnik::runtime::RuntimeTypeRegistry native_types;
  sputnik::runtime::register_builtin_runtime_modules(
      native_modules, native_dispatch, native_types);
  sputnik::runtime::register_core_prelude_bindings(native_modules);
  sputnik::runtime::register_legacy_native_type_paths(native_modules);
  for (const auto &source : sources) {
    // notebook.ui combines source helpers with native Stopwatch. Other native
    // namespaces remain reserved, and its native bindings cannot be replaced.
    if (source.id != "notebook.ui" && native_modules.has_namespace(source.id)) {
      result.diagnostics.push_back({source.id, source.path, "project dependency conflicts with native module: " + source.id, {}});
    }
  }
  if (!result.diagnostics.empty()) return result;
  const auto native_only = [&](const std::string &id) {
    return native_modules.has_namespace(id) && !(id == "notebook.ui" && by_id.count(id));
  };
  for (const auto &requested : explicit_imports) {
    const auto canonical = aliases.resolve(requested);
    const auto provider = sputnik::frontend::foreign_import_provider(canonical);
    const auto id = provider.empty() ? canonical : provider;
    if (!native_only(id) && selected.insert(id).second) roots.push_back(id);
  }
  if (roots.empty()) { result.ok = true; return result; }

  // Only compile modules reachable from the selected roots. An unfinished
  // unrelated source buffer is allowed to coexist in the same project.
  std::map<std::string, unsigned> visited;
  std::optional<sputnik::pkg::PackageResolver> packages;
  std::vector<sputnik::bytecode::GraphModule> graph_modules;
  std::function<bool(const std::string &)> compile = [&](const std::string &requested) {
    const auto id = aliases.resolve(requested);
    const auto found = by_id.find(id);
    if (native_only(id)) return true;
    const BundledModuleSource *source = found == by_id.end() ? nullptr : found->second;
    std::string path = source ? source->path : std::string{};
    if (visited[id] == 2U)
      return true;
    if (visited[id] == 1U) {
      result.diagnostics.push_back(
          {id, path, "bundled module dependency cycle: " + id, {}});
      return false;
    }
    visited[id] = 1U;
    CompileResult compiled;
    try {
      if (source) compiled = compile_source_text(source->source, source->path, source->id);
      else {
        if (!packages) packages.emplace(package_search_directory);
        const auto resolved = packages->resolve(id);
        if (!resolved) {
          result.diagnostics.push_back({id, {}, "missing bundled module dependency: " + id +
              " (searched " + packages->search_description() + "; install its package with sputnik pkg install <package>, or add its directory to project dependencies)", {}});
          return false;
        }
        path = resolved->path;
        if (!resolved->bytecode.empty()) {
          auto decoded = sputnik::bytecode::deserialize_module(resolved->bytecode);
          compiled.ok = decoded.ok();
          compiled.module = std::move(decoded.module);
          if (!compiled.ok) compiled.error = sputnik::bytecode::verify_errors_to_json(decoded.errors);
        } else compiled = compile_source_text(resolved->source, resolved->path, id);
        if (compiled.ok && !resolved->package_artifact.empty()) {
          const auto add_string = [&](const std::string &value) {
            compiled.module.strings.push_back(value);
            return static_cast<std::uint32_t>(compiled.module.strings.size() - 1);
          };
          const auto key = add_string("sputnik.package.artifact:" + resolved->package_artifact);
          const auto value = add_string(resolved->package_digest);
          compiled.module.attrs.push_back({key, value});
        }
      }
    } catch (const std::exception &error) {
      result.diagnostics.push_back({id, path, error.what(), {}});
      return false;
    }
    if (!compiled.ok) {
      result.diagnostics.push_back({id, path, std::move(compiled.error),
                                    std::move(compiled.error_ranges)});
      return false;
    }
    sputnik::bytecode::redirect_module_imports(&compiled.module, aliases);
    for (const sputnik::bytecode::DepEntry &dep : compiled.module.dependencies) {
      if (!compile(compiled.module.strings.at(dep.module_name_str_id)))
        return false;
    }
    visited[id] = 2U;
    result.modules.push_back({id, path, compiled.module});
    graph_modules.push_back(
        {id, path, std::move(compiled.module), false});
    return true;
  };
  for (const std::string &root : roots) {
    if (!compile(root))
      return result;
  }

  auto linked = sputnik::bytecode::link_graph(
      graph_modules, roots, [&](const std::string &name) {
        return native_modules.has_namespace(name);
      }, aliases);
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
  for (const auto &module : result.modules) result.import_paths[module.id];
  for (const auto &entry : linked.exports) {
    if (native_modules.binding_for_path(entry.provider + "." + entry.public_name)) {
      const auto source = by_id.find(entry.provider);
      result.diagnostics.push_back({entry.provider, source == by_id.end() ? std::string{} : source->second->path,
          "source export conflicts with native binding: " + entry.provider + "." + entry.public_name, {}});
      continue;
    }
    result.import_paths[entry.provider][entry.public_name] = entry.qualified_path;
  }
  for (const std::string &root : roots) {
    const auto source = by_id.find(root);
    if (source == by_id.end() || !source->second->auto_import) continue;
    for (const sputnik::bytecode::GraphExport &entry : linked.exports) {
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

  result.image = std::make_shared<const sputnik::bytecode::BcModule>(
      std::move(linked.module));
  for (const auto &binding : result.exports)
    result.ambient_constant_paths.emplace(binding.name, binding.qualified_path);
  result.ok = true;
  return result;
}
