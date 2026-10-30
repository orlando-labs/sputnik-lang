#include "package/store.h"
#include "package/distribution.h"
#include "package/registry.h"

#include "buildsys/build.h"
#include "bytecode/format.h"
#include "frontend/lexer/token.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace sputnik::pkg {
namespace {
namespace fs = std::filesystem;

std::string read(const fs::path &path) {
  constexpr std::uintmax_t limit = 256U * 1024U * 1024U;
  if (fs::file_size(path) > limit)
    throw std::runtime_error("package file exceeds 256 MiB: " + path.string());
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read package file: " + path.string());
  std::string result;
  char buffer[16384];
  while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
    if (result.size() + static_cast<std::size_t>(input.gcount()) > limit)
      throw std::runtime_error("package file exceeds 256 MiB: " +
                               path.string());
    result.append(buffer, static_cast<std::size_t>(input.gcount()));
  }
  if (!input.eof())
    throw std::runtime_error("incomplete package read: " + path.string());
  return result;
}

std::string diagnostics(const std::vector<PackageDiagnostic> &items) {
  std::string result;
  for (const auto &item : items)
    result += item.message + "\n";
  return result;
}

bool component(const std::string &name) {
  if (name.empty() || name == "." || name == "..")
    return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '_' || c == '-';
  });
}

std::string artifact_filename(std::string name, const std::string &version) {
  std::replace(name.begin(), name.end(), '/', '-');
  return name + "-" + version + ".sputnikpkg";
}

bool distribution(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return input.get() == 0x1f && input.get() == 0x8b;
}

fs::path member(const fs::path &directory, const std::string &name) {
  if (fs::path(name).is_absolute())
    throw std::runtime_error("package member must be relative: " + name);
  const auto root = fs::canonical(directory);
  const auto path = fs::canonical(root / name);
  const auto relative = path.lexically_relative(root);
  if (relative.empty() || *relative.begin() == ".." ||
      !fs::is_regular_file(path))
    throw std::runtime_error("package member escapes package root: " + name);
  return path;
}

void verify_distribution_members(const LocatedPackage &package) {
  // Reuse the inventory from this activation's already verified archive.
  // Large CUDA runtimes otherwise get decompressed twice on every Run.
  const auto inspected = package.distribution ? package.distribution :
      std::make_shared<const DistributionInfo>(inspect_distribution(package.artifact_path));
  for (const auto &entry : inspected->members)
    if (file_sha256(member(package.directory, entry.path)) != entry.sha256)
      throw std::runtime_error("installed distribution member was modified: " + entry.path);
}

std::vector<fs::path> children(const fs::path &directory) {
  std::vector<fs::path> result;
  if (!fs::exists(directory))
    return result;
  if (!fs::is_directory(directory))
    throw std::runtime_error("package search path is not a directory: " +
                             directory.string());
  for (const auto &entry : fs::directory_iterator(directory))
    if (entry.path().filename().string().front() != '.')
      result.push_back(entry.path());
  std::sort(result.begin(), result.end());
  return result;
}

std::optional<LocatedPackage> directory_package(const fs::path &directory) {
  LocatedPackage result;
  result.directory = fs::canonical(directory);
  fs::path manifest;
  for (const auto *name : {"sputnik.build.yaml", "sputnik.build.yml", "sputnik.build.json"}) {
    if (!fs::is_regular_file(directory / name)) continue;
    if (!manifest.empty()) throw std::runtime_error("ambiguous package manifests: " + directory.string());
    manifest = directory / name;
  }
  if (manifest.empty()) return std::nullopt;
  const auto parsed = build::parse_build_configuration(read(manifest), manifest.string());
  if (!parsed.ok()) throw std::runtime_error(build::diagnostics_to_string(parsed.diagnostics));
  if (!parsed.manifest.stdlib_modules.empty())
    throw std::runtime_error("source package cannot replace the stdlib: " + manifest.string());
  result.manifest = build::package_manifest(parsed.manifest);
  if (result.manifest.version.empty()) result.manifest.version = "local";
  // The full source fingerprint is computed only if this package is activated.
  result.digest = "sha256:" + lexer::sha256_hex(read(manifest));
  result.source_kind = "path";
  result.source = result.directory.generic_string();
  return result;
}

struct Version {
  std::vector<std::string> numbers;
  std::string prerelease;
  bool valid = false;
};

Version version(const std::string &text) {
  Version result;
  const auto build = text.find('+');
  const auto dash = text.find('-');
  const auto end = std::min(build, dash);
  std::istringstream parts(text.substr(0, end));
  std::string part;
  while (std::getline(parts, part, '.')) {
    if (part.empty() ||
        !std::all_of(part.begin(), part.end(),
                     [](unsigned char c) { return std::isdigit(c); }))
      return result;
    const auto first = part.find_first_not_of('0');
    result.numbers.push_back(first == std::string::npos ? "0"
                                                        : part.substr(first));
  }
  if (dash != std::string::npos && dash < build)
    result.prerelease = text.substr(
        dash + 1, build == std::string::npos ? build : build - dash - 1);
  result.valid = result.numbers.size() == 3;
  return result;
}

int numeric_compare(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return a.size() < b.size() ? -1 : 1;
  return a == b ? 0 : a < b ? -1 : 1;
}
} // namespace

fs::path package_home() {
  if (const auto *value = std::getenv("SPUTNIK_PKG_HOME"))
    if (*value)
      return fs::absolute(value).lexically_normal();
  if (const auto *value = std::getenv("HOME"))
    if (*value)
      return fs::path(value) / ".sputnik" / "packages";
  throw std::runtime_error("set SPUTNIK_PKG_HOME: HOME is unavailable");
}

std::string package_host_target() {
#if defined(__x86_64__) || defined(_M_X64)
  const std::string arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  const std::string arch = "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  const std::string arch = "arm";
#elif defined(__i386__) || defined(_M_IX86)
  const std::string arch = "i386";
#else
  const std::string arch = "unknown";
#endif
#if defined(__APPLE__)
  return arch + "-apple-darwin";
#elif defined(__linux__)
  return arch + "-unknown-linux";
#elif defined(_WIN32)
  return arch + "-pc-windows";
#else
  return arch + "-unknown-unknown";
#endif
}

