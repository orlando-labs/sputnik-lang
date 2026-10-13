#pragma once
#include "notebook/project.h"
#include "tools/isputnik/session.h"

#include <filesystem>
#include <vector>

// Read manifest-listed sources only. No installation, compilation, code
// execution, cwd-based search, or writes into the dependency directory.
std::vector<BundledModuleSource> load_directory_dependency(
    const std::filesystem::path &project_directory,
    const sputnik::notebook::ProjectDependency &dependency);

// Explicit package preparation only: update an existing native link without
// evaluating cells/loading a library or replacing other project metadata.
bool update_project_native_dependency(
    const std::filesystem::path &project_directory,
    const std::filesystem::path &package_directory,
    const std::string &manifest, const std::string &library);

// Read a package's prepared dependency descriptor without dlopen/evaluation.
sputnik::notebook::ProjectDependency prepared_native_dependency(
    const std::filesystem::path &package_directory);
void register_project_native_updates(const std::filesystem::path &package_directory,
                                     const std::filesystem::path &project_directory);

// Explicit execution-time trust boundary; opening a project never calls this.
// Prebuilt libraries are pinned by SHA-256. No compiler, shell or PATH search.
void load_prepared_native_dependencies(const sputnik::notebook::LoadedProject &,
                                      const sputnik::bytecode::BcModule &, bool authorized);
