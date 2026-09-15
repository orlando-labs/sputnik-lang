#include "notebook/compiler.h"

#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace amber::notebook {
namespace {

struct SourceKey {
  std::string file;
  std::size_t start = 0;
  std::size_t end = 0;

  bool operator<(const SourceKey &other) const {
    if (file != other.file) {
      return file < other.file;
    }
    if (start != other.start) {
      return start < other.start;
    }
    return end < other.end;
  }
};

SourceKey source_key(const lexer::Span &span) {
  return {span.file, span.start.offset, span.end.offset};
}

bool same_span(const lexer::Span &left, const lexer::Span &right) {
  return left.file == right.file && left.start.offset == right.start.offset &&
         left.end.offset == right.end.offset;
}

lexer::Diagnostic diagnostic(const lexer::Span &span, const std::string &code,
                             const std::string &message) {
  return {code, "error", "notebook", message, span};
}

bool is_external_reference_span(
    const lexer::Span &span,
    const std::vector<ExternalReadRef> &external_reads) {
  return std::any_of(external_reads.begin(), external_reads.end(),
                     [&span](const ExternalReadRef &read) {
                       return same_span(span, read.span);
                     });
}

bytecode::BcModule checked_notebook_base_module(bytecode::BcModule module) {
  if (module.notebook_metadata.has_value() ||
      (module.file_flags & bytecode::kFileFlagNotebookOnly) != 0U ||
      std::any_of(module.code_objects.begin(), module.code_objects.end(),
                  [](const bytecode::BcCode &code) {
                    return code.kind == bytecode::CodeKind::NotebookCell;
                  })) {
    throw std::invalid_argument(
        "composite notebook base must be an ordinary module image");
  }
  return module;
}

// LookupConst consumes a dotted path as a sequence of identifier symbols.
// Validate the ambient mapping here, at the source occurrence that requests
// it, so malformed project metadata cannot silently turn into a different
// runtime lookup.  Use the language lexer instead of an ASCII-only test: Amber
// permits the same Unicode identifier ranges in module and constant names.
bool valid_ambient_constant_path(const std::string &path) {
  if (path.empty() ||
      std::any_of(path.begin(), path.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
      })) {
    return false;
  }
  lexer::Lexer lexer(path, "<notebook-ambient-path>");
  const lexer::LexResult lexed = lexer.lex();
  if (!lexed.ok() || lexed.tokens.size() < 4U) {
    // At least `module.Constant` plus EOF is required.  A single-segment
    // LookupConst would not identify an ambient bundled export.
    return false;
  }
  const std::size_t last = lexed.tokens.size() - 1U;
  if (lexed.tokens[last].kind != lexer::TokenKind::Eof) {
    return false;
  }
  // The lexer inserts a synthetic trailing Newline for a source fragment
  // without one. It is not part of the path grammar.
  std::size_t token_end = last;
  if (token_end > 0U &&
      lexed.tokens[token_end - 1U].kind == lexer::TokenKind::Newline) {
    --token_end;
  }
  if (token_end < 3U) {
    return false;
  }
  for (std::size_t index = 0; index < token_end; ++index) {
    const lexer::TokenKind kind = lexed.tokens[index].kind;
    if ((index % 2U) == 0U) {
      if (kind != lexer::TokenKind::Identifier) {
        return false;
      }
    } else if (kind != lexer::TokenKind::Dot) {
      return false;
    }
  }
  return (token_end % 2U) == 1U;
}

