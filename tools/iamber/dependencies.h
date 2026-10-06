#pragma once
#include "notebook/project.h"
#include "tools/iamber/session.h"

#include <filesystem>
#include <vector>

// Read manifest-listed sources only. No installation, compilation, code
// execution, cwd-based search, or writes into the dependency directory.
std::vector<BundledModuleSource> load_directory_dependency(
    const std::filesystem::path &project_directory,
    const amber::notebook::ProjectDependency &dependency);

// Explicit execution-time trust boundary; opening a project never calls this.
// Prebuilt libraries are pinned by SHA-256. No compiler, shell or PATH search.
void load_prepared_native_dependencies(const amber::notebook::LoadedProject &,
                                      const amber::bytecode::BcModule &, bool authorized);
