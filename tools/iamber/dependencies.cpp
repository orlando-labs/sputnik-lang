#include "tools/iamber/dependencies.h"
#include "buildsys/build.h"
#include "package/package.h"
#include "frontend/lexer/token.h"
#include "runtime/amber_ext_runtime.h"
#include "runtime/amber_ext.h"

#include <dlfcn.h>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
std::string extra_string(const amber::notebook::ProjectDependency &dependency, const char *key) {
  const auto entry = dependency.extra.find(key);
  if (entry == dependency.extra.end()) return {};
  const auto value = amber::notebook::parse_project_input_value(entry->second);
  if (const auto text = std::get_if<std::string>(&value)) return *text;
  throw std::runtime_error(std::string("dependency ") + key + " must be a string");
}
fs::path checked_member(const fs::path &root, const std::string &relative) {
  const fs::path path(relative);
  if (path.empty() || path.is_absolute()) throw std::runtime_error("package member must be relative: " + relative);
  for (const auto &part : path)
    if (part == "..") throw std::runtime_error("package member escapes its root: " + relative);
  const auto resolved = fs::canonical(root / path);
  const auto within = resolved.lexically_relative(root);
  if (within.empty() || *within.begin() == ".." || !fs::is_regular_file(resolved))
    throw std::runtime_error("package member is not a file within its root: " + relative);
  return resolved;
}
fs::path source_member(const fs::path &root, const fs::path &manifest_dir, const std::string &path) {
  const auto resolved = fs::canonical(manifest_dir / path);
  const auto relative = resolved.lexically_relative(root);
  if (relative.empty() || *relative.begin() == ".." || !fs::is_regular_file(resolved))
    throw std::runtime_error("package member escapes package root: " + path);
  return resolved;
}
std::string read_source(const fs::path &path, std::size_t *budget) {
  const auto size = fs::file_size(path);
  if (size > 16 * 1024 * 1024 || size > *budget) throw std::runtime_error("package source size limit exceeded: " + path.string());
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read package member: " + path.string());
  // Bounded even when the file grows while being read.
  std::string text; char buffer[8192];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount()) {
    if (text.size() + static_cast<std::size_t>(stream.gcount()) > size)
      throw std::runtime_error("package member changed while reading: " + path.string());
    text.append(buffer, static_cast<std::size_t>(stream.gcount()));
  }
  if (!stream.eof() || text.size() != size) throw std::runtime_error("incomplete package read: " + path.string());
  *budget -= text.size();
  return text;
}
}