bool version_less(const std::string &left, const std::string &right) {
  const auto a = version(left), b = version(right);
  if (a.valid != b.valid)
    return !a.valid;
  if (!a.valid)
    return left < right;
  for (std::size_t i = 0; i < 3; ++i) {
    const auto compared = numeric_compare(a.numbers[i], b.numbers[i]);
    if (compared)
      return compared < 0;
  }
  if (a.prerelease.empty() != b.prerelease.empty())
    return !a.prerelease.empty();
  std::istringstream aa(a.prerelease), bb(b.prerelease);
  std::string x, y;
  while (std::getline(aa, x, '.')) {
    if (!std::getline(bb, y, '.'))
      return false;
    const auto numeric = [](const std::string &s) {
      return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isdigit(c);
      });
    };
    if (numeric(x) && numeric(y)) {
      const auto compared = numeric_compare(x, y);
      if (compared)
        return compared < 0;
    } else if (numeric(x) != numeric(y))
      return numeric(x);
    else if (x != y)
      return x < y;
  }
  return static_cast<bool>(std::getline(bb, y, '.'));
}

LocatedPackage read_package(const fs::path &path,
                            const std::string &signing_key) {
  if (distribution(path)) {
    if (!signing_key.empty())
      throw std::runtime_error("public distributions use registry SHA-256; legacy signing keys are unsupported");
    auto info = inspect_distribution(path);
    LocatedPackage result;
    result.manifest = std::move(info.manifest);
    result.artifact_path = fs::canonical(path);
    result.directory = result.artifact_path.parent_path();
    result.digest = "sha256:" + info.sha256;
    result.distribution = std::make_shared<const DistributionInfo>(std::move(info));
    const auto provenance = result.directory / ".registry-source";
    if (fs::is_regular_file(provenance) && !fs::is_symlink(provenance)) {
      std::istringstream input(read(provenance));
      std::getline(input, result.source);
      result.source_kind = "registry";
    }
    return result;
  }
  const auto text = read(path);
  const auto verified =
      verify_package_artifact(text, signing_key, path.string());
  if (!verified.ok)
    throw std::runtime_error(diagnostics(verified.diagnostics));
  auto parsed = parse_package_artifact(text, path.string());
  if (!parsed.ok())
    throw std::runtime_error(diagnostics(parsed.diagnostics));
  for (const auto &blob : parsed.artifact.native_blobs) {
    const fs::path relative(blob.path);
    if (relative.empty() || relative.is_absolute())
      throw std::runtime_error("native package member must be relative: " +
                               blob.path);
    for (const auto &part : relative)
      if (part == ".." || part == ".native")
        throw std::runtime_error(
            "native package member escapes or replaces package storage: " +
            blob.path);
    if (blob.path == "modules.index" || blob.path == "manifest.json" ||
        blob.path == "sputnik.lock" || blob.path == "native-library.meta" ||
        relative.extension() == ".sputnikpkg")
      throw std::runtime_error(
          "native package member replaces package metadata: " + blob.path);
  }
  for (const auto &module : parsed.artifact.modules) {
    const auto decoded = bytecode::deserialize_module(module.bytes);
    if (!decoded.ok())
      throw std::runtime_error("invalid package bytecode: " + module.name +
                               "\n" +
                               bytecode::verify_errors_to_json(decoded.errors));
  }
  LocatedPackage result;
  result.directory = fs::canonical(path).parent_path();
  result.artifact_path = fs::canonical(path);
  result.manifest = parsed.artifact.manifest;
  result.digest = "sha256:" + lexer::sha256_hex(text);
  result.artifact =
      std::make_shared<const PackageArtifact>(std::move(parsed.artifact));
  return result;
}

LocatedPackage read_source_package(const fs::path &directory) {
    auto package = directory_package(directory);
  if (!package)
    throw std::runtime_error("no package manifest in " + directory.string());
  for (const auto &module : package->manifest.modules)
    (void)member(package->directory, module.path);
  return std::move(*package);
}

fs::path find_registry_package(const fs::path &registry,
                               const std::string &name,
                               const std::string &requested,
                               const VariantPolicy &policy) {
  if (!(component(name) || registry_reference(name)) || (!requested.empty() && !component(requested)))
    throw std::runtime_error("invalid package name/version: " + name + " " +
                             requested);
  std::map<std::string, std::vector<fs::path>> artifacts;
  for (const auto &path : children(registry / name)) {
    const auto v = path.filename().string();
    if (!fs::is_directory(path) || (!requested.empty() && requested != v)) continue;
    const auto filename = artifact_filename(name,v);
    if (fs::is_regular_file(path / filename)) artifacts[v].push_back(path / filename);
    for (const auto &dir : children(path))
      if (fs::is_directory(dir) && fs::is_regular_file(dir / filename)) artifacts[v].push_back(dir / filename);
  }
  std::vector<std::string> versions;
  for (const auto &entry : artifacts) versions.push_back(entry.first);
  if (versions.empty())
    throw std::runtime_error("package is not in registry: " + name +
                             (requested.empty() ? "" : "@" + requested));
  std::sort(versions.begin(), versions.end(), version_less);
  // An unqualified install prefers a stable release over a prerelease.
  const auto host = probe_package_host(policy.backend == "auto" || package_gpu_backend(policy.backend) || !policy.variant.empty());
  for (bool stable : {true,false}) for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
    if (requested.empty() && version(*it).prerelease.empty() != stable) continue;
    if (!requested.empty() && !stable) continue;
    std::vector<PackageManifest> manifests;
    for (const auto &artifact : artifacts[*it]) manifests.push_back(read_package(artifact).manifest);
    const auto selected = select_package_variant(manifests,host,policy);
    if (selected >= 0) return artifacts[*it][selected];
  }
  throw std::runtime_error("NO_COMPATIBLE_ARTIFACT: " + name + "@" + requested);
}

