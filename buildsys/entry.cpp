#include "buildsys/build.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace sputnik::build {
BuildManifestResult resolve_entry_module(const BuildManifest &manifest,
                                        const std::filesystem::path &directory) {
  BuildManifestResult result;
  result.manifest = manifest;
  if (manifest.entry.empty()) return result;
  try {
    const auto file = std::filesystem::canonical(directory / manifest.entry);
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read entry: " + file.string());
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    lexer::Lexer lexer(source, file.string());
    auto lexed = lexer.lex();
    if (!lexed.ok()) throw std::runtime_error(lexer::diagnostics_to_json(lexed.diagnostics));
    parser::Parser parser(lexed.tokens);
    auto parsed = parser.parse_module_unit();
    if (!parsed.ok()) throw std::runtime_error(lexer::diagnostics_to_json(parsed.diagnostics));
    if (parsed.module_name.empty()) throw std::runtime_error("configured entry must declare package: " + file.string());
    if (!manifest.root_module.empty() && manifest.root_module != parsed.module_name)
      throw std::runtime_error("entry package does not match configured root: " + file.string());
    result.manifest.root_module = parsed.module_name;
    if (result.manifest.name.empty()) result.manifest.name = parsed.module_name;
    auto &modules = result.manifest.modules;
    const auto existing = std::find_if(modules.begin(), modules.end(), [&](const auto &module) { return module.name == parsed.module_name; });
    if (existing == modules.end()) modules.push_back({parsed.module_name, file.string(), false, {}});
    else if (std::filesystem::canonical(directory / existing->path) != file)
      throw std::runtime_error("entry has a conflicting module provider: " + parsed.module_name);
  } catch (const std::exception &error) {
    result.diagnostics.push_back({"BuildManifestError", error.what(), directory.string()});
  }
  return result;
}

BuildManifestResult parse_build_manifest(const std::string &source,
                                        const std::string &path) {
  auto first = source.find_first_not_of(" \t\r\n");
  auto result = first != std::string::npos && (source[first] == '{' || source[first] == '[')
      ? parse_build_manifest_json(source, path) : parse_build_manifest_yaml(source, path);
  if (result.ok() && !result.manifest.entry.empty() && !path.empty())
    return resolve_entry_module(result.manifest, std::filesystem::absolute(path).parent_path());
  return result;
}


} // namespace sputnik::build
