#include "frontend/foreign_import.h"
#include "tools/isputnik/session.h"

#include "bytecode/emitter.h"
#include "bytecode/graph_linker.h"
#include "runtime/stdlib_registry.h"
#include "package/store.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <exception>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace {
struct ImportReference {
  std::string owner, path, module;
  sputnik::lexer::Span span;
  std::vector<std::pair<std::string, sputnik::lexer::Span>> names;
};

std::string field(const sputnik::ast::Expr &node, const std::string &name) {
  for (const auto &entry : node.string_fields) if (entry.name == name) return entry.value;
  return {};
}

std::vector<ImportReference> source_imports(const std::string &source,
                                          const std::string &path,
                                          const std::string &owner) {
  auto lexed = sputnik::lexer::Lexer(source, path).lex();
  if (!lexed.ok()) return {};
  auto parsed = sputnik::parser::Parser(lexed.tokens).parse_module_unit();
  std::vector<ImportReference> imports;
  for (const auto &item : parsed.items) {
    if (!item || item->kind != "AstImportStmt") continue;
    ImportReference ref{owner, path, field(*item, "module_path"), item->span, {}};
    for (const auto &list : item->list_fields) if (list.name == "names")
      for (const auto &name : list.values) if (name)
        ref.names.push_back({field(*name, "source_name"), name->span});
    imports.push_back(std::move(ref));
  }
  return imports;
}

BundledEnvironmentDiagnostic import_diagnostic(const ImportReference &ref,
                                               std::string message,
                                               const sputnik::lexer::Span &span) {
  SourceErrorRange range;
  range.file = ref.path;
  range.start_line = span.start.line;
  range.start_column = span.start.col ? span.start.col - 1U : 0U;
  range.end_line = span.end.line;
  range.end_column = span.end.col ? span.end.col - 1U : range.start_column + 1U;
  range.start_offset = span.start.offset;
  range.end_offset = span.end.offset;
  range.has_offsets = true;
  if (span.start.line)
    message += " at " + ref.path + ":" + std::to_string(span.start.line) +
               ":" + std::to_string(span.start.col);
  return {ref.owner, ref.path, std::move(message), {range}};
}
}