fs::path install_in_home(const fs::path &path, const fs::path &home,
                         const std::string &signing_key, const std::string &origin,
                         const std::function<void(const fs::path &)> &prepare) {
  const auto package = read_package(path, signing_key);
  if (!package.artifact) {
    if (!registry_reference(package.manifest.name) || !component(package.manifest.version))
      throw std::runtime_error("invalid distribution installation identity");
    fs::create_directories(home);
    const auto target = home / package.manifest.name / package.manifest.version / package.manifest.variant;
    const auto filename = artifact_filename(package.manifest.name, package.manifest.version);
    const auto root = fs::canonical(home);
    const auto relative = fs::weakly_canonical(target).lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..")
      throw std::runtime_error("package installation escapes package home");
    for (auto parent = target; parent != home && parent != parent.parent_path(); parent = parent.parent_path())
      if (fs::is_symlink(fs::symlink_status(parent))) throw std::runtime_error("symlinked package installation directory");
    if (fs::exists(target)) {
      const auto existing = read_package(target / filename);
      if (existing.digest != package.digest)
        throw std::runtime_error("installed package artifact differs from registry");
      verify_distribution_members(existing);
      if (!existing.manifest.native_extensions.empty()) verify_prepared_native(existing);
      if (!origin.empty() && existing.source_kind == "registry" && existing.source != origin)
        throw std::runtime_error("installed package comes from a different registry");
      return target / filename;
    }
    auto pattern = (root / ".install-XXXXXX").string();
    auto temporary = ::mkdtemp(pattern.data());
    if (!temporary) throw std::runtime_error("cannot stage package installation");
    const fs::path staging(temporary);
    try {
      const auto payload = staging / "payload";
      extract_distribution(path, payload);
      fs::copy_file(path, payload / filename);
      std::ofstream index(payload / "modules.index");
      index << "sputnik.package.index.v2\n" << package.manifest.name << '\n'
            << package.manifest.version << '\n' << package.digest << '\n'
            << package.manifest.variant << '\n';
      for (const auto &m : package.manifest.modules) index << m.name << '\n';
      index.close();
      if (!index) throw std::runtime_error("cannot write installed package index");
      if (!origin.empty()) {
        std::ofstream source(payload / ".registry-source"); source << origin << '\n';
        source.close(); if (!source) throw std::runtime_error("cannot write registry provenance");
      }
      if (prepare) prepare(payload);
      fs::create_directories(target.parent_path());
      fs::rename(payload, target);
    } catch (...) { fs::remove_all(staging); throw; }
    fs::remove_all(staging);
    return target / filename;
  }
  const auto result =
      install_package_artifact(read(path), home.string(), signing_key);
  if (!result.ok)
    throw std::runtime_error(diagnostics(result.diagnostics));
  const auto root =
      fs::canonical(fs::path(result.installed_path).parent_path());
  for (const auto &blob : package.artifact->native_blobs) {
    const fs::path relative(blob.path);
    if (relative.empty() || relative.is_absolute())
      throw std::runtime_error("native package member must be relative: " +
                               blob.path);
    for (const auto &part : relative)
      if (part == "..")
        throw std::runtime_error(
            "native package member escapes package root: " + blob.path);
    const auto target = root / relative;
    const auto resolved = fs::weakly_canonical(target).lexically_relative(root);
    if (resolved.empty() || *resolved.begin() == "..")
      throw std::runtime_error("native package member escapes package root: " +
                               blob.path);
    const std::string bytes(blob.bytes.begin(), blob.bytes.end());
    if (fs::exists(target)) {
      if (read(target) != bytes)
        throw std::runtime_error("installed native source was modified: " +
                                 blob.path);
      continue;
    }
    fs::create_directories(target.parent_path());
    std::ofstream output(target, std::ios::binary);
    output << bytes;
    output.close();
    if (!output)
      throw std::runtime_error("cannot install native member: " + blob.path);
  }
  const auto temporary =
      root / (".modules.index." + std::to_string(::getpid()));
  std::ofstream index(temporary, std::ios::binary);
  index << "sputnik.package.index.v1\n"
        << package.manifest.name << '\n'
        << package.manifest.version << '\n'
        << package.digest << '\n';
  for (const auto &module : package.manifest.modules)
    index << module.name << '\n';
  index.close();
  if (!index)
    throw std::runtime_error("cannot write installed package index");
  fs::rename(temporary, root / "modules.index");
  return result.installed_path;
}

void record_prepared_native(const LocatedPackage &package) {
  std::ostringstream output;
  output << "sputnik.native.files.v1\n" << package.manifest.variant << '\n' << package.digest << '\n';
  const auto cache = package.directory / ".native";
  std::vector<fs::path> files;
  if (fs::is_directory(cache)) for (const auto &entry : fs::recursive_directory_iterator(cache)) {
    if (entry.is_symlink()) throw std::runtime_error("symlink in prepared native cache");
    if (entry.is_regular_file() && (entry.path().extension() == ".so" || entry.path().extension() == ".dylib" || entry.path().filename().string().find(".so.") != std::string::npos)) files.push_back(entry.path());
  }
  std::sort(files.begin(),files.end());
  for (const auto &file : files) output << file_sha256(file) << ' ' << std::quoted(file.lexically_relative(package.directory).generic_string()) << '\n';
  std::ofstream out(package.directory / "native-files.meta"); out << output.str(); out.close();
  if (!out) throw std::runtime_error("cannot write prepared native inventory");
}

void verify_prepared_native(const LocatedPackage &package) {
  const auto metadata = package.directory / "native-files.meta";
  if (package.artifact && !fs::exists(metadata)) return; // legacy text artifacts
  if (!fs::is_regular_file(metadata) || fs::is_symlink(metadata) || fs::file_size(metadata) > 1048576)
    throw std::runtime_error("missing/invalid prepared native inventory; reinstall " + package.manifest.name);
  std::istringstream input(read(metadata)); std::string schema,variant,digest;
  std::getline(input,schema); std::getline(input,variant); std::getline(input,digest);
  if (schema != "sputnik.native.files.v1" || variant != package.manifest.variant || digest != package.digest)
    throw std::runtime_error("prepared native inventory differs from artifact");
  // For prebuilt archives the entry library must be the archive's exact bytes,
  // even if someone edits both the local loader descriptor and cache inventory.
  const bool prebuilt = package.manifest.native_extensions.size() == 1 && package.manifest.native_extensions[0].kind == "prebuilt";
  std::map<std::string,std::set<std::string>> known;
  if (prebuilt) {
    const auto &entry = package.manifest.native_extensions[0].library;
    const auto expected = file_sha256(member(package.directory,entry));
    known[fs::path(entry).filename().string()].insert(expected);
    std::istringstream loader(read(package.directory / "native-library.meta"));
    std::string target,library,hash;
    std::getline(loader,target); std::getline(loader,library); std::getline(loader,hash);
    if (hash != expected || file_sha256(member(package.directory,library)) != expected)
      throw std::runtime_error("prepared native entry differs from archive");
    for (const auto &provider : package.manifest.native_providers)
      for (const auto &lib : provider.libraries) known[fs::path(lib.path).filename().string()].insert(file_sha256(member(package.directory,lib.path)));
  }
  std::string hash,path; std::set<std::string> seen;
  while (input >> hash >> std::quoted(path)) {
    if (hash.size() != 64 || !seen.insert(path).second || file_sha256(member(package.directory,path)) != hash)
      throw std::runtime_error("prepared native library changed: " + path);
    const auto expected = known.find(fs::path(path).filename().string());
    if (prebuilt && expected != known.end() && !expected->second.count(hash))
      throw std::runtime_error("prepared native dependency differs from archive: " + path);
  }
  if (!input.eof()) throw std::runtime_error("invalid prepared native inventory");
}

