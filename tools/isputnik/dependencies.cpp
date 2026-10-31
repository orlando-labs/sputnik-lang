#include "tools/isputnik/dependencies.h"
#include "buildsys/build.h"
#include "package/package.h"
#include "package/store.h"
#include "frontend/lexer/token.h"
#include "runtime/sputnik_ext_runtime.h"
#include "runtime/sputnik_ext.h"

#include <dlfcn.h>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
std::string extra_string(const sputnik::notebook::ProjectDependency &dependency, const char *key) {
  const auto entry = dependency.extra.find(key);
  if (entry == dependency.extra.end()) return {};
  const auto value = sputnik::notebook::parse_project_input_value(entry->second);
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
std::string read_source(const fs::path &path, std::size_t *budget,
                        std::size_t limit = 16 * 1024 * 1024) {
  const auto size = fs::file_size(path);
  if (size > limit || size > *budget) throw std::runtime_error("package member size limit exceeded: " + path.string());
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
    const sputnik::notebook::ProjectDependency &dependency) {
  namespace fs = std::filesystem;
  const auto root = fs::canonical(project_directory / fs::path(dependency.path));
  if (!fs::is_directory(root)) throw std::runtime_error("dependency is not a package directory: " + root.string());
  std::size_t budget = 64 * 1024 * 1024;
  std::vector<sputnik::pkg::PackageModule> modules;
  std::string root_module;
  fs::path manifest_dir = root;
  const auto explicit_manifest = extra_string(dependency, "manifest");
  {
    std::string manifest = explicit_manifest;
    if (manifest.empty()) for (const auto name : {"sputnik.build.yaml", "sputnik.build.yml", "sputnik.build.json"}) {
      if (fs::exists(root / name)) {
        if (!manifest.empty()) throw std::runtime_error("ambiguous build manifests in " + root.string());
        manifest = name;
      }
    }
    if (manifest.empty()) throw std::runtime_error("no sputnik.build manifest in " + root.string());
    const auto path = checked_member(root, manifest);
    manifest_dir = path.parent_path();
    const auto parsed = sputnik::build::parse_build_manifest(read_source(path, &budget), path.string());
    if (!parsed.ok()) throw std::runtime_error(sputnik::build::diagnostics_to_string(parsed.diagnostics));
    if (!parsed.manifest.dependencies.empty())
      throw std::runtime_error("registry/versioned package dependencies are not supported by notebook directory links yet: " + path.string());
    // Do not silently change numeric semantics: source preambles are supported,
    // build-only numeric overrides need the ordinary package build pipeline.
    if (!parsed.manifest.profiles.numeric_int.empty() || !parsed.manifest.profiles.numeric_overflow.empty())
      throw std::runtime_error("notebook source dependencies require numeric profiles in source, not build overrides: " + path.string());
    // Native declarations use the host's already registered thunks. Accepting
    // ffi.v1 here does not compile extensions or grant FFI capabilities.
    for (const auto &feature : parsed.manifest.profiles.required_features)
      if (feature != "ffi.v1" && !sputnik::build::runtime_supports_feature(feature))
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

sputnik::notebook::ProjectDependency prepared_native_dependency(const std::filesystem::path &package_directory) {
  const auto root = fs::canonical(package_directory);
  std::size_t budget = 256U * 1024U * 1024U;
  auto dependency = sputnik::notebook::parse_project_dependency(
      read_source(checked_member(root, "build/notebook/current.json"), &budget));
  if (fs::weakly_canonical(root / dependency.path) != root)
    throw std::runtime_error("prepared descriptor refers to a different package");
  const auto library = checked_member(root, extra_string(dependency, "native_library"));
  const auto digest = sputnik::lexer::sha256_hex(read_source(library, &budget, 256U * 1024U * 1024U));
  if (extra_string(dependency, "native_sha256") != digest)
    throw std::runtime_error("prepared native library digest changed; prepare the package again");
  (void)load_directory_dependency(root, dependency);
  return dependency;
}

void register_project_native_updates(const std::filesystem::path &package_directory,
                                     const std::filesystem::path &project_directory) {
  const auto root = fs::canonical(package_directory);
  const auto project = sputnik::notebook::load_project(project_directory);
  bool linked = false;
  for (const auto &dependency : project.document.dependencies)
    linked |= fs::weakly_canonical(project.directory / dependency.path) == root &&
              !extra_string(dependency, "native_library").empty();
  if (!linked) throw std::runtime_error("project has no native dependency on " + root.string());
  const auto directory = root / "build/notebook/projects";
  fs::create_directories(directory);
  const auto relative = fs::canonical(directory).lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("notebook registration escapes package root");
  const auto key = sputnik::lexer::sha256_hex(project.directory.string());
  const auto target = directory / (key + ".json");
  const auto payload = "{\"path\":" + sputnik::notebook::serialize_project_input_value(
      sputnik::runtime::NotebookInputValue{project.directory.string()}) + "}\n";
  std::size_t budget = 16 * 1024 * 1024;
  if (fs::exists(target) && read_source(target, &budget) == payload) return;
  const auto temporary = directory / (key + ".tmp." + std::to_string(::getpid()));
  if (fs::exists(temporary)) throw std::runtime_error("notebook registration is already in progress");
  std::ofstream stream(temporary, std::ios::binary);
  stream << payload;
  stream.close();
  if (!stream) throw std::runtime_error("cannot write notebook registration: " + temporary.string());
  fs::rename(temporary, target);
}

bool update_project_native_dependency(const std::filesystem::path &project_directory,
                                      const std::filesystem::path &package_directory,
                                      const std::string &manifest,
                                      const std::string &library) {
  auto project = sputnik::notebook::load_project(project_directory);
  const auto root = fs::canonical(package_directory);
  if (!fs::is_directory(root)) throw std::runtime_error("dependency is not a package directory: " + root.string());
  (void)checked_member(root, manifest);
  const auto library_path = checked_member(root, library);
  std::size_t budget = 256U * 1024U * 1024U;
  const auto digest = sputnik::lexer::sha256_hex(read_source(library_path, &budget, budget));
  const auto quoted = [](const std::string &value) {
    return sputnik::notebook::serialize_project_input_value(sputnik::runtime::NotebookInputValue{value});
  };
  auto next = project.document;
  bool found = false, changed = false;
  for (auto &dependency : next.dependencies) {
    if (fs::weakly_canonical(project.directory / dependency.path) != root) continue;
    found = true;
    if (extra_string(dependency, "native_library").empty())
      throw std::runtime_error("refusing to convert a source dependency into a native dependency");
    const auto previous = dependency.extra;
    dependency.extra["manifest"] = quoted(manifest);
    dependency.extra["native_library"] = quoted(library);
    dependency.extra["native_sha256"] = quoted(digest);
    (void)load_directory_dependency(project.directory, dependency);
    changed |= previous != dependency.extra;
  }
  if (!found) throw std::runtime_error("project has no dependency on " + root.string());
  if (changed) sputnik::notebook::save_project(&project, next);
  return changed;
}

void load_prepared_native_dependencies(const sputnik::notebook::LoadedProject &project,
                                      const sputnik::bytecode::BcModule &module, bool authorized) {
  // Worker owner-thread only. Registry pointers and their dynamic libraries
  // have process lifetime; never dlclose code still referenced by a world.
  struct LoadedLibrary { void *handle; std::string digest; };
  static std::map<std::string, LoadedLibrary> libraries;
  auto dependencies = project.document.dependencies;
  std::map<std::string, sputnik::pkg::PackageManifest> installed_manifests;
  std::map<std::string, std::string> artifact_digests;
  for (const auto &attr : module.attrs) {
    const auto &key = module.strings.at(attr.key_str_id);
    const std::string prefix = "sputnik.package.artifact:";
    if (key.rfind(prefix, 0) != 0) continue;
    const auto inserted = artifact_digests.emplace(key, module.strings.at(attr.value_str_id));
    if (!inserted.second) {
      if (inserted.first->second != module.strings.at(attr.value_str_id))
        throw std::runtime_error("cannot mix package snapshots in one notebook environment");
      continue;
    }
    const auto package = sputnik::pkg::read_package(key.substr(prefix.size()));
    if (package.digest != module.strings.at(attr.value_str_id))
      throw std::runtime_error("installed package changed since Apply: " + package.manifest.name);
    if (package.manifest.native_extensions.empty() || installed_manifests.count(package.directory.string())) continue;
    sputnik::pkg::verify_prepared_native(package);
    std::size_t budget = 16384;
    std::istringstream descriptor(read_source(checked_member(package.directory, "native-library.meta"), &budget));
    std::string target, library, digest;
    std::getline(descriptor, target); std::getline(descriptor, library); std::getline(descriptor, digest);
    if (target != sputnik::pkg::package_host_target()) throw std::runtime_error("installed native package targets another platform: " + package.manifest.name);
    const auto quoted = [](const std::string &text) {
      return sputnik::notebook::serialize_project_input_value(sputnik::runtime::NotebookInputValue{text});
    };
    sputnik::notebook::ProjectDependency dependency;
    dependency.path = package.directory.string();
    dependency.extra["native_library"] = quoted(library);
    dependency.extra["native_sha256"] = quoted(digest);
    dependencies.push_back(std::move(dependency));
    installed_manifests.emplace(package.directory.string(), package.manifest);
  }
  for (const auto &dependency : dependencies) {
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
    if (digest.size() != 64 || sputnik::lexer::sha256_hex(bytes) != digest)
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
    auto version = reinterpret_cast<const char *(*)()>(symbol("sputnik_language_version"));
    const char *producer = version();
    if (!producer || !sputnik::language::compatible_producer(producer)) throw std::runtime_error("native dependency Sputnik language version mismatch");
    sputnik::build::BuildManifest manifest;
    const auto installed = installed_manifests.find(root.string());
    if (installed != installed_manifests.end()) {
      manifest.native_extensions = installed->second.native_extensions;
      for (const auto &entry : installed->second.modules) manifest.modules.push_back({entry.name, entry.path, false, {}});
    } else {
      const auto manifest_name = extra_string(dependency, "manifest");
      const auto manifest_path = checked_member(root, manifest_name.empty() ? "sputnik.build.json" : manifest_name);
      const auto parsed = sputnik::build::parse_build_manifest(read_source(manifest_path, &budget), manifest_path.string());
      if (!parsed.ok()) throw std::runtime_error(sputnik::build::diagnostics_to_string(parsed.diagnostics));
      manifest = parsed.manifest;
    }
    std::set<std::string> blocking;
    std::map<std::string, std::string> symbols;
    for (const auto &extension : manifest.native_extensions) {
      blocking.insert(extension.blocking_symbols.begin(), extension.blocking_symbols.end());
      for (const auto &entry : extension.symbols) symbols[entry.logical] = entry.symbol;
    }
    sputnik::runtime::RuntimeNativePackageDescriptor package;
    std::set<std::string> added;
    std::set<std::string> added_errors;
    const auto belongs = [&](const std::string &name) {
      for (const auto &entry : manifest.modules)
        if (name.rfind(entry.name + ".", 0) == 0) return true;
      return false;
    };
    for (const auto &attr : module.attrs) {
      if (attr.key_str_id >= module.strings.size() || attr.value_str_id >= module.strings.size()) continue;
      const auto &key = module.strings[attr.key_str_id], &value = module.strings[attr.value_str_id];
      std::string logical;
      if (key.rfind("sputnik.native.bind:", 0) == 0 && value.size() > 2 && value[1] == ':') logical = value.substr(2);
      else if (key.rfind("sputnik.native.method:", 0) == 0) logical = value;
      if (!logical.empty() && added.insert(logical).second) {
        const auto mapped = symbols.find(logical);
        if (auto fn = symbol(mapped == symbols.end() ? logical : mapped->second, false))
          package.thunks.push_back({logical, fn, blocking.count(logical) != 0});
      }
      const std::string type_prefix = "sputnik.native.type:", error_prefix = "sputnik.native.error:";
      if (key.rfind(type_prefix, 0) == 0 && belongs(key.substr(type_prefix.size()))) {
        std::vector<std::string> fields;
        std::size_t start = 0, end;
        do { end = value.find('\t', start); fields.push_back(value.substr(start, end - start)); start = end + 1; } while (end != std::string::npos);
        if (fields.size() < 2) throw std::runtime_error("malformed native type metadata");
        sputnik::runtime::NativeTypeDescriptor type; type.tag = fields[0];
        if (fields[1] == "owned" || fields[1] == "collected") {
          if (fields.size() < 3 || fields[2].empty()) throw std::runtime_error("native owner missing destructor");
          const auto mapped = symbols.find(fields[2]);
          auto fn = symbol(mapped == symbols.end() ? fields[2] : mapped->second);
          if (fields[1] == "owned") {
            type.ownership = sputnik::runtime::RuntimeForeignHandle::Ownership::Owned;
            std::memcpy(&type.owned_destructor, &fn, sizeof(fn));
          } else {
            type.ownership = sputnik::runtime::RuntimeForeignHandle::Ownership::Collected;
            std::memcpy(&type.collected_reclaim, &fn, sizeof(fn));
          }
        } else type.ownership = sputnik::runtime::RuntimeForeignHandle::Ownership::Borrowed;
        package.types.push_back(std::move(type));
      } else if (key.rfind(error_prefix, 0) == 0 && belongs(key.substr(error_prefix.size()))) {
        package.errors.push_back({key.substr(error_prefix.size()), value.empty() ? "NativeError" : value, {}, -1});
        added_errors.insert(key.substr(error_prefix.size()));
      }
    }
    for (const auto &extension : manifest.native_extensions)
      for (const auto &error : extension.errors)
        if (added_errors.insert(error.name).second) {
          std::int64_t exit_code = -1;
          try {
            std::size_t consumed = 0;
            const auto parsed = std::stoll(error.default_exit_code, &consumed);
            if (consumed == error.default_exit_code.size()) exit_code = parsed;
          } catch (const std::exception &) {}
          package.errors.push_back({error.name, error.parent.empty() ? "NativeError" : error.parent,
                                    error.default_message, exit_code});
        }
    sputnik::runtime::NativeExtRegistry::global().register_package(std::move(package));
  }
}