BundledEnvironmentPreparation prepare_bundled_environment(
    const std::vector<BundledModuleSource> &project_sources,
    const std::vector<std::string> &explicit_imports,
    const std::string &package_search_directory,
    const sputnik::frontend::ImportAliases &explicit_aliases,
    const std::vector<sputnik::notebook::CellSource> &import_sources) {
  BundledEnvironmentPreparation result;
  auto sources = project_sources;
  try {
    for (auto &source : notebook_essential_sources()) {
      if (std::none_of(sources.begin(), sources.end(), [&](const auto &existing) {
            return existing.id == source.id;
          })) sources.push_back(std::move(source));
    }
  } catch (const std::exception &error) {
    result.diagnostics.push_back({{}, {}, error.what(), {}});
    return result;
  }
  auto aliases = explicit_aliases;
  // Registry short names are notebook-owned bindings to installed providers.
  // Never resolve them over the network while compiling or running a cell.
  std::set<std::filesystem::path> homes;
  if (!package_search_directory.empty()) homes.insert(std::filesystem::path(package_search_directory) / "packages");
  if (const auto *home = std::getenv("SPUTNIK_NOTEBOOK_PACKAGE_HOME")) if (*home) homes.insert(home);
  try {
    for (const auto &home : homes) {
      const auto index = home / "imports.index";
      if (!std::filesystem::exists(index)) continue;
      if (std::filesystem::file_size(index) > 1048576) throw std::runtime_error("Notebook import bindings exceed 1 MiB.");
      std::ifstream input(index);
      std::string line;
      while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto first = line.find('\t'), last = line.find('\t', first == std::string::npos ? 0 : first + 1);
        if (first == std::string::npos || last == std::string::npos) throw std::runtime_error("Invalid notebook import binding. Reinstall its package in Dependencies.");
        const auto alias = line.substr(first + 1, last - first - 1), target = line.substr(last + 1);
        const bool local = std::any_of(sources.begin(), sources.end(), [&](const auto &source) {
          return sputnik::frontend::ImportAliases::prefix(source.id, alias);
        });
        if (!local && !explicit_aliases.paths().count(alias)) aliases.add(alias, target);
      }
    }
  } catch (const std::exception &error) {
    result.diagnostics.push_back({{}, {}, error.what(), {}});
    return result;
  }
  result.import_aliases = aliases;
  try { aliases.validate(); }
  catch (const std::exception &error) {
    result.diagnostics.push_back({{}, {}, error.what(), {}});
    return result;
  }
  std::set<std::string> used_aliases;
  std::map<std::string, std::string> resolved_aliases;
  for (const auto &[alias, target] : aliases.paths()) {
    resolved_aliases.emplace(alias, aliases.resolve(alias));
    if (explicit_aliases.paths().count(alias)) used_aliases.insert(alias);
  }
  const auto resolve_import = [&](const std::string &requested) {
    const auto canonical = aliases.resolve(requested);
    if (canonical == requested) return canonical;
    const std::string *best = nullptr;
    for (const auto &[alias, target] : resolved_aliases) {
      if ((!best || alias.size() > best->size()) &&
          sputnik::frontend::ImportAliases::prefix(requested, alias) &&
          canonical == target + requested.substr(alias.size()))
        best = &alias;
    }
    if (best) used_aliases.insert(*best);
    return canonical;
  };
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
  std::optional<sputnik::pkg::PackageResolver> packages;
  std::optional<bool> installed_notebook_ui;
  const auto native_only = [&](const std::string &id) {
    if (!native_modules.has_namespace(id)) return false;
    if (id != "notebook.ui") return true;
    if (by_id.count(id)) return false;
    // This hybrid namespace also has installed source/bytecode helpers. Use
    // the normal verified package resolver before treating it as native-only;
    // native Stopwatch still works when no helper package is installed.
    if (!installed_notebook_ui) {
      if (!packages) packages.emplace(package_search_directory);
      try { installed_notebook_ui = packages->resolve(id).has_value(); }
      catch (const std::exception &) {
        // Report metadata errors through the normal compile diagnostic path.
        installed_notebook_ui = true;
      }
    }
    return !*installed_notebook_ui;
  };
  for (const auto &requested : explicit_imports) {
    const auto canonical = resolve_import(requested);
    const auto provider = sputnik::frontend::foreign_import_provider(canonical);
    const auto id = provider.empty() ? canonical : provider;
    if (!native_only(id) && selected.insert(id).second) roots.push_back(id);
  }

  std::vector<ImportReference> cell_imports;
  for (const auto &source : import_sources) {
    auto imports = source_imports(source.source, source.file, {});
    cell_imports.insert(cell_imports.end(), imports.begin(), imports.end());
  }
  std::map<std::string, std::set<std::string>> exports;
  const auto validate_import = [&](const ImportReference &ref) {
    const auto id = resolve_import(ref.module);
    if (!sputnik::frontend::foreign_import_provider(id).empty()) return true;
    for (const auto &[name, span] : ref.names) {
      const auto provider = exports.find(id);
      if ((provider != exports.end() && provider->second.count(name)) ||
          native_modules.has_namespace(id + "." + name)) continue;
      result.diagnostics.push_back(import_diagnostic(ref,
          "Cannot import '" + name + "' from '" + ref.module +
          "': name is not available.", span));
      return false;
    }
    return true;
  };

  // Only compile modules reachable from the selected roots. An unfinished
  // unrelated source buffer is allowed to coexist in the same project.
  std::map<std::string, unsigned> visited;
  std::vector<sputnik::bytecode::GraphModule> graph_modules;
  std::function<bool(const std::string &, const ImportReference *)> compile =
      [&](const std::string &requested, const ImportReference *origin) {
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
    std::vector<ImportReference> imports;
    try {
      if (source) {
        compiled = compile_source_text(source->source, source->path, source->id);
        imports = source_imports(source->source, source->path, source->id);
      }
      else {
        if (!packages) packages.emplace(package_search_directory);
        const auto resolved = packages->resolve(id);
        if (!resolved) {
          const auto message = "Cannot import module '" + requested +
              "': module is not available in this notebook.";
          if (origin) result.diagnostics.push_back(import_diagnostic(*origin, message, origin->span));
          else result.diagnostics.push_back({id, {}, message, {}});
          return false;
        }
        path = resolved->path;
        if (!resolved->bytecode.empty()) {
          auto decoded = sputnik::bytecode::deserialize_module(resolved->bytecode);
          compiled.ok = decoded.ok();
          compiled.module = std::move(decoded.module);
          if (!compiled.ok) compiled.error = sputnik::bytecode::verify_errors_to_json(decoded.errors);
        } else {
          compiled = compile_source_text(resolved->source, resolved->path, id);
          imports = source_imports(resolved->source, resolved->path, id);
        }
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
    for (const auto &dep : compiled.module.dependencies)
      (void)resolve_import(compiled.module.strings.at(dep.module_name_str_id));
    sputnik::bytecode::redirect_module_imports(&compiled.module, aliases);
    for (const sputnik::bytecode::DepEntry &dep : compiled.module.dependencies) {
      const auto &dependency = compiled.module.strings.at(dep.module_name_str_id);
      const auto importing = std::find_if(imports.begin(), imports.end(), [&](const auto &ref) {
        return aliases.resolve(ref.module) == dependency;
      });
      if (!compile(dependency, importing == imports.end() ? nullptr : &*importing))
        return false;
    }
    for (const auto &ref : imports) if (!validate_import(ref)) return false;
    for (const auto &entry : compiled.module.exports)
      exports[id].insert(compiled.module.symbols.at(entry.symbol_id));
    visited[id] = 2U;
    result.modules.push_back({id, path, compiled.module});
    graph_modules.push_back(
        {id, path, std::move(compiled.module), false});
    return true;
  };
  for (const std::string &root : roots) {
    const auto importing = std::find_if(cell_imports.begin(), cell_imports.end(), [&](const auto &ref) {
      return aliases.resolve(ref.module) == root;
    });
    if (!compile(root, importing == cell_imports.end() ? nullptr : &*importing))
      return result;
  }
  for (const auto &ref : cell_imports) if (!validate_import(ref)) return result;
  if (roots.empty()) { result.ok = true; return result; }

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

  // A registered short name affects diagnostics once the notebook uses it.
  for (const auto &alias : used_aliases) {
    const auto &target = resolved_aliases.at(alias);
    result.fault_name_prefixes.try_emplace(target, alias);
    result.fault_name_prefixes.try_emplace(
        sputnik::bytecode::native_error_namespace(target),
        sputnik::bytecode::native_error_namespace(alias));
  }

  // notebook.show is a VM intrinsic. Record the class exports of the provider
  // selected by `import plot` so the VM uses the same binding as the compiler.
  const auto plot_provider = aliases.resolve("plot");
  if (plot_provider != "plot") {
    for (const auto &entry : linked.exports) {
      if (entry.provider != plot_provider || entry.kind != "class" ||
          (entry.public_name != "Figure" && entry.public_name != "Figure3D" &&
           entry.public_name != "Rendered"))
        continue;
      auto &image = linked.module;
      const auto key = static_cast<std::uint32_t>(image.strings.size());
      image.strings.push_back("sputnik.notebook.plot_type:" + entry.public_name);
      image.strings.push_back(entry.qualified_path);
      image.attrs.push_back({key, key + 1});
    }
  }
  result.image = std::make_shared<const sputnik::bytecode::BcModule>(
      std::move(linked.module));
  for (const auto &binding : result.exports)
    result.ambient_constant_paths.emplace(binding.name, binding.qualified_path);
  result.ok = true;
  return result;
}