std::vector<std::string> uninstall_from_home(const std::string &name,
                                             const fs::path &home,
                                             const std::string &requested,
                                             const std::string &selected_variant) {
  if (!(component(name) || registry_reference(name)) || (!requested.empty() && !component(requested)))
    throw std::runtime_error("invalid package name/version: " + name + " " +
                             requested);
  const auto absent = [&] {
    return std::runtime_error("package is not installed: " + name +
                              (requested.empty() ? "" : "@" + requested));
  };
  if (!selected_variant.empty() && (requested.empty() || !component(selected_variant)))
    throw std::runtime_error("--variant requires an exact --version");
  if (!fs::is_directory(home))
    throw absent();
  const auto root = fs::canonical(home);
  const auto directory = root / name;
  const auto namespace_end = name.find('/');
  if (namespace_end != std::string::npos &&
      fs::is_symlink(fs::symlink_status(root / name.substr(0, namespace_end))))
    throw std::runtime_error("refusing to uninstall through a symlinked namespace");
  if (fs::is_symlink(fs::symlink_status(directory)))
    throw std::runtime_error(
        "refusing to uninstall a symlinked package directory: " +
        directory.string());
  if (!fs::is_directory(directory))
    throw absent();
  std::vector<std::string> versions;
  const auto check_version = [&](const fs::path &path) {
    const auto version = path.filename().string();
    if (!component(version))
      return;
    if (fs::is_symlink(fs::symlink_status(path)))
      throw std::runtime_error(
          "refusing to uninstall a symlinked package version: " +
          path.string());
    if (!fs::is_directory(path)) return;
    bool found = false;
    auto check_artifact = [&](const fs::path &dir) {
      if (fs::is_symlink(fs::symlink_status(dir))) throw std::runtime_error("refusing to uninstall a symlinked package variant");
      const auto artifact = dir / artifact_filename(name, version);
      if (fs::is_symlink(fs::symlink_status(artifact))) throw std::runtime_error("refusing to uninstall a symlinked package artifact");
      found = found || fs::is_regular_file(artifact);
    };
    if (selected_variant.empty()) {
      check_artifact(path);
      if (!found) for (const auto &dir : children(path)) if (fs::is_directory(dir)) check_artifact(dir);
    }
    else check_artifact(path / selected_variant);
    if (!found) return;
    versions.push_back(version);
  };
  if (requested.empty()) {
    for (const auto &path : children(directory))
      check_version(path);
  } else
    check_version(directory / requested);
  if (versions.empty())
    throw absent();
  std::sort(versions.begin(), versions.end(), version_less);

  // Unpublish complete version directories before deleting their files, so
  // resolvers never observe a half-removed artifact or native-library index.
  auto pattern = (root / ".remove-XXXXXX").string();
  const auto temporary = ::mkdtemp(pattern.data());
  if (!temporary)
    throw std::runtime_error("cannot stage package removal in " +
                             root.string());
  const fs::path staging(temporary);
  std::size_t moved = 0;
  try {
    for (const auto &version : versions) {
      fs::rename(directory / version / selected_variant, staging / version);
      ++moved;
    }
  } catch (...) {
    while (moved > 0) {
      const auto &version = versions[--moved];
      std::error_code error;
      fs::rename(staging / version, directory / version / selected_variant, error);
    }
    std::error_code error;
    fs::remove(staging, error);
    throw;
  }
  fs::remove_all(staging);
  // Keep unrelated files or checkout directories under the package's name.
  std::error_code error;
  fs::remove(directory, error);
  return versions;
}

PackageResolver::PackageResolver(fs::path directory, fs::path home,
                                 bool use_lock)
    : entry_directory_(directory.empty()
                           ? fs::path{}
                           : fs::weakly_canonical(fs::absolute(directory))),
      home_(std::move(home)) {
  host_ = probe_package_host();
  source_directory_ = entry_directory_;
  if (use_lock)
    read_lockfile();
  for (const auto *name : {"sputnik.build.yaml", "sputnik.build.yml", "sputnik.build.json"}) {
    const auto path = entry_directory_ / name;
    if (!fs::is_regular_file(path)) continue;
    const auto parsed = build::parse_build_configuration(read(path), path.string());
    if (!parsed.ok()) throw std::runtime_error(build::diagnostics_to_string(parsed.diagnostics));
    entry_dependencies_ = parsed.manifest.dependencies;
    break;
  }
}

std::map<std::string, PackagePin> read_package_lock(const fs::path &path) {
  std::map<std::string, PackagePin> pins;
  std::istringstream lines(read(path));
  std::string line, schema;
  PackagePin pin;
  std::set<std::string> fields;
  std::string name;
  auto finish = [&] {
    if (name.empty())
      return;
    if (!registry_reference(name) || pin.version.empty() || pin.digest.size() != 71 ||
        pin.digest.rfind("sha256:", 0) != 0 || pin.digest.substr(7).find_first_not_of("0123456789abcdef") != std::string::npos ||
        (!pin.variant.empty() && !component(pin.variant)) || !pins.emplace(name, pin).second)
      throw std::runtime_error("duplicate/incomplete package lock entry: " +
                               name);
    name.clear();
    pin = {};
    fields.clear();
  };
  while (std::getline(lines, line)) {
    if (line == "[[packages]]") {
      finish();
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string::npos)
      continue;
    auto key = line.substr(0, equals);
    key.erase(std::remove_if(key.begin(), key.end(),
                             [](unsigned char c) { return std::isspace(c); }),
              key.end());
    std::istringstream value(line.substr(equals + 1));
    std::string text;
    if (!(value >> std::quoted(text)))
      throw std::runtime_error("invalid sputnik.lock string");
    if (key != "schema" && !fields.insert(key).second) throw std::runtime_error("duplicate sputnik.lock field: " + key);
    if (key == "schema") {
      schema = text;
      // Existing artifact lockfiles describe a different format; leave them to
      // package verification rather than interpreting their synthetic hashes.
      if (schema == "sputnik.lock.v1")
        return {};
      if (schema != "sputnik.packages.lock.v1")
        throw std::runtime_error("unsupported sputnik.lock schema: " + schema);
    } else if (key == "name")
      name = text;
    else if (key == "version")
      pin.version = text;
    else if (key == "digest")
      pin.digest = text;
    else if (key == "variant") pin.variant = text;
    else if (key == "source_kind") pin.source_kind = text;
    else if (key == "source") pin.source = text;
    else
      throw std::runtime_error("unsupported sputnik.lock field: " + key);
  }
  finish();
  if (schema.empty())
    throw std::runtime_error("sputnik.lock is missing its schema");
  return pins;
}

