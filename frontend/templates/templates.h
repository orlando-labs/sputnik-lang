#pragma once

#include "frontend/ast/expr.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sputnik::templates {

struct SourceFormat {
  std::string suffix;
  std::string module;
  std::string macro;
};

struct SourceInput {
  std::string path;
  std::string source;
};

struct ProviderUse {
  SourceFormat format;
  lexer::Span declaration;
};

struct PrepareResult {
  std::vector<lexer::Diagnostic> diagnostics;
  std::map<std::string, std::string> sources;
  std::vector<ProviderUse> providers;
  std::string cache_material;
  bool ok() const { return diagnostics.empty(); }
};

// The host owns filesystem access. The callback resolves a path relative to
// declaring_file and returns its canonical diagnostic path and contents.
using SourceLoader = std::function<SourceInput(
    const std::string &declaring_file, const std::string &relative_path)>;
PrepareResult prepare(std::vector<std::unique_ptr<ast::Expr>> &items,
                      const std::string &declaring_file,
                      const std::string &declaring_source,
                      const std::vector<SourceFormat> &formats,
                      const SourceLoader &loader);

} // namespace sputnik::templates