// A notebook slot belongs to the executing cell, never to a worker callback.
// Snapshot external values at closure creation and pass them through ordinary
// lexical captures. For nested blocks this lifting repeats until the slot load
// reaches the root NotebookCell procedure.
void capture_notebook_inputs(hir::NotebookLoweringResult &lowered) {
  struct Input {
    std::string descriptor, capture_slot;
    lexer::Span span;
  };
  std::map<std::string, hir::Procedure *> blocks;
  std::map<std::string, std::vector<Input>> inputs;
  std::set<std::string> prepared;
  for (auto &block : lowered.blocks) blocks.emplace(block.id, &block);
  const auto field = [](const ast::Expr &node, const std::string &name) {
    for (const auto &item : node.string_fields)
      if (item.name == name) return item.value;
    return std::string{};
  };
  const auto walk = [](std::unique_ptr<ast::Expr> &root, const auto &visit) {
    std::function<void(std::unique_ptr<ast::Expr> &)> recurse = [&](auto &node) {
      if (!node) return;
      for (auto &item : node->node_fields) recurse(item.value);
      for (auto &item : node->list_fields)
        for (auto &child : item.values) recurse(child);
      visit(node);
    };
    recurse(root);
  };
  const auto trees = [&](hir::Procedure &procedure, const auto &visit) {
    walk(procedure.signature, visit);
    for (auto &pattern : procedure.param_patterns) walk(pattern, visit);
    walk(procedure.body, visit);
  };
  std::function<void(hir::Procedure &)> prepare = [&](hir::Procedure &procedure) {
    if (!prepared.insert(procedure.id).second) return;
    trees(procedure, [&](std::unique_ptr<ast::Expr> &node) {
      if (node->kind != "HClosure") return;
      const auto found = blocks.find(field(*node, "procedure"));
      if (found == blocks.end()) return;
      auto &block = *found->second;
      prepare(block);
      if (inputs[block.id].empty()) return;
      ast::ListField *captures = nullptr;
      for (auto &list : node->list_fields)
        if (list.name == "captures") captures = &list;
      if (!captures) {
        node->list_field("captures", {});
        captures = &node->list_fields.back();
      }
      std::vector<std::unique_ptr<ast::Expr>> prefix;
      for (const auto &input : inputs[block.id]) {
        std::size_t next = 0;
        for (const auto &local : procedure.locals)
          next = std::max(next, static_cast<std::size_t>(std::stoull(local.slot.substr(1))) + 1U);
        const auto slot = "l" + std::to_string(next);
        procedure.locals.push_back({slot, "__notebook_input_" + input.descriptor,
                                    "temp", "synthetic", input.span});
        auto load = ast::make_expr("HLoadNotebookSlot", input.span);
        load->string_field("descriptor", input.descriptor);
        auto store = ast::make_expr("HStoreLocal", input.span);
        store->string_field("slot", slot);
        store->node_field("expr", std::move(load));
        prefix.push_back(std::move(store));
        auto capture = ast::make_expr("HCapture", input.span);
        capture->string_field("slot", input.capture_slot);
        capture->string_field("source_kind", "local");
        capture->string_field("source_slot", slot);
        captures->values.push_back(std::move(capture));
        for (auto &entry : block.captures) {
          if (entry.slot == input.capture_slot) {
            entry.source_kind = "local"; entry.source_slot = slot;
          }
        }
      }
      node->list_field("capture_initializers", std::move(prefix));
    });
    if (procedure.kind == "notebook_cell") return;
    trees(procedure, [&](std::unique_ptr<ast::Expr> &node) {
      if (node->kind != "HLoadNotebookSlot") return;
      const auto descriptor = field(*node, "descriptor");
      auto &list = inputs[procedure.id];
      auto found = std::find_if(list.begin(), list.end(), [&](const Input &input) {
        return input.descriptor == descriptor;
      });
      if (found == list.end()) {
        const auto slot = "u" + std::to_string(procedure.captures.size());
        const auto name = "__notebook_input_" + descriptor;
        procedure.captures.push_back({slot, name, "local", "", name, node->span});
        list.push_back({descriptor, slot, node->span});
        found = std::prev(list.end());
      }
      const auto span = node->span;
      node = ast::make_expr("HLoadCapture", span);
      node->string_field("slot", found->capture_slot);
    });
  };
  prepare(*lowered.procedure);
}

} // namespace

