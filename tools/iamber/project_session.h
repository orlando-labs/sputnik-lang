#pragma once

#include "notebook/project.h"
#include "tools/iamber/session.h"

// Owner-thread operations. Loading replaces the Session and closes its old
// activity channel/backend. Acquire wait handles AFTER loading, never before.
// A multi-tab host retains each loaded Session instead of reloading on switch.
std::vector<BundledModuleSource> load_project_bundled_sources(
    const amber::notebook::LoadedProject &project);
void load_project_into_session(Session *session,
                               const amber::notebook::LoadedProject &project,
                               const std::string &sheet_id = {});
amber::notebook::ProjectDocument
project_document_from_session(const Session &session,
                              const amber::notebook::LoadedProject &project);
// Unserializable in-progress edits count as modified so Quit can offer an
// explicit discard even when Save must reject the current document.
bool project_session_modified(const Session &session,
                              const amber::notebook::LoadedProject &project);
void save_project_session(Session *session,
                          amber::notebook::LoadedProject *project);

// Reloads module bytes through the persisted manifest's checked paths, then
// explicitly applies a fresh environment to the current sheet.
bool apply_project_environment(
    Session *session, const amber::notebook::LoadedProject &project,
    bool force_all = false, bool show_running = false,
    const EvaluationProgress &progress = {});

void load_project_module_into_session(
    Session *session, const amber::notebook::LoadedProject &project,
    const amber::notebook::LoadedProjectModule &module);
bool project_module_session_modified(
    const Session &session, const amber::notebook::LoadedProjectModule &module);
void save_project_module_session(Session *session,
                                 const amber::notebook::LoadedProject &project,
                                 amber::notebook::LoadedProjectModule *module);