std::vector<BundledModuleSource> load_directory_dependency(
    const std::filesystem::path &project_directory,
    const amber::notebook::ProjectDependency &dependency) {
  namespace fs = std::filesystem;
  const auto root = fs::canonical(project_directory / fs::path(dependency.path));
  if (!fs::is_directory(root)) throw std::runtime_error("dependency is not a package directory: " + root.string());
  std::size_t budget = 64 * 1024 * 1024;
  std::vector<amber::pkg::PackageModule> modules;
  std::string root_module;
  fs::path manifest_dir = root;
  const auto explicit_manifest = extra_string(dependency, "manifest");
  if (explicit_manifest.empty() && fs::exists(root / "amber.toml")) {
    const auto path = checked_member(root, "amber.toml");
    const auto parsed = amber::pkg::parse_manifest_toml(read_source(path, &budget), path.string());
    if (!parsed.ok()) {
      std::string errors;
      for (const auto &diagnostic : parsed.diagnostics) errors += diagnostic.message + "\n";
      throw std::runtime_error(path.string() + ": " + errors);
    }
    modules = parsed.manifest.modules;
    root_module = parsed.manifest.root_module;
    if (!parsed.manifest.dependencies.empty())
      throw std::runtime_error("registry/versioned package dependencies are not supported by notebook directory links yet: " + path.string());
  } else {
    std::string manifest = explicit_manifest;
    if (manifest.empty()) for (const auto name : {"amber.build.yaml", "amber.build.yml", "amber.build.json"}) {
      if (fs::exists(root / name)) {
        if (!manifest.empty()) throw std::runtime_error("ambiguous build manifests in " + root.string());
        manifest = name;
      }
    }
    if (manifest.empty()) throw std::runtime_error("no amber.toml or amber.build manifest in " + root.string());
    const auto path = checked_member(root, manifest);
    manifest_dir = path.parent_path();
    const auto parsed = amber::build::parse_build_manifest(read_source(path, &budget), path.string());
    if (!parsed.ok()) throw std::runtime_error(amber::build::diagnostics_to_string(parsed.diagnostics));
    // Do not silently change numeric semantics: source preambles are supported,
    // build-only numeric overrides need the ordinary package build pipeline.
    if (!parsed.manifest.profiles.numeric_int.empty() || !parsed.manifest.profiles.numeric_overflow.empty())
      throw std::runtime_error("notebook source dependencies require numeric profiles in source, not build overrides: " + path.string());
    // Native declarations use the host's already registered thunks. Accepting
    // ffi.v1 here does not compile extensions or grant FFI capabilities.
    for (const auto &feature : parsed.manifest.profiles.required_features)
      if (feature != "ffi.v1" && !amber::build::runtime_supports_feature(feature))
        throw std::runtime_error("unsupported package feature: " + feature);
    if (!parsed.manifest.profiles.forbidden_features.empty())
      throw std::runtime_error("notebook directory links cannot enforce forbidden build profiles yet: " + path.string());
    for (const auto &module : parsed.manifest.modules) modules.push_back({module.name, module.path});
    if (!parsed.manifest.stdlib_modules.empty()) throw std::runtime_error("package cannot replace the notebook's native stdlib: " + path.string());
    root_module = parsed.manifest.root_module;
  }
  if (modules.empty() || modules.size() > 1024) throw std::runtime_error("package requires 1..1024 declared modules");
  std::set<std::string> names;
  std::vector<BundledModuleSource> result;
  for (const auto &module : modules) {
    if (module.name.empty() || !names.insert(module.name).second) throw std::runtime_error("duplicate/empty package module: " + module.name);
    const auto path = source_member(root, manifest_dir, module.path);
    result.push_back({module.name, path.string(), read_source(path, &budget),
                      dependency.auto_import && module.name == root_module, {}});
  }
  if (!names.count(root_module)) throw std::runtime_error("package root is not a declared module: " + root_module);
  return result;
}