std::shared_ptr<const bytecode::BcModule> NotebookImage::snapshot() const {
  bytecode::BcModule snapshot = module();
  bytecode::NotebookMetadata metadata;
  metadata.descriptors.reserve(descriptors_.size());
  metadata.cells = cell_entries_;
  for (const NotebookImageDescriptor &descriptor : descriptors_) {
    const auto existing =
        std::find(snapshot.strings.begin(), snapshot.strings.end(),
                  descriptor.name);
    std::size_t name_index = 0;
    if (existing == snapshot.strings.end()) {
      if (snapshot.strings.size() >=
          static_cast<std::size_t>(
              std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error(
            "notebook image string id space exhausted while snapshotting");
      }
      name_index = snapshot.strings.size();
      snapshot.strings.push_back(descriptor.name);
    } else {
      name_index =
          static_cast<std::size_t>(existing - snapshot.strings.begin());
    }
    metadata.descriptors.push_back(bytecode::NotebookDescriptorEntry{
        descriptor.id, descriptor.key.cell_id,
        static_cast<std::uint32_t>(name_index)});
  }
  // NBMD was introduced in bytecode format 1.1. Ordinary compiler output and
  // raw host-adapted NotebookCell modules remain 1.0; self-describing notebook
  // snapshots advertise the extension in the header as well as by section.
  if (snapshot.format_version.major == 1U &&
      snapshot.format_version.minor < 1U) {
    snapshot.format_version.minor = 1U;
  }
  snapshot.file_flags |= bytecode::kFileFlagNotebookOnly;
  snapshot.notebook_metadata = std::move(metadata);
  return std::make_shared<const bytecode::BcModule>(std::move(snapshot));
}

NotebookImage::NotebookImage(std::vector<std::string> string_seed,
                             std::vector<std::string> symbol_seed)
    : emitter_(std::move(string_seed), std::move(symbol_seed)) {}

void NotebookImage::reserve_code_ids(const std::vector<std::uint32_t> &ids) {
  emitter_.reserve_code_ids(ids);
}

NotebookImage::NotebookImage(bytecode::BcModule base_module)
    : emitter_(checked_notebook_base_module(std::move(base_module))) {}

std::optional<std::uint32_t>
NotebookImage::descriptor_for(const BindingKey &key) const {
  const auto found = descriptor_ids_.find(key);
  return found == descriptor_ids_.end()
             ? std::nullopt
             : std::optional<std::uint32_t>(found->second);
}

std::uint32_t NotebookImage::intern_descriptor(const BindingKey &key,
                                               const std::string &name) {
  const auto found = descriptor_ids_.find(key);
  if (found != descriptor_ids_.end()) {
    return found->second;
  }
  if (next_descriptor_id_ == std::numeric_limits<std::uint32_t>::max()) {
    return 0;
  }
  const std::uint32_t id = next_descriptor_id_++;
  descriptor_ids_.emplace(key, id);
  descriptors_.push_back({id, key, name});
  return id;
}

bool NotebookImage::code_id_in_use(std::uint32_t code_id) const {
  return std::any_of(module().code_objects.begin(), module().code_objects.end(),
                     [code_id](const bytecode::BcCode &code) {
                       return code.code_id == code_id;
                     });
}

std::uint32_t NotebookImage::allocate_code_id() {
  while (next_code_id_ != 0 && code_id_in_use(next_code_id_)) {
    if (next_code_id_ == std::numeric_limits<std::uint32_t>::max()) {
      return 0;
    }
    ++next_code_id_;
  }
  if (next_code_id_ == 0) {
    return 0;
  }
  return next_code_id_++;
}

void NotebookImage::discard_code(std::uint32_t code_id) {
  bytecode::BcModule &mutable_module = emitter_.module();
  const auto code = std::find_if(mutable_module.code_objects.rbegin(),
                                 mutable_module.code_objects.rend(),
                                 [code_id](const bytecode::BcCode &value) {
                                   return value.code_id == code_id;
                                 });
  if (code == mutable_module.code_objects.rend()) {
    return;
  }
  mutable_module.code_objects.erase(std::next(code).base());
  mutable_module.line_table.erase(
      std::remove_if(mutable_module.line_table.begin(),
                     mutable_module.line_table.end(),
                     [code_id](const bytecode::LineEntry &entry) {
                       return entry.code_id == code_id;
                     }),
      mutable_module.line_table.end());
  mutable_module.local_debug.erase(
      std::remove_if(mutable_module.local_debug.begin(),
                     mutable_module.local_debug.end(),
                     [code_id](const bytecode::LocalDebugEntry &entry) {
                       return entry.code_id == code_id;
                     }),
      mutable_module.local_debug.end());
}

CellCompileResult compile_cell(const CellSource &cell,
                               const DependencyGraph &graph,
                               NotebookImage &image,
                               const NotebookCompileOptions &options) {
  CellCompileResult result;
  const CellAnalysis analysis = analyze_cell(cell);
  if (!analysis.ok()) {
    result.diagnostics = analysis.diagnostics;
    return result;
  }
  if (cell.id == 0) {
    result.diagnostics.push_back(diagnostic(
        analysis.items.empty() ? lexer::Span{} : analysis.items.front()->span,
        "NB1000", "notebook cell requires a non-zero CellId"));
    return result;
  }
  if (graph.analysis(cell.id) == nullptr) {
    const lexer::Span span =
        analysis.items.empty() ? lexer::Span{} : analysis.items.front()->span;
    result.diagnostics.push_back(
        diagnostic(span, "NB1001", "cell is not present in dependency graph"));
    return result;
  }

  const std::string module_name =
      options.module_name.empty() ? "notebook.cell." + std::to_string(cell.id)
                                  : options.module_name;
  binder::BindResult bind = binder::bind_module(analysis.items, module_name);
  result.diagnostics = bind.diagnostics;
  if (!bind.ok()) {
    return result;
  }

  // Ordinary unresolved-name diagnostics are correct for modules but an
  // unresolved name at one of these exact source occurrences is an explicit
  // notebook input. Missing providers are diagnosed below with the same span.
  for (const lexer::Diagnostic &unresolved :
       binder::unresolved_name_diagnostics(analysis.items, bind.graph)) {
    if (!is_external_reference_span(unresolved.span,
                                    analysis.external_read_refs)) {
      result.diagnostics.push_back(unresolved);
    }
  }
  if (!result.diagnostics.empty()) {
    return result;
  }

  // Descriptor and automatic code-id allocation are part of the cell compile
  // transaction. A rejected edit must not consume stable identities or leave
  // sidecar metadata for code that was never installed in the image.
  std::vector<NotebookImageDescriptor> previous_descriptors =
      image.descriptors_;
  auto previous_descriptor_ids = image.descriptor_ids_;
  const std::uint32_t previous_next_descriptor_id = image.next_descriptor_id_;
  const std::uint32_t previous_next_code_id = image.next_code_id_;
  const std::size_t previous_cell_entry_count = image.cell_entries_.size();
  const auto rollback_identity_allocations = [&]() {
    image.descriptors_.swap(previous_descriptors);
    image.descriptor_ids_.swap(previous_descriptor_ids);
    image.cell_entries_.resize(previous_cell_entry_count);
    image.next_descriptor_id_ = previous_next_descriptor_id;
    image.next_code_id_ = previous_next_code_id;
  };

  std::map<SourceKey, std::uint32_t> external_descriptors;
  std::map<SourceKey, std::string> external_constant_paths;
  std::map<std::string, std::uint32_t> output_descriptors;
  try {
    for (const ExternalReadRef &read : analysis.external_read_refs) {
      const std::optional<BindingKey> provider =
          graph.provider_for(cell.id, read.name);
      if (!provider.has_value()) {
        const auto ambient = options.ambient_constant_paths.find(read.name);
        if (ambient == options.ambient_constant_paths.end()) {
          if (binder::is_native_prelude_name(read.name)) {
            // Native prelude names are runtime constants when no preceding
            // notebook provider shadows them. This also handles names such
            // as system introduced by expansion of an imported string tag.
            external_constant_paths.emplace(source_key(read.span), read.name);
            continue;
          }
          result.diagnostics.push_back(diagnostic(
              read.span, "NB1003",
              "missing notebook input '" + read.name + "'"));
          continue;
        }
        if (!valid_ambient_constant_path(ambient->second)) {
          result.diagnostics.push_back(diagnostic(
              read.span, "NB1006",
              "invalid ambient constant path for '" + read.name + "': '" +
                  ambient->second + "'"));
          continue;
        }
        external_constant_paths.emplace(source_key(read.span), ambient->second);
        continue;
      }
      const std::uint32_t descriptor =
          image.intern_descriptor(*provider, read.name);
      if (descriptor == 0 &&
          image.descriptor_for(*provider).has_value() == false) {
        result.diagnostics.push_back(diagnostic(
            read.span, "NB1004", "notebook descriptor id space exhausted"));
        continue;
      }
      external_descriptors.emplace(source_key(read.span), descriptor);
    }

    for (const std::string &name : analysis.writes) {
      const BindingKey key{cell.id, name};
      const std::uint32_t descriptor = image.intern_descriptor(key, name);
      if (descriptor == 0 && image.descriptor_for(key).has_value() == false) {
        result.diagnostics.push_back(diagnostic(
            analysis.items.empty() ? lexer::Span{}
                                   : analysis.items.front()->span,
            "NB1004", "notebook descriptor id space exhausted"));
        continue;
      }
      output_descriptors.emplace(name, descriptor);
    }
  } catch (...) {
    rollback_identity_allocations();
    throw;
  }
  if (!result.diagnostics.empty()) {
    rollback_identity_allocations();
    return result;
  }

  hir::NotebookLoweringOptions lowering_options;
  lowering_options.external_read_descriptor =
      [&external_descriptors](
          const std::string &,
          const lexer::Span &span) -> std::optional<std::uint32_t> {
    const auto found = external_descriptors.find(source_key(span));
    return found == external_descriptors.end()
               ? std::nullopt
               : std::optional<std::uint32_t>(found->second);
  };
  lowering_options.external_read_constant_path =
      [&external_constant_paths](
          const std::string &, const lexer::Span &span)
      -> std::optional<std::string> {
    const auto found = external_constant_paths.find(source_key(span));
    return found == external_constant_paths.end()
               ? std::nullopt
               : std::optional<std::string>(found->second);
  };
  lowering_options.declared_write_descriptor =
      [&output_descriptors](
          const std::string &name) -> std::optional<std::uint32_t> {
    const auto found = output_descriptors.find(name);
    return found == output_descriptors.end()
               ? std::nullopt
               : std::optional<std::uint32_t>(found->second);
  };

  hir::NotebookLoweringResult lowered;
  try {
    lowered = hir::lower_notebook_cell(analysis.items, module_name, bind.graph,
                                       lowering_options);
  } catch (...) {
    rollback_identity_allocations();
    throw;
  }
  result.diagnostics = std::move(lowered.diagnostics);
  if (!lowered.ok()) {
    rollback_identity_allocations();
    return result;
  }
  capture_notebook_inputs(lowered);

  const std::uint32_t code_id =
      options.code_id.has_value() ? *options.code_id : image.allocate_code_id();
  if (code_id == 0 || image.code_id_in_use(code_id)) {
    const lexer::Span span =
        analysis.items.empty() ? lexer::Span{} : analysis.items.front()->span;
    result.diagnostics.push_back(
        diagnostic(span, "NB1005", "duplicate or invalid notebook code_id"));
    rollback_identity_allocations();
    return result;
  }

  // Prepare every allocation owned by the compiler sidecar before committing
  // bytecode to the emitter. Once append_cell succeeds, the remaining moves
  // into CellCompileResult are non-allocating and the image is fully usable by
  // snapshot() even if the caller immediately discards the compile result.
  std::vector<CellSlotUse> prepared_outputs;
  std::vector<CellSlotUse> prepared_inputs;
  bytecode::NotebookCellEntry cell_entry;
  try {
    prepared_outputs.reserve(output_descriptors.size());
    cell_entry.output_descriptor_ids.reserve(output_descriptors.size());
    for (const auto &entry : output_descriptors) {
      const std::uint32_t descriptor = entry.second;
      const auto found =
          std::find_if(image.descriptors_.begin(), image.descriptors_.end(),
                       [descriptor](const NotebookImageDescriptor &value) {
                         return value.id == descriptor;
                       });
      if (found != image.descriptors_.end()) {
        prepared_outputs.push_back(
            {*found, NotebookCellSlotDirection::Output});
        cell_entry.output_descriptor_ids.push_back(descriptor);
      }
    }

    std::set<std::uint32_t> input_descriptors;
    std::vector<std::uint32_t> ordered_input_descriptors;
    ordered_input_descriptors.reserve(external_descriptors.size());
    for (const auto &entry : external_descriptors) {
      if (input_descriptors.insert(entry.second).second) {
        ordered_input_descriptors.push_back(entry.second);
      }
    }
    prepared_inputs.reserve(ordered_input_descriptors.size());
    cell_entry.input_descriptor_ids.reserve(ordered_input_descriptors.size());
    for (const std::uint32_t descriptor : ordered_input_descriptors) {
      const auto found =
          std::find_if(image.descriptors_.begin(), image.descriptors_.end(),
                       [descriptor](const NotebookImageDescriptor &value) {
                         return value.id == descriptor;
                       });
      if (found != image.descriptors_.end()) {
        prepared_inputs.push_back({*found, NotebookCellSlotDirection::Input});
        cell_entry.input_descriptor_ids.push_back(descriptor);
      }
    }
    cell_entry.code_id = code_id;
    cell_entry.cell_id = cell.id;
    image.cell_entries_.push_back(std::move(cell_entry));
  } catch (...) {
    rollback_identity_allocations();
    throw;
  }

  bytecode::NotebookEmitResult emitted;
  try {
    emitted =
        image.emitter_.append_cell(*lowered.procedure, code_id, module_name, lowered.blocks);
  } catch (...) {
    // NotebookEmitter has already restored its complete checkpoint. Keep the
    // descriptor/code-id sidecar in the same transaction before propagating
    // allocation or unexpected backend failures to the caller.
    rollback_identity_allocations();
    throw;
  }
  result.diagnostics = std::move(emitted.diagnostics);
  if (!result.diagnostics.empty()) {
    image.discard_code(code_id);
    rollback_identity_allocations();
    return result;
  }

  result.ok = true;
  result.code_id = code_id;
  result.code_index = emitted.code_index;
  result.outputs = std::move(prepared_outputs);
  result.inputs = std::move(prepared_inputs);
  return result;
}

} // namespace amber::notebook
