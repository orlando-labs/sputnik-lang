#pragma once

#include "frontend/ast/expr.h"
#include "frontend/foreign_import.h"

#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace sputnik::frontend {

// Import paths are matched by segments, with the longest replacement first.
// The entry program (or notebook cells) owns the graph-wide replacement table;
// dependency modules can still use their own ordinary local import aliases.
class ImportAliases {
public:
  void add(const std::string &alias, const std::string &target) {
    if (alias.empty() || target.empty() || alias == target) return;
    const auto previous = paths_.find(alias);
    if (previous != paths_.end() && previous->second != target)
      throw std::runtime_error("ImportError: conflicting import replacement for '" + alias +
                               "': '" + previous->second + "' and '" + target + "'");
    paths_[alias] = target;
  }

  void merge(const ImportAliases &other) {
    for (const auto &entry : other.paths_) add(entry.first, entry.second);
  }

  std::string resolve(std::string path) const {
    std::set<std::string> applied;
    for (;;) {
      const std::pair<const std::string, std::string> *best = nullptr;
      for (const auto &entry : paths_) {
        if (!prefix(path, entry.first)) continue;
        // Preserve existing aliases such as `import a.b as a`: the
        // provider's own canonical namespace must not grow to a.b.b....
        if (prefix(entry.second, entry.first) && prefix(path, entry.second)) continue;
        if (!best || entry.first.size() > best->first.size()) best = &entry;
      }
      if (!best) return path;
      if (!applied.insert(best->first).second)
        throw std::runtime_error("ImportError: cyclic import replacement involving '" + best->first + "'");
      path = best->second + path.substr(best->first.size());
    }
  }

  void validate() const {
    for (const auto &entry : paths_) (void)resolve(entry.first);
  }
  bool empty() const { return paths_.empty(); }
  const std::map<std::string, std::string> &paths() const { return paths_; }

  static bool prefix(const std::string &path, const std::string &head) {
    return path == head || (path.size() > head.size() &&
        path.compare(0, head.size(), head) == 0 && path[head.size()] == '.');
  }

private:
  std::map<std::string, std::string> paths_;
};

inline std::string import_field(const ast::Expr &item, const std::string &name) {
  for (const auto &field : item.string_fields)
    if (field.name == name) return field.value;
  return {};
}

inline ImportAliases collect_import_aliases(
    const std::vector<std::unique_ptr<ast::Expr>> &items) {
  ImportAliases aliases;
  for (const auto &item : items) {
    if (!item || item->kind != "AstImportStmt" ||
        import_field(*item, "import_kind") != "module") continue;
    const auto path = import_field(*item, "module_path");
    if (foreign_import_provider(path).empty())
      aliases.add(import_field(*item, "alias"), path);
  }
  aliases.validate();
  return aliases;
}

} // namespace sputnik::frontend