void PackageResolver::read_lockfile() {
  if (entry_directory_.empty() || !fs::is_regular_file(entry_directory_ / "sputnik.lock")) return;
  // Embedded artifact locks retain their historical meaning.
  std::istringstream lines(read(entry_directory_ / "sputnik.lock"));
  std::string line;
  while (std::getline(lines,line)) {
    const auto equals = line.find('='); if (equals == std::string::npos) continue;
    auto key = line.substr(0,equals);
    key.erase(std::remove_if(key.begin(),key.end(),[](unsigned char c) { return std::isspace(c); }),key.end());
    if (key != "schema") continue;
    std::istringstream value(line.substr(equals+1)); std::string schema;
    if (value >> std::quoted(schema) && schema == "sputnik.lock.v1") return;
    break;
  }
  pins_ = read_package_lock(entry_directory_ / "sputnik.lock");
  locked_ = true;
  std::vector<PackageDependency> local;
  for (const auto &[name, pin] : pins_)
    if (pin.source_kind == "path") local.push_back({name, pin.version, "path", {}, pin.source});
  bind_paths(local, entry_directory_);
}

void PackageResolver::discover() {
  if (discovered_)
    return;
  discovered_ = true;
  if (home_.empty())
    home_ = package_home();
  const auto scan = [&](const fs::path &root, int priority) {
    std::function<void(const fs::path &, unsigned)> walk;
    walk = [&](const fs::path &directory, unsigned depth) {
      for (const auto &path : children(directory)) {
        if (!fs::is_directory(path)) continue;
        const auto relative = path.lexically_relative(root);
        auto name = relative.parent_path().generic_string();
        auto v = path.filename().string();
        auto artifact = path / artifact_filename(name, v);
        if (fs::is_regular_file(path / "modules.index") && fs::is_regular_file(path / "sputnik.build.json")) {
          // Distribution manifests are small verified metadata; bytecode/native
          // payloads remain unopened until the package is actually activated.
          auto metadata = directory_package(path);
          name = metadata->manifest.name; v = metadata->manifest.version;
          artifact = path / artifact_filename(name,v);
        }
        if (depth && fs::is_regular_file(artifact)) {
          LocatedPackage package;
          package.directory = fs::canonical(path);
          package.artifact_path = fs::canonical(artifact);
          const auto index_path = path / "modules.index";
          if (fs::is_regular_file(path / "sputnik.build.json")) {
            package = *directory_package(path);
            package.artifact_path = fs::canonical(artifact);
            const auto provenance = path / ".registry-source";
            if (fs::is_regular_file(provenance) && !fs::is_symlink(provenance)) {
              std::istringstream in(read(provenance)); std::getline(in, package.source); package.source_kind = "registry";
            }
            std::istringstream index(read(index_path)); std::string ignored;
            for (int i=0; i<3; ++i) std::getline(index,ignored);
            std::getline(index,package.digest);
          } else if (fs::is_regular_file(index_path)) {
            std::istringstream index(read(index_path));
            std::string schema, module;
            std::getline(index, schema);
            std::getline(index, package.manifest.name);
            std::getline(index, package.manifest.version);
            std::getline(index, package.digest);
            if (schema != "sputnik.package.index.v1" || package.digest.size() != 71)
              throw std::runtime_error("invalid installed package index: " + index_path.string());
            while (std::getline(index, module))
              if (!module.empty()) package.manifest.modules.push_back({module, {}});
          } else package = read_package(artifact);
          if (package.manifest.name != name || package.manifest.version != v)
            throw std::runtime_error("package identity disagrees with its installation path: " + artifact.string());
          candidates_.push_back({std::move(package), priority});
        } else if (fs::exists(path / "modules.index")) {
          throw std::runtime_error("missing installed package artifact: " + artifact.string());
        } else if (auto package = directory_package(path)) {
          candidates_.push_back({std::move(*package), priority});
        } else if (depth < 3) walk(path, depth + 1);
      }
    };
    walk(root, 0);
  };
  if (!entry_directory_.empty())
    scan(entry_directory_ / "packages", 0);
  // A remote notebook keeps its installed packages outside immutable source
  // revisions. Its worker sets this process-local overlay before compilation;
  // entry-directory locks and the shared package fallback retain their paths.
  if (const auto *notebook_home = std::getenv("SPUTNIK_NOTEBOOK_PACKAGE_HOME"))
    if (*notebook_home && (entry_directory_.empty() ||
        fs::path(notebook_home) != entry_directory_ / "packages"))
      scan(notebook_home, 0);
  scan(home_, 1);
}

bool PackageResolver::shadowed(const Candidate &candidate) const {
  return std::any_of(
      candidates_.begin(), candidates_.end(), [&](const auto &other) {
        return other.priority < candidate.priority &&
               other.package.manifest.name == candidate.package.manifest.name;
      });
}

void PackageResolver::bind_paths(
    const std::vector<PackageDependency> &dependencies,
    const fs::path &declaring_directory) {
  for (const auto &dependency : dependencies) {
    if (dependency.source != "path") continue;
    if (!(component(dependency.name) || registry_reference(dependency.name)) || dependency.path.empty())
      throw std::runtime_error("invalid path dependency: " + dependency.name);
    const auto directory = fs::canonical(declaring_directory / dependency.path);
    auto package = read_source_package(directory);
    if (package.manifest.name != dependency.name)
      throw std::runtime_error("path dependency name mismatch: expected " + dependency.name +
                               ", found " + package.manifest.name + " in " + directory.string());
    if (!dependency.version.empty() && dependency.version != package.manifest.version)
      throw std::runtime_error("path dependency version mismatch: " + dependency.name);
    const auto inserted = paths_.emplace(dependency.name, directory);
    if (!inserted.second) {
      if (inserted.first->second != directory)
        throw std::runtime_error("conflicting path dependencies for " + dependency.name);
      continue;
    }
    const auto active = active_.find(dependency.name);
    if (active != active_.end() && selected_[active->second].directory != directory)
      throw std::runtime_error("cannot replace an active package with a different path: " + dependency.name);
    candidates_.push_back({package, -1});
    bind_paths(package.manifest.dependencies, package.directory);
  }
}

