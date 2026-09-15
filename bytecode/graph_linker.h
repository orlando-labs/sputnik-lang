#pragma once

#include "bytecode/format.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace amber::bytecode {

// A decoded, verifier-approved module presented to the graph linker. The
// linker never reads or writes the filesystem; path is carried only so a
// caller can attach a useful diagnostic to a source artifact.
struct GraphModule {
  std::string name;
  std::string path;
  BcModule module;
  bool stdlib = false;
};

// Bump when merged-image semantics or the linker-visible bytecode ABI changes.
// Consumers use this to invalidate cached linked graphs independently of the
// source-module artifact cache.
inline constexpr std::uint32_t kGraphLinkerAbiVersion = 2U;

struct GraphExport {
  std::string provider;
  std::string public_name;
  // The provider-qualified live binding used by LOOKUP_CONST in the merged
  // image. It can differ from public_name for aliased functions/classes.
  std::string qualified_path;
  std::string kind;
  std::uint32_t code_id = 0;
  std::uint32_t method_index = 0;
  std::uint32_t class_index = 0;
};

struct GraphLinkDiagnostic {
  std::string error_name;
  std::string message;
  std::string module_name;
  std::string dependency_name;
  std::string export_name;
};

struct GraphLinkResult {
  bool ok = false;
  BcModule module;
  std::vector<GraphExport> exports;
  std::vector<std::string> init_order;
  bool has_entry_init_code_id = false;
  std::uint32_t entry_init_code_id = 0;
  bool has_entry_main_code_id = false;
  std::uint32_t entry_main_code_id = 0;
  std::vector<GraphLinkDiagnostic> diagnostics;
};

using ExternalNamespacePredicate =
    std::function<bool(const std::string &module_name)>;

// Links the root and its transitive bytecode dependencies into one verified
// ordinary image. The external namespace predicate admits dependencies
// supplied by the runtime (for example stdlib/native namespaces) without
// making this bytecode component depend on runtime registries.
// Reachable inputs and the output are verified. Dependency compatibility is
// checked for bytecode inputs; the host owns compatibility of external names.
// The image preserves the highest input format/language version. Its ABI hash
// is unset: no individual input's public-module ABI identifies a merged image.
GraphLinkResult link_graph(const std::vector<GraphModule> &modules,
                           const std::string &root_module,
                           ExternalNamespacePredicate is_external_namespace =
                               {});

// Links all roots and their transitive bytecode dependencies into one image.
// Roots are visited in the order supplied; each root's dependencies are
// emitted before that root and shared dependencies are emitted once.
GraphLinkResult link_graph(const std::vector<GraphModule> &modules,
                           const std::vector<std::string> &root_modules,
                           ExternalNamespacePredicate is_external_namespace =
                               {});

} // namespace amber::bytecode
