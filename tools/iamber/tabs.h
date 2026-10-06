#pragma once

#include "tools/iamber/dispatch.h"
#include "tools/iamber/project_session.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Stable owner-thread state. Switching selects an existing object; it never
// replaces a Session underneath workers, callbacks or an activity waiter.
struct ProjectTab {
  Session session;
  std::optional<amber::notebook::LoadedProjectModule> module;
  bool edit_mode = false;
  RuntimePumpDispatch runtime_dispatch;
  SessionActivityWaiter activity;
  std::uint64_t environment_generation = 1;
};

class ProjectTabs {
public:
  explicit ProjectTabs(amber::notebook::LoadedProject project,
                       const std::string &initial_sheet = {},
                       const std::string &initial_module = {});
  ~ProjectTabs();
  ProjectTabs(const ProjectTabs &) = delete;
  ProjectTabs &operator=(const ProjectTabs &) = delete;

  ProjectTab &active() noexcept;
  const ProjectTab &active() const noexcept;
  const std::vector<std::unique_ptr<ProjectTab>> &tabs() const noexcept;
  std::size_t selected() const noexcept;
  bool switch_by(int delta);
  bool modified(std::size_t index) const;
  std::size_t modified_count() const;
  void save_active();
  // Explicit structure saves, independent of dirty editor buffers and Apply.
  // Creation selects a new stable tab; existing Sessions are never replaced.
  void create_sheet(const std::string &id, const std::string &title = {});
  void create_module(const std::string &id);
  bool set_auto_import(const std::string &module_id, bool enabled);
  bool apply_active(bool force_all = false, bool show_running = false,
                    const EvaluationProgress &progress = {});
  std::string tab_bar() const;
  const amber::notebook::LoadedProject &project() const noexcept;
  std::uint64_t environment_generation() const noexcept;
  SessionActivityWaiter activity_waiter() const noexcept;
  void close_activity() noexcept;
  const amber::runtime::NotebookInputSnapshot &inputs() const noexcept { return *inputs_; }
  bool set_input(const std::string &id, const std::string &json);
  void create_input(const amber::notebook::ProjectInput &input);
  void save_board(const amber::notebook::ProjectBoard &board);
  void add_dependency(const std::string &directory);
  void remove_dependency(const std::string &path);

private:
  void require_idle() const;
  void attach_new_tab(ProjectTab *tab);
  amber::notebook::LoadedProject project_;
  SessionActivityChannel activity_;
  std::vector<std::unique_ptr<ProjectTab>> tabs_;
  std::size_t selected_ = 0;
  std::uint64_t environment_generation_ = 1;
  std::vector<BundledModuleSource> environment_sources_;
  std::shared_ptr<const amber::runtime::NotebookInputSnapshot> inputs_;
};