void PackageResolver::require(
    const std::vector<PackageDependency> &dependencies,
    const fs::path &declaring_directory) {
  if (entry_dependencies_pending_) {
    entry_dependencies_pending_ = false;
    auto combined = std::move(entry_dependencies_);
    combined.insert(combined.end(), dependencies.begin(), dependencies.end());
    require(combined, entry_directory_);
    return;
  }
  const auto base = declaring_directory.empty() ? entry_directory_ : declaring_directory;
  // Bind every local override before activating any dependency in this batch.
  bind_paths(dependencies, base);
  // Record every exact requirement before choosing a version, so declaration
  // order cannot accidentally activate latest ahead of another explicit pin.
  for (const auto &dependency : dependencies) {
    if (!(component(dependency.name) || registry_reference(dependency.name)) ||
        (dependency.source != "path" && !component(dependency.version)))
      throw std::runtime_error(
          "package dependencies currently require exact versions: " +
          dependency.name + " " + dependency.version);
    if (dependency.version.empty()) continue;
    const auto inserted =
        requirements_.emplace(dependency.name, dependency.version);
    if (!inserted.second && inserted.first->second != dependency.version)
      throw std::runtime_error("conflicting package versions for " +
                               dependency.name);
  }
  for (const auto &dependency : dependencies)
    require_one(dependency);
}

void PackageResolver::probe_accelerators_for(const PackageManifest &manifest,
                                            const VariantPolicy &policy) {
  const auto &target = manifest.target;
  const bool gpu = package_gpu_backend(target.compute_backend) ||
      std::any_of(target.compute_backends.begin(), target.compute_backends.end(),
                  [](const auto &backend) { return package_gpu_backend(backend); });
  if (!accelerators_probed_ && gpu && (policy.backend == "auto" ||
      package_gpu_backend(policy.backend) || !policy.variant.empty())) {
    host_ = probe_package_host(true);
    accelerators_probed_ = true;
  }
}

void PackageResolver::require_one(const PackageDependency &dependency) {
  discover();
  const auto active = active_.find(dependency.name);
  if (active != active_.end()) {
    if (!dependency.version.empty() && selected_[active->second].manifest.version != dependency.version)
      throw std::runtime_error("conflicting package versions for " +
                               dependency.name);
    return;
  }
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < candidates_.size(); ++i) {
    const auto &candidate = candidates_[i];
    if (candidate.package.manifest.name != dependency.name ||
        (!dependency.version.empty() && candidate.package.manifest.version != dependency.version) ||
        shadowed(candidate))
      continue;
    const auto pin = pins_.find(dependency.name);
    if (pin != pins_.end() && (candidate.package.manifest.version != pin->second.version ||
        (!pin->second.variant.empty() && candidate.package.manifest.variant != pin->second.variant))) continue;
    auto policy = selection_;
    if (pin != pins_.end()) policy.variant = pin->second.variant;
    else if (variants_.count(dependency.name)) policy.variant = variants_.at(dependency.name);
    probe_accelerators_for(candidate.package.manifest, policy);
    const auto problem = package_incompatibility(candidate.package.manifest, host_, policy, true);
    if (!problem.empty()) {
      if (pin != pins_.end() && candidate.package.manifest.variant == pin->second.variant)
        throw std::runtime_error("LOCK_TARGET_MISMATCH: " + dependency.name + ": " + problem);
      continue;
    }
    if (!best || candidate.priority < candidates_[*best].priority)
      best = i;
    else if (candidate.priority == candidates_[*best].priority && pin == pins_.end()) {
      const auto choice = select_package_variant({candidates_[*best].package.manifest, candidate.package.manifest},host_,policy,true);
      if (choice == 1) best = i;
    }
  }
  if (!best)
    throw std::runtime_error("missing package " + dependency.name + "@" +
                             dependency.version +
                             "; run: sputnik pkg install " + dependency.name +
                             " --version " + dependency.version);
  activate(*best);
}

void PackageResolver::activate(std::size_t index) {
  auto package = candidates_.at(index).package;
  const auto &name = package.manifest.name;
  const auto already = active_.find(name);
  if (already != active_.end()) {
    const auto &current = selected_[already->second];
    if (current.directory != package.directory)
      throw std::runtime_error("cannot mix two versions/builds of package " +
                               name);
    return;
  }
  if (!package.artifact_path.empty()) {
    auto verified = read_package(package.artifact_path);
    if (verified.manifest.name != package.manifest.name ||
        verified.manifest.version != package.manifest.version ||
        (!package.digest.empty() && verified.digest != package.digest))
      throw std::runtime_error(
          "installed artifact differs from package index: " +
          package.artifact_path.string());
    package = std::move(verified);
    if (!package.artifact) verify_distribution_members(package);
    for (const auto &dependency : package.manifest.dependencies)
      if (dependency.source == "path")
        throw std::runtime_error("installed packages cannot contain local path dependencies: " + name);
  } else {
    std::string material = package.digest;
    for (const auto &module : package.manifest.modules)
      material += "\n" + module.name + "\n" +
                  read(member(package.directory, module.path));
    for (const auto &extension : package.manifest.native_extensions)
      for (const auto *paths : {&extension.sources, &extension.headers})
        for (const auto &path : *paths)
          material +=
              "\n" + path + "\n" + read(member(package.directory, path));
    package.digest = "sha256:" + lexer::sha256_hex(material);
    std::string graph = package.manifest.name + "\n" + package.manifest.version + "\n" + package.manifest.variant;
    for (const auto &dependency : package.manifest.dependencies)
      graph += "\n" + dependency.name + "\n" + dependency.version + "\n" + dependency.source + "\n" +
               fs::path(dependency.path).lexically_normal().generic_string();
    package.lock_digest = "sha256:" + lexer::sha256_hex(graph);
    package.source_kind = "path";
    package.source = package.directory.lexically_relative(entry_directory_).generic_string();
    if (package.source.empty()) package.source = package.directory.generic_string();
  }
  const auto pin = pins_.find(name);
  if (locked_ && pin == pins_.end())
    throw std::runtime_error("package is missing from sputnik.lock: " + name +
                             "; run sputnik pkg lock to update it");
  if (pin != pins_.end() && (pin->second.version != package.manifest.version ||
                             pin->second.digest != (package.lock_digest.empty() ? package.digest : package.lock_digest) ||
                             (!pin->second.variant.empty() && pin->second.variant != package.manifest.variant) ||
                             (!pin->second.source_kind.empty() && pin->second.source_kind != package.source_kind) ||
                             (!pin->second.source.empty() && pin->second.source != package.source)))
    throw std::runtime_error("package differs from sputnik.lock: " + name);
  auto execution = selection_;
  execution.variant = package.manifest.variant;
  probe_accelerators_for(package.manifest, execution);
  const auto incompatible = package_incompatibility(package.manifest, host_, execution, true);
  if (!incompatible.empty()) throw std::runtime_error("LOCK_TARGET_MISMATCH: " + name + ": " + incompatible);
  const auto matches = [](const PackageNativeRequirement &r, const PackageNativeProvider &p) {
    return r.version == p.version && (r.abi.empty() || r.abi == p.abi) && (r.build_id.empty() || r.build_id == p.build_id);
  };
  for (const auto &r : package.manifest.native_requirements) {
    const auto key = r.kind + ":" + r.id;
    auto previous = native_requirements_.find(key);
    if (previous != native_requirements_.end()) {
      if (previous->second.version != r.version || (!previous->second.abi.empty() && !r.abi.empty() && previous->second.abi != r.abi) ||
          (!previous->second.build_id.empty() && !r.build_id.empty() && previous->second.build_id != r.build_id))
        throw std::runtime_error("NATIVE_ABI_CONFLICT: incompatible technical requirements for " + r.id);
      if (previous->second.abi.empty()) previous->second.abi = r.abi;
      if (previous->second.build_id.empty()) previous->second.build_id = r.build_id;
    } else native_requirements_.emplace(key,r);
    const auto provider = native_providers_.find(key);
    if (provider != native_providers_.end() && !matches(r,provider->second)) throw std::runtime_error("NATIVE_ABI_CONFLICT: " + r.id);
  }
  for (const auto &p : package.manifest.native_providers) {
    const auto key = p.kind + ":" + p.id;
    const auto previous = native_providers_.find(key);
    if (previous != native_providers_.end() && (previous->second.version != p.version || previous->second.abi != p.abi || previous->second.build_id != p.build_id))
      throw std::runtime_error("NATIVE_ABI_CONFLICT: conflicting singleton providers for " + p.id);
    const auto requirement = native_requirements_.find(key);
    if (requirement != native_requirements_.end() && !matches(requirement->second,p)) throw std::runtime_error("NATIVE_ABI_CONFLICT: " + p.id);
    native_providers_.emplace(key,p);
  }
  active_[name] = selected_.size();
  selected_.push_back(package);
  require(package.manifest.dependencies, package.directory);
  require(package.manifest.artifact_dependencies, package.directory);
}

