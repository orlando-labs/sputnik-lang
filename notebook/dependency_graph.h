#pragma once

#include "notebook/model.h"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace amber::notebook {

// A source-ordered dependency graph for one notebook sheet.  Rebuilding is
// cheap enough for an interactive document and, importantly, is deterministic:
// a read in cell N resolves to the latest provider strictly before N.
class DependencyGraph {
public:
  DependencyGraph() = default;
  explicit DependencyGraph(
      const std::vector<CellSource> &cells,
      const std::set<std::string> &ambient_names = {});

  // Ambient names belong to the immutable environment generation selected by
  // the host.  They are copied into the graph and never inferred or mutated
  // while cells are rebuilt.
  const std::set<std::string> &ambient_names() const { return ambient_names_; }

  // Replaces the graph while preserving the CellId identities supplied by the
  // document.  A zero or duplicate ID is retained in analysis but omitted from
  // graph edges and reported through diagnostics().
  void rebuild(const std::vector<CellSource> &cells);
  void rebuild(const std::vector<CellSource> &cells,
               const std::set<std::string> &ambient_names);
  void swap(DependencyGraph &other) noexcept;

  const std::vector<CellId> &cell_order() const { return order_; }
  const std::vector<CellId> &execution_order() const { return execution_; }
  std::vector<CellId> execution_order(const std::set<CellId> &subset) const;

  const CellAnalysis *analysis(CellId id) const;
  const std::vector<DependencyEdge> &edges() const { return edges_; }

  // Returns the provider selected for an external read in consumer.  A
  // missing name returns nullopt; local same-cell reads are not external reads
  // and have no provider entry.
  std::optional<BindingKey> provider_for(CellId consumer,
                                         const std::string &name) const;
  std::vector<BindingRead> reads_for(CellId consumer) const;
  std::vector<BindingKey> writes_for(CellId producer) const;
  const std::set<std::string> &missing_names(CellId consumer) const;

  // Direct or transitive dependents in stable document order.
  std::vector<CellId> direct_dependents_of(CellId provider) const;
  std::vector<CellId> dependents_of(CellId provider) const;

  // The roots plus every transitive dependent, returned in a valid execution
  // order.  This is the scheduler's invalidation plan after an edit or
  // published watched value.
  std::vector<CellId> invalidation_plan(const std::set<CellId> &changed) const;
  std::vector<CellId> invalidation_plan(CellId changed) const;

  // Invalidates consumers from both graph versions.  This is required when an
  // edit removes or renames an output: those consumers are no longer reachable
  // in the rebuilt graph, but their previously published results are stale.
  std::vector<CellId>
  transition_invalidation_plan(const DependencyGraph &previous,
                               const std::set<CellId> &changed) const;
  std::vector<CellId>
  transition_invalidation_plan(const DependencyGraph &previous,
                               CellId changed) const;

  // Strongly connected components with at least one cycle.  The current
  // nearest-preceding provider policy normally makes the graph acyclic, but
  // exposing this result keeps the API useful when runtime/dynamic edges are
  // layered on later.
  const std::vector<std::vector<CellId>> &cycles() const { return cycles_; }
  bool has_cycle() const { return !cycles_.empty(); }

  // Duplicate/zero IDs and other rebuild-time structural issues.  Frontend
  // parse diagnostics remain attached to each CellAnalysis.
  const std::vector<std::string> &diagnostics() const { return diagnostics_; }

private:
  struct ReadKey {
    CellId consumer = 0;
    std::string name;

    bool operator==(const ReadKey &other) const {
      return consumer == other.consumer && name == other.name;
    }
  };

  struct ReadKeyHash {
    std::size_t operator()(const ReadKey &key) const noexcept;
  };

  std::vector<CellId> topological_order(const std::set<CellId> *subset) const;
  void detect_cycles();

  std::vector<CellId> order_;
  std::unordered_map<CellId, std::size_t> positions_;
  std::unordered_map<CellId, CellAnalysis> analyses_;
  std::unordered_map<ReadKey, std::optional<BindingKey>, ReadKeyHash>
      providers_for_reads_;
  std::unordered_map<CellId, std::vector<DependencyEdge>> incoming_;
  std::unordered_map<CellId, std::vector<CellId>> dependents_;
  std::vector<DependencyEdge> edges_;
  std::vector<CellId> execution_;
  std::vector<std::vector<CellId>> cycles_;
  std::unordered_map<CellId, std::set<std::string>> missing_;
  std::vector<std::string> diagnostics_;
  std::set<std::string> ambient_names_;
};

} // namespace amber::notebook
