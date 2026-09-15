#include "notebook/dependency_graph.h"
#include "frontend/binder/binder.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <string>
#include <unordered_set>
#include <utility>

namespace amber::notebook {

namespace {

template <typename T>
void sort_by_position(
    std::vector<T> *values,
    const std::unordered_map<CellId, std::size_t> &positions) {
  std::sort(values->begin(), values->end(),
            [&positions](const T &left, const T &right) {
              const auto left_it = positions.find(left);
              const auto right_it = positions.find(right);
              const std::size_t left_position =
                  left_it == positions.end() ? static_cast<std::size_t>(-1)
                                             : left_it->second;
              const std::size_t right_position =
                  right_it == positions.end() ? static_cast<std::size_t>(-1)
                                              : right_it->second;
              return left_position < right_position;
            });
}

} // namespace

std::size_t
DependencyGraph::ReadKeyHash::operator()(const ReadKey &key) const noexcept {
  const std::size_t h1 = std::hash<CellId>{}(key.consumer);
  const std::size_t h2 = std::hash<std::string>{}(key.name);
  return h1 ^
         (h2 + static_cast<std::size_t>(0x9e3779b9U) + (h1 << 6U) + (h1 >> 2U));
}

DependencyGraph::DependencyGraph(
    const std::vector<CellSource> &cells,
    const std::set<std::string> &ambient_names)
    : ambient_names_(ambient_names) {
  rebuild(cells);
}

void DependencyGraph::swap(DependencyGraph &other) noexcept {
  order_.swap(other.order_);
  positions_.swap(other.positions_);
  analyses_.swap(other.analyses_);
  providers_for_reads_.swap(other.providers_for_reads_);
  incoming_.swap(other.incoming_);
  dependents_.swap(other.dependents_);
  edges_.swap(other.edges_);
  execution_.swap(other.execution_);
  cycles_.swap(other.cycles_);
  missing_.swap(other.missing_);
  diagnostics_.swap(other.diagnostics_);
  ambient_names_.swap(other.ambient_names_);
}

void DependencyGraph::rebuild(const std::vector<CellSource> &cells) {
  rebuild(cells, ambient_names_);
}

void DependencyGraph::rebuild(
    const std::vector<CellSource> &cells,
    const std::set<std::string> &ambient_names) {
  ambient_names_ = ambient_names;
  order_.clear();
  positions_.clear();
  analyses_.clear();
  providers_for_reads_.clear();
  incoming_.clear();
  dependents_.clear();
  edges_.clear();
  execution_.clear();
  cycles_.clear();
  missing_.clear();
  diagnostics_.clear();

  std::unordered_set<CellId> seen;
  for (std::size_t index = 0; index < cells.size(); ++index) {
    const CellSource &cell = cells[index];
    if (cell.id == 0) {
      diagnostics_.push_back("notebook cell at position " +
                             std::to_string(index) + " has a zero CellId");
      continue;
    }
    if (!seen.insert(cell.id).second) {
      diagnostics_.push_back("duplicate CellId " + std::to_string(cell.id));
      continue;
    }
    order_.push_back(cell.id);
    positions_.emplace(cell.id, index);
    analyses_.emplace(cell.id, analyze_cell(cell));
    missing_.emplace(cell.id, std::set<std::string>{});
    dependents_.emplace(cell.id, std::vector<CellId>{});
    incoming_.emplace(cell.id, std::vector<DependencyEdge>{});
  }

  // The latest provider map is intentionally local to this pass.  It is
  // updated only after a cell's reads have been resolved, which is what makes
  // `x = x + 1` read the previous cell's x rather than its own write.
  std::unordered_map<std::string, BindingKey> latest_provider;
  for (const CellId id : order_) {
    const auto analysis_it = analyses_.find(id);
    if (analysis_it == analyses_.end()) {
      continue;
    }
    const CellAnalysis &cell = analysis_it->second;
    for (const std::string &name : cell.reads) {
      ReadKey read_key{id, name};
      const auto provider_it = latest_provider.find(name);
      if (provider_it == latest_provider.end()) {
        providers_for_reads_.emplace(std::move(read_key), std::nullopt);
        if (ambient_names_.count(name) == 0U &&
            !binder::is_native_prelude_name(name)) {
          missing_[id].insert(name);
        }
        continue;
      }

      const BindingKey provider = provider_it->second;
      providers_for_reads_.emplace(std::move(read_key), provider);
      DependencyEdge edge{provider, id};
      edges_.push_back(edge);
      incoming_[id].push_back(edge);
      dependents_[provider.cell_id].push_back(id);
    }

    for (const std::string &name : cell.writes) {
      latest_provider[name] = BindingKey{id, name};
    }
  }

  for (auto &entry : dependents_) {
    sort_by_position(&entry.second, positions_);
    entry.second.erase(std::unique(entry.second.begin(), entry.second.end()),
                       entry.second.end());
  }
  execution_ = topological_order(nullptr);
  detect_cycles();
}

const CellAnalysis *DependencyGraph::analysis(CellId id) const {
  const auto it = analyses_.find(id);
  return it == analyses_.end() ? nullptr : &it->second;
}

std::optional<BindingKey>
DependencyGraph::provider_for(CellId consumer, const std::string &name) const {
  const auto it = providers_for_reads_.find(ReadKey{consumer, name});
  if (it == providers_for_reads_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<BindingRead> DependencyGraph::reads_for(CellId consumer) const {
  std::vector<BindingRead> result;
  const CellAnalysis *cell = analysis(consumer);
  if (cell == nullptr) {
    return result;
  }
  result.reserve(cell->reads.size());
  for (const std::string &name : cell->reads) {
    result.push_back(BindingRead{consumer, name, provider_for(consumer, name)});
  }
  return result;
}

std::vector<BindingKey> DependencyGraph::writes_for(CellId producer) const {
  std::vector<BindingKey> result;
  const CellAnalysis *cell = analysis(producer);
  if (cell == nullptr) {
    return result;
  }
  result.reserve(cell->writes.size());
  for (const std::string &name : cell->writes) {
    result.push_back(BindingKey{producer, name});
  }
  return result;
}

const std::set<std::string> &
DependencyGraph::missing_names(CellId consumer) const {
  static const std::set<std::string> empty;
  const auto it = missing_.find(consumer);
  return it == missing_.end() ? empty : it->second;
}

std::vector<CellId>
DependencyGraph::direct_dependents_of(CellId provider) const {
  const auto it = dependents_.find(provider);
  return it == dependents_.end() ? std::vector<CellId>{} : it->second;
}

std::vector<CellId> DependencyGraph::dependents_of(CellId provider) const {
  std::set<CellId> found;
  std::queue<CellId> pending;
  pending.push(provider);
  while (!pending.empty()) {
    const CellId current = pending.front();
    pending.pop();
    const auto it = dependents_.find(current);
    if (it == dependents_.end()) {
      continue;
    }
    for (const CellId dependent : it->second) {
      if (found.insert(dependent).second) {
        pending.push(dependent);
      }
    }
  }
  std::vector<CellId> result(found.begin(), found.end());
  sort_by_position(&result, positions_);
  return result;
}

std::vector<CellId> DependencyGraph::invalidation_plan(CellId changed) const {
  return invalidation_plan(std::set<CellId>{changed});
}

std::vector<CellId>
DependencyGraph::invalidation_plan(const std::set<CellId> &changed) const {
  std::set<CellId> affected;
  for (const CellId root : changed) {
    if (positions_.count(root) == 0U) {
      continue;
    }
    affected.insert(root);
    for (const CellId dependent : dependents_of(root)) {
      affected.insert(dependent);
    }
  }
  return execution_order(affected);
}

std::vector<CellId> DependencyGraph::transition_invalidation_plan(
    const DependencyGraph &previous, const std::set<CellId> &changed) const {
  std::set<CellId> affected;
  const std::vector<CellId> current_plan = invalidation_plan(changed);
  affected.insert(current_plan.begin(), current_plan.end());
  const std::vector<CellId> previous_plan = previous.invalidation_plan(changed);
  affected.insert(previous_plan.begin(), previous_plan.end());
  return execution_order(affected);
}

std::vector<CellId>
DependencyGraph::transition_invalidation_plan(const DependencyGraph &previous,
                                              CellId changed) const {
  return transition_invalidation_plan(previous, std::set<CellId>{changed});
}

std::vector<CellId>
DependencyGraph::execution_order(const std::set<CellId> &subset) const {
  return topological_order(&subset);
}

std::vector<CellId>
DependencyGraph::topological_order(const std::set<CellId> *subset) const {
  std::set<CellId> selected;
  if (subset == nullptr) {
    selected.insert(order_.begin(), order_.end());
  } else {
    for (const CellId id : *subset) {
      if (positions_.count(id) != 0U) {
        selected.insert(id);
      }
    }
  }

  std::unordered_map<CellId, std::size_t> indegree;
  for (const CellId id : selected) {
    indegree[id] = 0;
  }
  // Topological ordering is cell-based.  Multiple names read from the same
  // provider form one cell edge and must increase the indegree only once.
  for (const auto &entry : dependents_) {
    if (selected.count(entry.first) == 0U) {
      continue;
    }
    for (const CellId dependent : entry.second) {
      if (selected.count(dependent) != 0U) {
        ++indegree[dependent];
      }
    }
  }

  // A min-heap by document position makes output stable even when a future
  // dynamic edge changes the graph shape.
  auto by_position = [this](CellId left, CellId right) {
    return positions_.at(left) > positions_.at(right);
  };
  std::priority_queue<CellId, std::vector<CellId>, decltype(by_position)> ready(
      by_position);
  for (const CellId id : selected) {
    if (indegree[id] == 0U) {
      ready.push(id);
    }
  }

  std::vector<CellId> result;
  result.reserve(selected.size());
  while (!ready.empty()) {
    const CellId current = ready.top();
    ready.pop();
    result.push_back(current);
    const auto dep_it = dependents_.find(current);
    if (dep_it == dependents_.end()) {
      continue;
    }
    for (const CellId dependent : dep_it->second) {
      if (selected.count(dependent) == 0U) {
        continue;
      }
      auto degree_it = indegree.find(dependent);
      if (degree_it != indegree.end() && --degree_it->second == 0U) {
        ready.push(dependent);
      }
    }
  }
  // Keep cyclic members visible to a scheduler/diagnostic caller.  A valid
  // topological prefix is still useful, but deterministic document order for
  // the remainder avoids silently dropping cells.
  if (result.size() != selected.size()) {
    for (const CellId id : order_) {
      if (selected.count(id) != 0U &&
          std::find(result.begin(), result.end(), id) == result.end()) {
        result.push_back(id);
      }
    }
  }
  return result;
}

void DependencyGraph::detect_cycles() {
  std::unordered_map<CellId, int> index;
  std::unordered_map<CellId, int> lowlink;
  std::unordered_set<CellId> on_stack;
  std::vector<CellId> stack;
  int next_index = 0;

  std::function<void(CellId)> visit = [&](CellId current) {
    index[current] = next_index;
    lowlink[current] = next_index;
    ++next_index;
    stack.push_back(current);
    on_stack.insert(current);

    const auto it = dependents_.find(current);
    if (it != dependents_.end()) {
      for (const CellId next : it->second) {
        if (index.count(next) == 0U) {
          visit(next);
          lowlink[current] = std::min(lowlink[current], lowlink[next]);
        } else if (on_stack.count(next) != 0U) {
          lowlink[current] = std::min(lowlink[current], index[next]);
        }
      }
    }

    if (lowlink[current] != index[current]) {
      return;
    }
    std::vector<CellId> component;
    while (!stack.empty()) {
      const CellId member = stack.back();
      stack.pop_back();
      on_stack.erase(member);
      component.push_back(member);
      if (member == current) {
        break;
      }
    }
    if (component.size() > 1U) {
      sort_by_position(&component, positions_);
      cycles_.push_back(std::move(component));
    } else if (!component.empty()) {
      const CellId member = component.front();
      const auto dep_it = dependents_.find(member);
      if (dep_it != dependents_.end() &&
          std::find(dep_it->second.begin(), dep_it->second.end(), member) !=
              dep_it->second.end()) {
        cycles_.push_back(std::move(component));
      }
    }
  };

  for (const CellId id : order_) {
    if (index.count(id) == 0U) {
      visit(id);
    }
  }
  std::sort(cycles_.begin(), cycles_.end(),
            [this](const std::vector<CellId> &left,
                   const std::vector<CellId> &right) {
              return positions_.at(left.front()) < positions_.at(right.front());
            });
}

} // namespace amber::notebook