std::optional<ResolvedPackageModule>
PackageResolver::resolve(const std::string &module) {
  if (entry_dependencies_pending_) require({});
  const auto local = source_modules_.find(module);
  if (local != source_modules_.end()) {
    ResolvedPackageModule result;
    result.name = module;
    result.path = local->second.string();
    result.source = read(local->second);
    return result;
  }
  if (!source_directory_.empty() && !module.empty() &&
      module.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") == std::string::npos) {
    std::vector<std::string> names{module};
    if (!source_prefix_.empty() && module.rfind(source_prefix_, 0) == 0)
      names.push_back(module.substr(source_prefix_.size()));
    std::set<fs::path> matches;
    for (auto name : names) {
      std::replace(name.begin(), name.end(), '.', '/');
      for (const auto *extension : {".s", ".spu", ".sputnik"}) {
        const auto file = source_directory_ / (name + extension);
        if (fs::is_regular_file(file)) matches.insert(fs::canonical(file));
      }
    }
    if (matches.size() > 1) throw std::runtime_error("ambiguous local module: " + module);
    if (!matches.empty()) {
      ResolvedPackageModule result;
      result.name = module;
      result.path = matches.begin()->string();
      result.source = read(*matches.begin());
      return result;
    }
  }
  discover();
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < candidates_.size(); ++i) {
    const auto &candidate = candidates_[i];
    const auto &manifest = candidate.package.manifest;
    if (shadowed(candidate))
      continue;
    if (std::none_of(manifest.modules.begin(), manifest.modules.end(),
                     [&](const auto &entry) { return entry.name == module; }))
      continue;
    const auto requirement = requirements_.find(manifest.name);
    const auto pin = pins_.find(manifest.name);
    if (requirement != requirements_.end() &&
        manifest.version != requirement->second)
      continue;
    if (pin != pins_.end() && manifest.version != pin->second.version)
      continue;
    if (pin != pins_.end() && !pin->second.variant.empty() && manifest.variant != pin->second.variant) continue;
    auto execution = selection_;
    if (pin != pins_.end()) execution.variant = pin->second.variant;
    else if (variants_.count(manifest.name)) execution.variant = variants_.at(manifest.name);
    probe_accelerators_for(manifest, execution);
    const auto problem = package_incompatibility(manifest, host_, execution, true);
    if (!problem.empty()) {
      if (pin != pins_.end() && manifest.variant == pin->second.variant) throw std::runtime_error("LOCK_TARGET_MISMATCH: " + manifest.name + ": " + problem);
      continue;
    }
    const auto active = active_.find(manifest.name);
    if (active != active_.end() &&
        selected_[active->second].directory != candidate.package.directory)
      continue;
    if (!best || candidate.priority < candidates_[*best].priority) {
      best = i;
      continue;
    }
    const auto &previous = candidates_[*best];
    if (candidate.priority != previous.priority)
      continue;
    if (manifest.name != previous.package.manifest.name)
      throw std::runtime_error("ambiguous module " + module + ": provided by " +
                               previous.package.manifest.name + " and " +
                               manifest.name);
    const auto &old_version = previous.package.manifest.version;
    if (old_version == manifest.version &&
        previous.package.directory != candidate.package.directory) {
      const auto choice = select_package_variant({previous.package.manifest, manifest},host_,execution,true);
      if (choice == 1) best = i;
      continue;
    }
    const bool stable = version(manifest.version).prerelease.empty();
    const bool old_stable = version(old_version).prerelease.empty();
    if ((stable && !old_stable) ||
        (stable == old_stable && version_less(old_version, manifest.version)))
      best = i;
  }
  if (!best)
    return std::nullopt;
  activate(*best);
  const auto &package =
      selected_.at(active_.at(candidates_[*best].package.manifest.name));
  ResolvedPackageModule result;
  result.name = module;
  if (!package.artifact_path.empty()) {
    result.package_artifact = package.artifact_path.string();
    result.package_digest = package.digest;
  }
  if (package.artifact) {
    for (const auto &entry : package.artifact->modules)
      if (entry.name == module) {
        result.path = package.artifact_path.string() + ":" + entry.path;
        result.bytecode = entry.bytes;
        result.package_artifact = package.artifact_path.string();
        result.package_digest = package.digest;
        return result;
      }
  } else {
    for (const auto &entry : package.manifest.modules)
      if (entry.name == module) {
        const auto path = member(package.directory, entry.path);
        result.path = path.string();
        const auto contents = read(path);
        if (entry.representation == "bytecode") {
          result.bytecode.assign(contents.begin(), contents.end());
          if (!bytecode::deserialize_module(result.bytecode).ok())
            throw std::runtime_error("invalid package bytecode: " + module);
        } else {
          result.source = contents;
        }
        return result;
      }
  }
  throw std::runtime_error("package module is missing: " + module);
}