void load_prepared_native_dependencies(const amber::notebook::LoadedProject &project,
                                      const amber::bytecode::BcModule &module, bool authorized) {
  // Worker owner-thread only. Registry pointers and their dynamic libraries
  // have process lifetime; never dlclose code still referenced by a world.
  struct LoadedLibrary { void *handle; std::string digest; };
  static std::map<std::string, LoadedLibrary> libraries;
  for (const auto &dependency : project.document.dependencies) {
    const auto library_name = extra_string(dependency, "native_library");
    if (library_name.empty()) continue;
    if (!authorized) throw std::runtime_error("Native dependencies require explicit trust before Run.");
    const auto root = fs::canonical(project.directory / dependency.path);
    const auto path = checked_member(root, library_name);
    const auto digest = extra_string(dependency, "native_sha256");
    std::size_t budget = 256U * 1024U * 1024U;
    // Native artifacts have a separate budget from small source documents.
    const auto size = fs::file_size(path);
    if (size > budget) throw std::runtime_error("native library exceeds 256 MiB");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read native library");
    std::string bytes; char buffer[16384];
    while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
      if (bytes.size() + static_cast<std::size_t>(file.gcount()) > size)
        throw std::runtime_error("native library changed while reading");
      bytes.append(buffer, static_cast<std::size_t>(file.gcount()));
    }
    if (!file.eof() || bytes.size() != size) throw std::runtime_error("incomplete native library read");
    if (digest.size() != 64 || amber::lexer::sha256_hex(bytes) != digest)
      throw std::runtime_error("native library digest changed; rebuild/prepare the notebook dependency: " + path.string());
    const auto cached = libraries.find(path.string());
    if (cached != libraries.end() && cached->second.digest != digest)
      throw std::runtime_error("native library was replaced; restart the worker before loading new code");
    void *handle = cached == libraries.end() ? ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL) : cached->second.handle;
    if (!handle) throw std::runtime_error(std::string("cannot load native library: ") + ::dlerror());
    // Retain even on a registration error: constructors/partial contributions
    // may have escaped. A worker restart is the unload boundary.
    libraries[path.string()] = {handle, digest};
    auto symbol = [&](const std::string &name, bool required = true) -> void * {
      void *fn = ::dlsym(handle, name.c_str());
      if (!fn && required) throw std::runtime_error("missing native symbol " + name + " in " + path.string());
      return fn;
    };
    auto abi = reinterpret_cast<std::uint32_t (*)()>(symbol("amber_ext_abi_version"));
    if (abi() != AMBER_EXT_ABI_VERSION) throw std::runtime_error("native dependency ABI mismatch");
    const auto manifest_name = extra_string(dependency, "manifest");
    const auto manifest_path = checked_member(root, manifest_name.empty() ? "amber.build.json" : manifest_name);
    const auto parsed = amber::build::parse_build_manifest(read_source(manifest_path, &budget), manifest_path.string());
    if (!parsed.ok()) throw std::runtime_error(amber::build::diagnostics_to_string(parsed.diagnostics));
    std::set<std::string> blocking;
    std::map<std::string, std::string> symbols;
    for (const auto &extension : parsed.manifest.native_extensions) {
      blocking.insert(extension.blocking_symbols.begin(), extension.blocking_symbols.end());
      for (const auto &entry : extension.symbols) symbols[entry.logical] = entry.symbol;
    }
    amber::runtime::RuntimeNativePackageDescriptor package;
    std::set<std::string> added;
    const auto belongs = [&](const std::string &name) {
      for (const auto &entry : parsed.manifest.modules)
        if (name.rfind(entry.name + ".", 0) == 0) return true;
      return false;
    };
    for (const auto &attr : module.attrs) {
      if (attr.key_str_id >= module.strings.size() || attr.value_str_id >= module.strings.size()) continue;
      const auto &key = module.strings[attr.key_str_id], &value = module.strings[attr.value_str_id];
      std::string logical;
      if (key.rfind("amber.native.bind:", 0) == 0 && value.size() > 2 && value[1] == ':') logical = value.substr(2);
      else if (key.rfind("amber.native.method:", 0) == 0) logical = value;
      if (!logical.empty() && added.insert(logical).second) {
        const auto mapped = symbols.find(logical);
        if (auto fn = symbol(mapped == symbols.end() ? logical : mapped->second, false))
          package.thunks.push_back({logical, fn, blocking.count(logical) != 0});
      }
      const std::string type_prefix = "amber.native.type:", error_prefix = "amber.native.error:";
      if (key.rfind(type_prefix, 0) == 0 && belongs(key.substr(type_prefix.size()))) {
        std::vector<std::string> fields;
        std::size_t start = 0, end;
        do { end = value.find('\t', start); fields.push_back(value.substr(start, end - start)); start = end + 1; } while (end != std::string::npos);
        if (fields.size() < 2) throw std::runtime_error("malformed native type metadata");
        amber::runtime::NativeTypeDescriptor type; type.tag = fields[0];
        if (fields[1] == "owned" || fields[1] == "collected") {
          if (fields.size() < 3 || fields[2].empty()) throw std::runtime_error("native owner missing destructor");
          const auto mapped = symbols.find(fields[2]);
          auto fn = symbol(mapped == symbols.end() ? fields[2] : mapped->second);
          if (fields[1] == "owned") {
            type.ownership = amber::runtime::RuntimeForeignHandle::Ownership::Owned;
            std::memcpy(&type.owned_destructor, &fn, sizeof(fn));
          } else {
            type.ownership = amber::runtime::RuntimeForeignHandle::Ownership::Collected;
            std::memcpy(&type.collected_reclaim, &fn, sizeof(fn));
          }
        } else type.ownership = amber::runtime::RuntimeForeignHandle::Ownership::Borrowed;
        package.types.push_back(std::move(type));
      } else if (key.rfind(error_prefix, 0) == 0 && belongs(key.substr(error_prefix.size()))) {
        package.errors.push_back({key.substr(error_prefix.size()), value.empty() ? "NativeError" : value, {}, -1});
      }
    }
    amber::runtime::NativeExtRegistry::global().register_package(std::move(package));
  }
}