void PackageResolver::register_source(const std::string &module, const fs::path &file) {
  if (module.empty()) return;
  const auto canonical = fs::canonical(file);
  const auto found = source_modules_.find(module);
  if (found != source_modules_.end() && found->second != canonical)
    throw std::runtime_error("conflicting local module providers for " + module + ": " +
                             found->second.string() + " and " + canonical.string());
  for (const auto &entry : source_modules_)
    if (entry.second == canonical && entry.first != module)
      throw std::runtime_error("local source has conflicting module identities: " + canonical.string());
  source_modules_[module] = canonical;
}

std::string PackageResolver::prepare_imports(
    std::vector<std::unique_ptr<ast::Expr>> &items, const fs::path &file,
    const std::string &module) {
  auto declaring_module = module;
  if (declaring_module.empty())
    for (const auto &item : items)
      if (item && item->kind == "AstPackageDecl")
        for (const auto &field : item->string_fields)
          if (field.name == "module_path") declaring_module = field.value;
  register_source(declaring_module, file);
  const auto directory = fs::canonical(file).parent_path();
  const auto locate = [](const fs::path &base, bool exact) -> fs::path {
    std::set<fs::path> matches;
    if (exact) {
      if (fs::is_regular_file(base)) matches.insert(fs::canonical(base));
    } else {
      for (const auto *extension : {".s", ".spu", ".sputnik"}) {
        const auto candidate = fs::path(base.string() + extension);
        if (fs::is_regular_file(candidate)) matches.insert(fs::canonical(candidate));
      }
    }
    if (matches.size() > 1) throw std::runtime_error("ambiguous local module: " + base.string());
    return matches.empty() ? fs::path{} : *matches.begin();
  };
  std::string material;
  for (auto &item : items) {
    if (!item || item->kind != "AstImportStmt") continue;
    const auto field = [&](const std::string &name) -> std::string {
      for (const auto &entry : item->string_fields) if (entry.name == name) return entry.value;
      return {};
    };
    const auto relative = field("relative_path");
    auto name = field("module_path");
    if (!relative.empty()) {
      const fs::path requested(relative);
      const auto extension = requested.extension().string();
      const bool exact = extension == ".s" || extension == ".spu" || extension == ".sputnik";
      const auto target = locate(directory / requested, exact);
      if (target.empty()) throw std::runtime_error("ImportError: relative module '" + relative + "' from " + file.string() + " was not found");
      auto lexed = lexer::Lexer(read(target), target.string()).lex();
      if (!lexed.ok()) throw std::runtime_error(lexer::diagnostics_to_json(lexed.diagnostics));
      parser::Parser parser(lexed.tokens);
      auto parsed = parser.parse_module_unit();
      if (!parsed.ok()) throw std::runtime_error(lexer::diagnostics_to_json(parsed.diagnostics));
      name = parsed.module_name;
      if (name.empty()) {
        for (const auto &entry : source_modules_)
          if (entry.second == target) name = entry.first;
        if (name.empty()) name = "sputnik.local.m" + lexer::sha256_hex(target.lexically_relative(entry_directory_).generic_string());
      }
      register_source(name, target);
      for (auto &entry : item->string_fields) {
        if (entry.name == "module_path") entry.value = name;
        if (entry.name == "alias" && entry.value.empty() && field("import_kind") == "module") {
          entry.value = exact ? requested.stem().string() : requested.filename().string();
          auto alias = lexer::Lexer(entry.value, file.string()).lex();
          if (!alias.ok() || alias.tokens.size() < 2 || alias.tokens[0].kind != lexer::TokenKind::Identifier ||
              alias.tokens[0].lexeme != entry.value)
            throw std::runtime_error("ImportError: relative import requires an explicit identifier alias: " + relative);
        }
      }
      material += "relative-import\n" + relative + "\n" + target.string() + "\n" + name + "\n";
    } else {
      // Local imports inside a loaded module retain that file's own context.
      auto path = name;
      std::replace(path.begin(), path.end(), '.', '/');
      auto target = locate(directory / path, false);
      const auto separator = declaring_module.rfind('.');
      if (target.empty() && separator != std::string::npos && name.rfind(declaring_module.substr(0, separator + 1), 0) == 0) {
        path = name.substr(separator + 1);
        std::replace(path.begin(), path.end(), '.', '/');
        target = locate(directory / path, false);
      }
      if (!target.empty()) {
        // A sibling filename can also name a different package (for example
        // orm/sqlite3.s versus the sqlite3 package). Its declaration must match.
        auto tokens = lexer::Lexer(read(target), target.string()).lex();
        if (tokens.ok()) {
          parser::Parser parser(tokens.tokens);
          auto parsed = parser.parse_module_unit();
          if (parsed.module_name == name) register_source(name, target);
        }
      }
    }
  }
  return material;
}

void PackageResolver::source_entry(const fs::path &file, const std::string &module) {
  source_directory_ = fs::weakly_canonical(fs::absolute(file).parent_path());
  if (fs::is_regular_file(file) && module.rfind("sputnik.entry.", 0) == 0)
    register_source(module, file);
  const auto separator = module.rfind('.');
  source_prefix_ = separator == std::string::npos ? "" : module.substr(0, separator + 1);
}

std::string PackageResolver::lockfile() const {
  auto packages = selected_;
  std::sort(packages.begin(), packages.end(), [](const auto &a, const auto &b) {
    return a.manifest.name < b.manifest.name;
  });
  std::ostringstream out;
  out << "schema = \"sputnik.packages.lock.v1\"\n";
  for (const auto &package : packages)
    out << "\n[[packages]]\nname = " << std::quoted(package.manifest.name)
        << "\nversion = " << std::quoted(package.manifest.version)
        << "\nvariant = " << std::quoted(package.manifest.variant)
        << "\ndigest = " << std::quoted(package.lock_digest.empty() ? package.digest : package.lock_digest)
        << "\nsource_kind = " << std::quoted(package.source_kind)
        << "\nsource = " << std::quoted(package.source) << "\n";
  return out.str();
}

std::string PackageResolver::search_description() const {
  return (entry_directory_.empty()
              ? ""
              : (entry_directory_ / "packages").string() + ", ") +
         home_.string();
}
} // namespace sputnik::pkg
