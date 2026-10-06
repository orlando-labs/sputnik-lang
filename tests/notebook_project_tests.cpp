#include "notebook/project.h"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>

namespace {

using amber::notebook::CellMode;
using amber::notebook::ProjectCell;
using amber::notebook::ProjectDocument;
using amber::notebook::ProjectExtraFields;
using amber::notebook::ProjectModule;
using amber::notebook::ProjectSheet;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "notebook project test failed: " << message << "\n";
    std::exit(1);
  }
}

template <typename Function>
void expect_throws(Function &&function, const std::string &message) {
  bool threw = false;
  try {
    function();
  } catch (const std::exception &) {
    threw = true;
  } catch (...) {
    threw = true;
  }
  expect(threw, message);
}

struct TempDirectory {
  TempDirectory() {
    char pattern[] = "/tmp/amber_notebook_project_XXXXXX";
    char *created = mkdtemp(pattern);
    expect(created != nullptr, "mkdtemp should create an isolated directory");
    path = created;
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  std::filesystem::path path;
};

void write_text(const std::filesystem::path &path, const std::string &text) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  expect(!error, "fixture parent directories should be creatable");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  expect(output.good(), "fixture file should be writable");
  output << text;
  expect(output.good(), "fixture file should be written");
}

std::string read_text(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  expect(input.good(), "fixture file should be readable");
  std::ostringstream contents;
  contents << input.rdbuf();
  expect(input.good() || input.eof(), "fixture file should be read");
  return contents.str();
}

ProjectDocument valid_document() {
  ProjectDocument document;
  document.title = "Demo notebook";
  document.active_sheet = "main";

  ProjectSheet sheet;
  sheet.id = "main";
  sheet.title = "Main";

  ProjectCell first;
  first.id = 1;
  first.kind = "code";
  first.source = "message = \"Привет 🌙\\n\"\n";
  first.mode = CellMode::Watch;
  sheet.cells.push_back(first);

  ProjectCell second;
  second.id = 2;
  second.kind = "code";
  second.source = "answer = 42\n";
  second.mode = CellMode::Manual;
  sheet.cells.push_back(second);

  document.sheets.push_back(std::move(sheet));
  return document;
}

void expect_extra_fields(const ProjectExtraFields &actual,
                         const ProjectExtraFields &expected,
                         const std::string &message) {
  expect(actual == expected, message);
}

void expect_documents_equal(const ProjectDocument &actual,
                            const ProjectDocument &expected,
                            const std::string &message) {
  expect(actual.title == expected.title, message + ": title");
  expect(actual.active_sheet == expected.active_sheet,
         message + ": active sheet");
  expect(actual.auto_imports == expected.auto_imports,
         message + ": auto imports");
  expect_extra_fields(actual.extra, expected.extra,
                      message + ": document extra");
  expect(actual.sheets.size() == expected.sheets.size(), message + ": sheets");
  for (std::size_t sheet_index = 0; sheet_index < expected.sheets.size();
       ++sheet_index) {
    const ProjectSheet &left = actual.sheets[sheet_index];
    const ProjectSheet &right = expected.sheets[sheet_index];
    expect(left.id == right.id, message + ": sheet id");
    expect(left.title == right.title, message + ": sheet title");
    expect_extra_fields(left.extra, right.extra, message + ": sheet extra");
    expect(left.cells.size() == right.cells.size(), message + ": cells");
    for (std::size_t cell_index = 0; cell_index < right.cells.size();
         ++cell_index) {
      const ProjectCell &left_cell = left.cells[cell_index];
      const ProjectCell &right_cell = right.cells[cell_index];
      expect(left_cell.id == right_cell.id, message + ": cell id");
      expect(left_cell.kind == right_cell.kind, message + ": cell kind");
      expect(left_cell.source == right_cell.source, message + ": cell source");
      expect(left_cell.formatting == right_cell.formatting,
             message + ": cell formatting");
      expect(left_cell.mode == right_cell.mode, message + ": cell mode");
      expect_extra_fields(left_cell.extra, right_cell.extra,
                          message + ": cell extra");
    }
  }
  expect(actual.modules.size() == expected.modules.size(),
         message + ": modules");
  for (std::size_t module_index = 0; module_index < expected.modules.size();
       ++module_index) {
    const ProjectModule &left = actual.modules[module_index];
    const ProjectModule &right = expected.modules[module_index];
    expect(left.id == right.id, message + ": module id");
    expect(left.path == right.path, message + ": module path");
    expect_extra_fields(left.extra, right.extra, message + ": module extra");
  }
}

void test_document_roundtrip_preserves_semantics_and_raw_metadata() {
  ProjectDocument original = valid_document();
  original.extra["future"] = "{\"counter\":1.2300e+400,\"nested\":[true,null]}";
  original.sheets[0].extra["vendor"] =
      "[\"native-ui\",{\"precision\":9007199254740993}]";
  original.sheets[0].cells[0].extra["metadata"] =
      "{\"nested\":{\"id\":18446744073709551613}}";
  original.sheets[0].cells[1].extra["ui"] = "{\"collapsed\":false}";
  ProjectCell markdown;
  markdown.id = std::numeric_limits<std::uint64_t>::max() - 1U;
  markdown.kind = "markdown";
  markdown.extra["source"] = "\"# Unicode \u00e9\"";
  markdown.extra["mode"] = "\"manual\"";
  markdown.extra["metadata"] = "{\"raw\":1.0000000000000001}";
  original.sheets[0].cells.push_back(std::move(markdown));
  original.modules.push_back(
      ProjectModule{"core", "lib/core.am", {{"metadata", "{\"abi\":1}"}}});
  original.auto_imports.push_back("core");

  const std::string serialized =
      amber::notebook::serialize_project_document(original);
  const ProjectDocument parsed =
      amber::notebook::parse_project_document(serialized);
  expect_documents_equal(parsed, original, "semantic project roundtrip");
  expect(parsed.sheets[0].cells[2].source.empty(),
         "non-code cell should not interpret source as code data");
  expect(parsed.sheets[0].cells[2].mode == CellMode::Watch,
         "non-code cell should retain its default mode");
  expect(parsed.sheets[0].cells[2].extra.at("source") == "\"# Unicode é\"",
         "non-code source metadata should remain raw extra JSON");
  expect(parsed.extra.at("future").find("1.2300e+400") != std::string::npos,
         "unknown metadata should preserve exact number spelling");

  const ProjectDocument reparsed = amber::notebook::parse_project_document(
      amber::notebook::serialize_project_document(parsed));
  expect_documents_equal(reparsed, original,
                         "second semantic project roundtrip");
}

void test_json_unicode_escapes_and_nested_raw_values() {
  const std::string json = R"JSON({
    "format":"amber-notebook",
    "version":1,
    "title":"Unicode \u041f\u0440\u0438\u0432\u0435\u0442 \u00e9\nquote\" slash\\",
    "active_sheet":"main",
    "sheets":[{
      "id":"main",
      "title":"Основной",
      "cells":[
        {"id":"1","kind":"code","source":"x = \"é\"\n","mode":"manual","future":{"exact":1.2300e+400}},
        {"id":"2","kind":"markdown","source":"# text","mode":"manual","future":[{"n":9007199254740993}]}
      ],
      "ui":{"zoom":1.0000}
    }],
    "modules":[{"id":"core","path":"lib/core.am","future":{"version":1.0000}}],
    "auto_imports":["core"],
    "future":{"large":18446744073709551613}
  })JSON";

  const ProjectDocument parsed = amber::notebook::parse_project_document(json);
  expect(parsed.title == "Unicode Привет é\nquote\" slash\\",
         "escaped Unicode title should decode correctly");
  expect(parsed.sheets[0].cells[0].source == "x = \"é\"\n",
         "escaped source should decode correctly");
  expect(parsed.sheets[0].cells[0].mode == CellMode::Manual,
         "code mode should decode manual");
  expect(parsed.sheets[0].cells[0].extra.at("future") ==
             "{\"exact\":1.2300e+400}",
         "code unknown metadata should retain nested raw JSON");
  expect(parsed.sheets[0].cells[1].source.empty() &&
             parsed.sheets[0].cells[1].mode == CellMode::Watch,
         "non-code cell should interpret only id and kind");
  expect(parsed.sheets[0].cells[1].extra.at("source") == "\"# text\"" &&
             parsed.sheets[0].cells[1].extra.at("mode") == "\"manual\"",
         "non-code fields should be retained as raw extras");
  expect(parsed.sheets[0].extra.at("ui") == "{\"zoom\":1.0000}",
         "sheet metadata should retain exact numeric spelling");
  expect(parsed.modules[0].extra.at("future") == "{\"version\":1.0000}",
         "module metadata should retain exact numeric spelling");
  expect(parsed.extra.at("future") == "{\"large\":18446744073709551613}",
         "document metadata should retain large integer spelling");
}

void test_document_id_boundaries_and_validation() {
  ProjectDocument document = valid_document();
  document.sheets[0].cells[0].id =
      std::numeric_limits<std::uint64_t>::max() - 1U;
  amber::notebook::validate_project_document(document);

  auto invalid = [&](ProjectDocument candidate, const std::string &message) {
    expect_throws(
        [&] { amber::notebook::validate_project_document(candidate); },
        message);
  };

  ProjectDocument zero = document;
  zero.sheets[0].cells[1].id = 0;
  invalid(std::move(zero), "zero cell id should be rejected");

  ProjectDocument maximum = document;
  maximum.sheets[0].cells[1].id = std::numeric_limits<std::uint64_t>::max();
  invalid(std::move(maximum), "UINT64_MAX cell id should be rejected");

  ProjectDocument duplicate = document;
  duplicate.sheets.push_back(duplicate.sheets[0]);
  duplicate.sheets.back().id = "other";
  invalid(std::move(duplicate),
          "duplicate cell ids across sheets should reject");

  ProjectDocument no_sheets = document;
  no_sheets.sheets.clear();
  invalid(std::move(no_sheets), "a project requires at least one sheet");

  ProjectDocument missing_active = document;
  missing_active.active_sheet.clear();
  invalid(std::move(missing_active), "active sheet is required");

  ProjectDocument unknown_active = document;
  unknown_active.active_sheet = "missing";
  invalid(std::move(unknown_active), "active sheet must name a sheet");

  ProjectDocument bad_sheet = document;
  bad_sheet.sheets[0].id.clear();
  invalid(std::move(bad_sheet), "sheet id is required");

  ProjectDocument bad_mode = document;
  bad_mode.sheets[0].cells[0].mode = static_cast<CellMode>(99);
  invalid(std::move(bad_mode), "unknown code mode should be rejected");
}

void test_module_and_auto_import_validation() {
  auto invalid_module = [](const std::string &id, const std::string &path,
                           const std::string &message) {
    ProjectDocument document = valid_document();
    document.modules.push_back(ProjectModule{id, path, {}});
    expect_throws([&] { amber::notebook::validate_project_document(document); },
                  message);
  };

  invalid_module("", "lib/core.am", "empty module id should be rejected");
  invalid_module("1core", "lib/core.am", "module id must be an identifier");
  invalid_module("bad/id", "lib/core.am",
                 "module id punctuation should be rejected");
  invalid_module("core", "", "empty module path should be rejected");
  invalid_module("core", "/tmp/core.am",
                 "absolute module path should be rejected");
  invalid_module("core", "../core.am", "parent module path should be rejected");
  invalid_module("core", "lib/../core.am",
                 "embedded parent module path should be rejected");
  invalid_module("core", "./core.am", "dot module path should be rejected");
  invalid_module("core", "lib\\core.am",
                 "backslash module path should be rejected");
  invalid_module("core", "lib/core.txt",
                 "non-Amber module path should be rejected");

  ProjectDocument duplicate = valid_document();
  duplicate.modules.push_back(ProjectModule{"core", "lib/a.am", {}});
  duplicate.modules.push_back(ProjectModule{"core", "lib/b.am", {}});
  expect_throws([&] { amber::notebook::validate_project_document(duplicate); },
                "duplicate module ids should be rejected");

  ProjectDocument unknown_import = valid_document();
  unknown_import.modules.push_back(ProjectModule{"core", "lib/core.am", {}});
  unknown_import.auto_imports = {"missing"};
  expect_throws(
      [&] { amber::notebook::validate_project_document(unknown_import); },
      "auto import must name a known module");

  ProjectDocument duplicate_import = valid_document();
  duplicate_import.modules.push_back(ProjectModule{"core", "lib/core.am", {}});
  duplicate_import.auto_imports = {"core", "core"};
  expect_throws(
      [&] { amber::notebook::validate_project_document(duplicate_import); },
      "duplicate auto imports should be rejected");
}

void test_malformed_json_and_schema_errors() {
  const std::string valid_json =
      amber::notebook::serialize_project_document(valid_document());
  expect_throws(
      [&] {
        amber::notebook::parse_project_document(
            "{\"format\":\"amber-notebook\",\"format\":\"other\"}");
      },
      "duplicate JSON keys should be rejected");

  std::string invalid_utf8 =
      "{\"format\":\"amber-notebook\",\"version\":1,\"title\":\"";
  invalid_utf8.push_back(static_cast<char>(0xc3));
  invalid_utf8.push_back(static_cast<char>(0x28));
  invalid_utf8 +=
      "\",\"active_sheet\":\"main\",\"sheets\":[{\"id\":\"main\",\"title\":"
      "\"Main\",\"cells\":[]}],\"modules\":[],\"auto_imports\":[]}";
  expect_throws([&] { amber::notebook::parse_project_document(invalid_utf8); },
                "invalid UTF-8 should be rejected");

  std::string valid_raw_controls =
      "{\"format\":\"amber-notebook\",\"version\":1,\"title\":\"";
  valid_raw_controls.push_back(static_cast<char>(0x7f));
  valid_raw_controls.push_back(static_cast<char>(0xc2));
  valid_raw_controls.push_back(static_cast<char>(0x85));
  valid_raw_controls +=
      "\",\"active_sheet\":\"main\",\"sheets\":[{\"id\":\"main\",\"title\":"
      "\"Main\",\"cells\":[]}],\"modules\":[],\"auto_imports\":[]}";
  const ProjectDocument raw_controls =
      amber::notebook::parse_project_document(valid_raw_controls);
  expect(raw_controls.title.size() == 3U &&
             static_cast<unsigned char>(raw_controls.title[0]) == 0x7fU &&
             static_cast<unsigned char>(raw_controls.title[1]) == 0xc2U &&
             static_cast<unsigned char>(raw_controls.title[2]) == 0x85U,
         "raw DEL and C1 Unicode must be accepted by JSON strings");

  std::string too_deep =
      "{\"format\":\"amber-notebook\",\"version\":1,\"title\":\"deep\","
      "\"active_sheet\":\"main\",\"sheets\":[{\"id\":\"main\",\"title\":"
      "\"Main\",\"cells\":[]}],\"modules\":[],\"auto_imports\":[],\"future\":";
  for (int depth = 0; depth < 1024; ++depth) {
    too_deep += "[";
  }
  too_deep += "0";
  for (int depth = 0; depth < 1024; ++depth) {
    too_deep += "]";
  }
  too_deep += "}";
  expect_throws([&] { amber::notebook::parse_project_document(too_deep); },
                "excessive JSON nesting should be rejected");

  expect_throws([&] { amber::notebook::parse_project_document("{}"); },
                "missing schema members should be rejected");
  expect_throws(
      [&] {
        amber::notebook::parse_project_document(
            "{\"format\":\"wrong\",\"version\":1,\"title\":\"x\",\"active_"
            "sheet\":\"main\",\"sheets\":[],\"modules\":[],\"auto_imports\":[]"
            "}");
      },
      "wrong schema format should be rejected");
  expect_throws(
      [&] {
        amber::notebook::parse_project_document(
            "{\"format\":\"amber-notebook\",\"version\":2,\"title\":\"x\","
            "\"active_sheet\":\"main\",\"sheets\":[],\"modules\":[],\"auto_"
            "imports\":[]}");
      },
      "unsupported schema version should be rejected");

  std::string bad_id = valid_json;
  std::size_t id_key_position = valid_json.find("\"id\"");
  std::size_t id_value_position = std::string::npos;
  std::size_t id_value_end = std::string::npos;
  while (id_key_position != std::string::npos) {
    const std::size_t colon = valid_json.find(':', id_key_position + 4U);
    const std::size_t opening_quote = colon == std::string::npos
                                          ? std::string::npos
                                          : valid_json.find('"', colon + 1U);
    const std::size_t closing_quote =
        opening_quote == std::string::npos
            ? std::string::npos
            : valid_json.find('"', opening_quote + 1U);
    if (closing_quote != std::string::npos &&
        valid_json.substr(opening_quote + 1U,
                          closing_quote - opening_quote - 1U) == "1") {
      id_value_position = opening_quote;
      id_value_end = closing_quote;
      break;
    }
    id_key_position = valid_json.find("\"id\"", id_key_position + 4U);
  }
  expect(id_value_position != std::string::npos,
         "valid fixture should contain a numeric cell id");
  const auto replace_cell_id = [&](const std::string &id) {
    std::string result = valid_json;
    result.replace(id_value_position, id_value_end - id_value_position + 1U,
                   "\"" + id + "\"");
    return result;
  };
  bad_id = replace_cell_id("01");
  expect_throws([&] { amber::notebook::parse_project_document(bad_id); },
                "noncanonical decimal ids should be rejected");

  std::string max_id = replace_cell_id("18446744073709551615");
  expect_throws([&] { amber::notebook::parse_project_document(max_id); },
                "UINT64_MAX JSON id should be rejected");

  std::string overflowing_id = replace_cell_id("27670116110564327425");
  expect_throws(
      [&] { amber::notebook::parse_project_document(overflowing_id); },
      "a wrapping UINT64 decimal JSON id should be rejected");

  std::string max_plus_one = replace_cell_id("18446744073709551616");
  expect_throws([&] { amber::notebook::parse_project_document(max_plus_one); },
                "UINT64_MAX plus one JSON id should be rejected");

  std::string nested_array = "[";
  for (int depth = 1; depth < 60; ++depth) {
    nested_array += "[";
  }
  nested_array += "0";
  for (int depth = 1; depth < 60; ++depth) {
    nested_array += "]";
  }
  nested_array += "]";
  ProjectDocument standalone_extra = valid_document();
  standalone_extra.extra["deep"] = nested_array;
  amber::notebook::validate_project_document(standalone_extra);

  ProjectDocument embedded_extra = valid_document();
  embedded_extra.sheets[0].cells[0].extra["deep"] = nested_array;
  std::string emitted;
  bool serialized = true;
  try {
    emitted = amber::notebook::serialize_project_document(embedded_extra);
  } catch (const std::exception &) {
    serialized = false;
  }
  if (serialized) {
    bool reparsed = false;
    try {
      (void)amber::notebook::parse_project_document(emitted);
      reparsed = true;
    } catch (const std::exception &) {
    }
    expect(reparsed,
           "a successful deep-extra serialization must remain loadable");
  }
}

void test_filesystem_create_save_and_reopen() {
  TempDirectory temporary;
  const std::filesystem::path directory = temporary.path / "demo";
  ProjectDocument document = valid_document();
  const amber::notebook::LoadedProject created =
      amber::notebook::create_project(directory, document);
  expect(std::filesystem::exists(directory / "project.json"),
         "create should write project.json");

  const std::filesystem::path module_path = directory / "lib" / "core.am";
  const std::string module_source =
      "# ordinary module source\nvalue = \"не выполнять\"\n";
  write_text(module_path, module_source);
  document.modules.push_back(ProjectModule{"core", "lib/core.am", {}});
  document.auto_imports.push_back("core");

  amber::notebook::LoadedProject mutable_project = created;
  amber::notebook::save_project(&mutable_project, document);
  expect(read_text(module_path) == module_source,
         "save should never rewrite module source files");

  const amber::notebook::LoadedProject reopened =
      amber::notebook::load_project(directory);
  expect_documents_equal(reopened.document, document,
                         "reopened project document");
  expect(reopened.baseline == read_text(directory / "project.json"),
         "reopened baseline should match exact project JSON bytes");
}

void test_create_refuses_existing_targets() {
  TempDirectory temporary;
  const std::filesystem::path existing = temporary.path / "existing";
  std::filesystem::create_directories(existing);
  write_text(existing / "keep.txt", "do not overwrite");
  expect_throws(
      [&] { amber::notebook::create_project(existing, valid_document()); },
      "create should refuse an existing directory");
  expect(read_text(existing / "keep.txt") == "do not overwrite",
         "existing target should remain untouched");
}

void test_external_modification_conflict_preserves_disk() {
  TempDirectory temporary;
  const std::filesystem::path directory = temporary.path / "conflict";
  amber::notebook::LoadedProject owner =
      amber::notebook::create_project(directory, valid_document());
  const std::string original_disk = read_text(directory / "project.json");

  ProjectDocument external = owner.document;
  external.title = "external writer";
  const std::string external_disk =
      amber::notebook::serialize_project_document(external);
  write_text(directory / "project.json", external_disk);

  ProjectDocument local = owner.document;
  local.title = "local writer";
  expect_throws([&] { amber::notebook::save_project(&owner, local); },
                "save should reject an external project.json modification");
  expect(read_text(directory / "project.json") == external_disk,
         "external project.json bytes should survive conflict");
  expect(external_disk != original_disk,
         "conflict fixture should actually modify project.json");
}

void test_traversal_and_symlinked_members_are_rejected() {
  TempDirectory temporary;
  const std::filesystem::path directory = temporary.path / "secure";
  amber::notebook::LoadedProject project =
      amber::notebook::create_project(directory, valid_document());
  const std::string baseline = read_text(directory / "project.json");

  ProjectDocument traversal = project.document;
  traversal.modules.push_back(ProjectModule{"escape", "../outside.am", {}});
  expect_throws([&] { amber::notebook::save_project(&project, traversal); },
                "save should reject module traversal outside project");
  expect(read_text(directory / "project.json") == baseline,
         "rejected traversal save should not change project.json");

  const std::filesystem::path outside_module = temporary.path / "outside.am";
  write_text(outside_module, "raise \"outside\"\n");
  const std::filesystem::path linked_module = directory / "lib" / "link.am";
  std::error_code directory_error;
  std::filesystem::create_directories(linked_module.parent_path(),
                                      directory_error);
  expect(!directory_error, "module symlink parent should be creatable");
  std::error_code symlink_error;
  std::filesystem::create_symlink(outside_module, linked_module, symlink_error);
  expect(!symlink_error, "module symlink fixture should be creatable");
  ProjectDocument linked_document = project.document;
  linked_document.modules.push_back(ProjectModule{"linked", "lib/link.am", {}});
  write_text(directory / "project.json",
             amber::notebook::serialize_project_document(linked_document));
  expect_throws([&] { amber::notebook::load_project(directory); },
                "load should reject a symlinked module member");

  const std::filesystem::path outside_json = temporary.path / "outside.json";
  write_text(outside_json,
             amber::notebook::serialize_project_document(project.document));
  std::error_code remove_error;
  std::filesystem::remove(directory / "project.json", remove_error);
  expect(!remove_error, "project fixture should be removable");
  symlink_error.clear();
  std::filesystem::create_symlink(outside_json, directory / "project.json",
                                  symlink_error);
  expect(!symlink_error, "project.json symlink fixture should be creatable");
  expect_throws([&] { amber::notebook::load_project(directory); },
                "load should reject a symlinked project.json member");
}

void test_load_has_no_execution_effects_and_save_preserves_modules() {
  TempDirectory temporary;
  const std::filesystem::path directory = temporary.path / "no_execute";
  amber::notebook::LoadedProject project =
      amber::notebook::create_project(directory, valid_document());
  const std::filesystem::path marker = temporary.path / "executed.marker";
  const std::string hostile_source =
      "io.File.open(\"" + marker.string() + "\", :write).write(\"executed\")\n";
  const std::filesystem::path module_path = directory / "lib" / "hostile.am";
  write_text(module_path, hostile_source);

  ProjectDocument document = project.document;
  document.modules.push_back(ProjectModule{"hostile", "lib/hostile.am", {}});
  const std::string before_save = read_text(module_path);
  amber::notebook::save_project(&project, document);
  const amber::notebook::LoadedProject loaded =
      amber::notebook::load_project(directory);
  const amber::notebook::LoadedProjectModule loaded_module =
      amber::notebook::load_project_module(loaded, "hostile");
  expect(!std::filesystem::exists(marker),
         "loading a module manifest must not execute module source");
  expect(read_text(module_path) == before_save,
         "load and save must preserve module source bytes");
  expect(loaded_module.source == hostile_source &&
             loaded_module.baseline == hostile_source,
         "module load should return exact source bytes without execution");
  expect(loaded.document.modules.size() == 1U &&
             loaded.document.modules[0].id == "hostile",
         "module manifest should load without execution");
}

amber::notebook::LoadedProject
make_module_project(const std::filesystem::path &directory,
                    const std::string &id, const std::string &path,
                    const std::string &source) {
  amber::notebook::LoadedProject project =
      amber::notebook::create_project(directory, valid_document());
  write_text(directory / std::filesystem::path(path), source);
  ProjectDocument document = project.document;
  document.modules.push_back(ProjectModule{id, path, {}});
  amber::notebook::save_project(&project, document);
  return project;
}

void test_module_source_persistence_conflicts_and_independent_baselines() {
  TempDirectory temporary;
  const std::filesystem::path directory = temporary.path / "modules";
  const std::string original_source = "export value\nvalue = 7\n";
  amber::notebook::LoadedProject project = make_module_project(
      directory, "core", "nested/deep/core.am", original_source);
  amber::notebook::LoadedProjectModule module =
      amber::notebook::load_project_module(project, "core");
  expect(module.id == "core" && module.path == "nested/deep/core.am" &&
             module.source == original_source &&
             module.baseline == original_source,
         "module load should expose id, path, source, and source baseline");

  const std::string project_baseline = project.baseline;
  ProjectDocument changed_document = project.document;
  changed_document.title = "document-only edit";
  amber::notebook::save_project(&project, changed_document);
  expect(read_text(directory / "nested/deep/core.am") == original_source,
         "project save must not touch module source bytes");
  expect(project.baseline != project_baseline,
         "project save should advance only the project baseline");
  expect(module.baseline == original_source,
         "project save must not advance a separately loaded module baseline");

  const std::string incomplete_source =
      std::string("prefix\0", 7) + std::string(1, static_cast<char>(0xc3)) +
      std::string(1, static_cast<char>(0xff));
  amber::notebook::save_project_module(project, &module, incomplete_source);
  expect(read_text(directory / "nested/deep/core.am") == incomplete_source,
         "module save should preserve incomplete arbitrary source bytes");
  expect(module.source == incomplete_source &&
             module.baseline == incomplete_source,
         "module save should advance its own source baseline");
  expect(project.baseline != project_baseline &&
             read_text(directory / "project.json") == project.baseline,
         "module save must not alter the project baseline or project JSON");

  for (const auto &entry :
       std::filesystem::directory_iterator(directory / "nested/deep")) {
    expect(entry.path().filename().string().find(".iamber.tmp-") != 0U,
           "atomic module save should clean up its staging file");
  }

  const std::string external_source = "external module edit\n";
  write_text(directory / "nested/deep/core.am", external_source);
  const std::string saved_source = "local module edit\n";
  const std::string module_baseline_before_conflict = module.baseline;
  const std::string module_source_before_conflict = module.source;
  expect_throws(
      [&] {
        amber::notebook::save_project_module(project, &module, saved_source);
      },
      "external module edit should reject a stale module save");
  expect(read_text(directory / "nested/deep/core.am") == external_source,
         "module conflict must preserve external disk bytes");
  expect(module.baseline == module_baseline_before_conflict &&
             module.source == module_source_before_conflict,
         "module conflict must preserve the in-memory source baseline");
}

void test_module_project_mapping_and_project_conflicts() {
  TempDirectory temporary;
  const std::filesystem::path directory = temporary.path / "mapping";
  amber::notebook::LoadedProject project =
      make_module_project(directory, "core", "lib/core.am", "module bytes\n");
  amber::notebook::LoadedProjectModule module =
      amber::notebook::load_project_module(project, "core");
  const std::string module_disk = read_text(directory / "lib/core.am");

  write_text(directory / "lib/rogue.am", "rogue bytes\n");
  project.document.modules.push_back(
      ProjectModule{"rogue", "lib/rogue.am", {}});
  expect_throws(
      [&] { (void)amber::notebook::load_project_module(project, "rogue"); },
      "a caller-mutated manifest must not authorize loading another member");
  amber::notebook::LoadedProjectModule forged{"rogue", "lib/rogue.am",
                                              "rogue bytes\n", "rogue bytes\n"};
  expect_throws(
      [&] {
        amber::notebook::save_project_module(project, &forged,
                                             "should not publish\n");
      },
      "a caller-mutated manifest must not authorize saving another member");
  expect(read_text(directory / "lib/rogue.am") == "rogue bytes\n",
         "rejected mutable-manifest access must preserve the unlisted file");
  project.document = amber::notebook::parse_project_document(project.baseline);

  project.document.modules[0].path = "lib/renamed.am";
  expect_throws(
      [&] {
        amber::notebook::save_project_module(project, &module,
                                             "should not publish");
      },
      "changed in-memory module path should reject a stale module save");
  expect(read_text(directory / "lib/core.am") == module_disk &&
             module.source == module_disk && module.baseline == module_disk,
         "mapping conflict must preserve module disk and baseline");

  amber::notebook::LoadedProject fresh =
      amber::notebook::load_project(directory);
  amber::notebook::LoadedProjectModule fresh_module =
      amber::notebook::load_project_module(fresh, "core");
  ProjectDocument externally_changed = fresh.document;
  externally_changed.title = "external project edit";
  write_text(directory / "project.json",
             amber::notebook::serialize_project_document(externally_changed));
  const std::string project_disk = read_text(directory / "project.json");
  expect_throws([&] { amber::notebook::load_project_module(fresh, "core"); },
                "external project.json edit should block module load");
  expect_throws(
      [&] {
        amber::notebook::save_project_module(fresh, &fresh_module,
                                             "blocked module edit\n");
      },
      "external project.json edit should block module save");
  expect(read_text(directory / "project.json") == project_disk &&
             read_text(directory / "lib/core.am") == module_disk &&
             fresh_module.source == module_disk &&
             fresh_module.baseline == module_disk,
         "project conflict must preserve project, module, and baselines");
}

void test_module_symlink_components_and_nonblocking_project_lock() {
  TempDirectory temporary;
  const std::filesystem::path directory = temporary.path / "safety";
  const std::string original_source = "safe module\n";
  amber::notebook::LoadedProject project = make_module_project(
      directory, "core", "nested/link/core.am", original_source);
  amber::notebook::LoadedProjectModule module =
      amber::notebook::load_project_module(project, "core");

  const std::filesystem::path outside = temporary.path / "outside" / "child";
  write_text(outside / "core.am", "outside module\n");
  const std::filesystem::path linked_directory = directory / "nested/link";
  std::error_code remove_error;
  std::filesystem::remove_all(linked_directory, remove_error);
  expect(!remove_error, "symlink component fixture should be removable");
  std::error_code symlink_error;
  std::filesystem::create_symlink(outside, linked_directory, symlink_error);
  expect(!symlink_error, "nested module symlink fixture should be creatable");
  expect_throws([&] { amber::notebook::load_project_module(project, "core"); },
                "module load should reject a symlinked path component");
  expect_throws(
      [&] {
        amber::notebook::save_project_module(project, &module,
                                             "should not follow symlink\n");
      },
      "module save should reject a symlinked path component");
  expect(read_text(outside / "core.am") == "outside module\n",
         "symlink rejection must preserve the outside module");

  const int lock_fd =
      ::open((directory / ".iamber-project.lock").c_str(), O_RDWR | O_CLOEXEC);
  expect(lock_fd >= 0, "project lock should be openable for contention test");
  expect(::flock(lock_fd, LOCK_EX | LOCK_NB) == 0,
         "test should acquire the project lock first");
  const auto start = std::chrono::steady_clock::now();
  expect_throws(
      [&] {
        amber::notebook::save_project_module(project, &module,
                                             "blocked by lock\n");
      },
      "module save should fail instead of waiting behind another writer");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  expect(elapsed < std::chrono::seconds(1),
         "project lock contention should remain bounded and nonblocking");
  expect(::flock(lock_fd, LOCK_UN) == 0,
         "test should release the project lock");
  expect(::close(lock_fd) == 0, "test should close the project lock");
}

void test_text_content_roundtrip_and_strict_formatting() {
  const std::string payload =
      R"JSON({"source":"🙂hello","formatting":{"version":1,"runs":[{"start":0,"length":2,"bold":true},{"start":2,"length":5,"style":"heading1"}]}})JSON";
  const auto content = amber::notebook::parse_project_text_content(payload);
  expect(content.source == "🙂hello" && !content.formatting.empty(),
         "text content should decode source and retain formatting JSON");
  const auto serialized =
      amber::notebook::serialize_project_text_content(content);
  const auto reparsed =
      amber::notebook::parse_project_text_content(serialized);
  expect(reparsed.source == content.source &&
             reparsed.formatting == content.formatting,
         "text content formatting should round-trip without loss");

  ProjectDocument document = valid_document();
  ProjectCell text;
  text.id = std::numeric_limits<std::uint64_t>::max() - 1U;
  text.kind = "text";
  text.source = "🙂hello";
  text.formatting = content.formatting;
  text.extra["native"] = R"({"selection":2})";
  document.sheets[0].cells.push_back(text);
  const auto document_roundtrip = amber::notebook::parse_project_document(
      amber::notebook::serialize_project_document(document));
  const auto &roundtrip_text = document_roundtrip.sheets[0].cells.back();
  expect(roundtrip_text.kind == "text" &&
             roundtrip_text.source == text.source &&
             roundtrip_text.formatting == text.formatting &&
             roundtrip_text.extra == text.extra,
         "text cells should retain source, formatting, and unknown metadata");

  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"({"source":"🙂","formatting":{"version":3,"runs":[]}})");
      },
      "unknown rich-text format versions should be rejected");
  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"({"source":"🙂","formatting":{"version":1,"runs":[{"start":1,"length":1}]}})");
      },
      "rich-text ranges splitting a surrogate pair should be rejected");
  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"({"source":"abcd","formatting":{"version":1,"runs":[{"start":0,"length":3},{"start":2,"length":1}]}})");
      },
      "overlapping rich-text ranges should be rejected");
  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"({"source":"abcd","formatting":{"version":1,"runs":[{"start":0,"length":1,"future":true}]}})");
      },
      "unknown rich-text run metadata should be rejected");
}

void test_v2_rich_text_headings_lists_and_tables() {
  const std::string source = "Title\n•\tOne\n2.\tFirst\n2.\tSecond\nA\nB\n";
  const std::string formatting =
      R"JSON({"version":2,"runs":[{"start":0,"length":5,"style":"heading1"},{"start":6,"length":5,"bold":true},{"start":31,"length":1,"italic":true}],"paragraphs":[{"start":0,"length":6,"style":"heading1"},{"start":6,"length":6,"list":"bullet"},{"start":12,"length":9,"list":"numbered"},{"start":21,"length":10,"list":"numbered"}],"tables":[{"start":31,"length":4,"columns":1,"cells":[{"start":31,"length":2},{"start":33,"length":2}]}]})JSON";
  amber::notebook::ProjectTextContent input{source, formatting};
  amber::notebook::validate_project_text_content(input);
  const auto content = amber::notebook::parse_project_text_content(
      amber::notebook::serialize_project_text_content(input));
  expect(content.source == source && content.formatting == formatting,
         "v2 rich-text metadata should validate and be retained exactly");
  const auto reparsed = amber::notebook::parse_project_text_content(
      amber::notebook::serialize_project_text_content(content));
  expect(reparsed.source == source && reparsed.formatting == formatting,
         "v2 rich-text metadata should round-trip");

  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"JSON({"source":"a\n","formatting":{"version":2,"runs":[],"paragraphs":[{"start":0,"length":1}]}})JSON");
      },
      "v2 paragraph metadata must cover a complete LF-delimited paragraph");
  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"JSON({"source":"x\n","formatting":{"version":2,"runs":[],"paragraphs":[{"start":0,"length":2,"list":"bullet"}]}})JSON");
      },
      "v2 bullet metadata must match the rendered bullet prefix");
  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"JSON({"source":"A\nB\n","formatting":{"version":2,"runs":[],"tables":[{"start":0,"length":4,"columns":2,"cells":[{"start":0,"length":2}]}]}})JSON");
      },
      "v2 tables must contain a positive multiple of their columns");
  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"JSON({"source":"🙂\n","formatting":{"version":2,"runs":[],"paragraphs":[{"start":1,"length":2}]}})JSON");
      },
      "v2 metadata ranges must not split UTF-16 surrogate pairs");
  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"JSON({"source":"A\nB\n","formatting":{"version":2,"runs":[],"tables":[{"start":0,"length":4,"columns":1,"cells":[{"start":0,"length":1},{"start":1,"length":3}]}]}})JSON");
      },
      "v2 table cells must each end after LF and partition the table");
  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"JSON({"source":"A\n","formatting":{"version":2,"runs":[],"future":true}})JSON");
      },
      "v2 unknown formatting members should be rejected");
  expect_throws(
      [] {
        (void)amber::notebook::parse_project_text_content(
            R"JSON({"source":"A\n","formatting":{"version":1,"runs":[],"paragraphs":[]}})JSON");
      },
      "v1 must not accept v2 formatting members");
}

void test_create_project_module_publishes_source_before_manifest() {
  TempDirectory temporary;
  const std::filesystem::path directory = temporary.path / "create_module";
  amber::notebook::LoadedProject project =
      amber::notebook::create_project(directory, valid_document());
  const std::string hostile_source =
      std::string("value = \"do not execute\"\n") +
      std::string(1, '\0') + std::string(1, static_cast<char>(0xff));
  const ProjectModule entry{"new_module", "modules/new_module.am", {}};

  const amber::notebook::LoadedProjectModule created =
      amber::notebook::create_project_module(&project, entry, hostile_source);
  expect(created.id == entry.id && created.path == entry.path &&
             created.source == hostile_source &&
             created.baseline == hostile_source,
         "new module should return exact source baseline");
  expect(read_text(directory / entry.path) == hostile_source,
         "new module source should be published byte-for-byte");
  expect(project.document.modules.size() == 1U &&
             project.document.modules.front().id == entry.id &&
             project.document.auto_imports.empty(),
         "new module should update only the manifest module list");
  expect(read_text(directory / "project.json") == project.baseline,
         "new module project baseline should match persisted manifest");
  const auto reopened = amber::notebook::load_project(directory);
  const auto loaded =
      amber::notebook::load_project_module(reopened, entry.id);
  expect(loaded.source == hostile_source &&
             !std::filesystem::exists(temporary.path / "executed.marker"),
         "new module should load without execution");

  const std::string manifest_before_duplicate =
      read_text(directory / "project.json");
  expect_throws(
      [&] {
        amber::notebook::create_project_module(
            &project,
            ProjectModule{"new_module", "modules/nested/new_module.am", {}},
            "other\n");
      },
      "duplicate module ID should be rejected before publication");
  expect(read_text(directory / "project.json") == manifest_before_duplicate,
         "duplicate module ID should preserve the manifest");

  write_text(directory / "modules/existing.am", "existing bytes\n");
  expect_throws(
      [&] {
        amber::notebook::create_project_module(
            &project, ProjectModule{"existing", "modules/existing.am", {}},
            "replacement\n");
      },
      "existing module path should be rejected before overwrite");
  expect(read_text(directory / entry.path) == hostile_source &&
             read_text(directory / "modules/existing.am") == "existing bytes\n",
         "existing module path should remain unchanged");

  project.document.title = "dirty";
  expect_throws(
      [&] {
        amber::notebook::create_project_module(
            &project, ProjectModule{"dirty", "modules/dirty.am", {}},
            "dirty\n");
      },
      "dirty in-memory project should reject module creation");
  expect(!std::filesystem::exists(directory / "modules/dirty.am"),
         "dirty project rejection should not create a source file");
}

void test_create_project_module_rejects_conflicts_and_symlinks() {
  TempDirectory temporary;
  const std::filesystem::path conflict_directory = temporary.path / "conflict";
  amber::notebook::LoadedProject conflict =
      amber::notebook::create_project(conflict_directory, valid_document());
  ProjectDocument external = conflict.document;
  external.title = "external";
  write_text(conflict_directory / "project.json",
             amber::notebook::serialize_project_document(external));
  const std::string conflict_manifest =
      read_text(conflict_directory / "project.json");
  expect_throws(
      [&] {
        amber::notebook::create_project_module(
            &conflict, ProjectModule{"conflict", "modules/conflict.am", {}},
            "conflict\n");
      },
      "external project manifest edit should reject module creation");
  expect(read_text(conflict_directory / "project.json") == conflict_manifest &&
             !std::filesystem::exists(conflict_directory / "modules"),
         "external conflict should leave manifest and module directory alone");

  const std::filesystem::path symlink_directory = temporary.path / "symlink";
  amber::notebook::LoadedProject symlink_project =
      amber::notebook::create_project(symlink_directory, valid_document());
  const std::filesystem::path outside = temporary.path / "outside";
  std::error_code error;
  std::filesystem::create_directories(outside, error);
  expect(!error, "symlink target directory should be creatable");
  std::filesystem::create_symlink(outside, symlink_directory / "modules",
                                  error);
  expect(!error, "module directory symlink should be creatable");
  expect_throws(
      [&] {
        amber::notebook::create_project_module(
            &symlink_project,
            ProjectModule{"outside", "modules/outside.am", {}},
            "must not follow\n");
      },
      "module directory symlink should be rejected");
  expect(!std::filesystem::exists(outside / "outside.am"),
         "module directory symlink should not mutate its target");

  const std::filesystem::path leaf_directory = temporary.path / "leaf";
  amber::notebook::LoadedProject leaf_project =
      amber::notebook::create_project(leaf_directory, valid_document());
  std::filesystem::create_directories(leaf_directory / "modules", error);
  expect(!error, "module directory should be creatable");
  write_text(outside / "leaf.am", "outside leaf\n");
  std::filesystem::create_symlink(outside / "leaf.am",
                                  leaf_directory / "modules/leaf.am", error);
  expect(!error, "module leaf symlink should be creatable");
  expect_throws(
      [&] {
        amber::notebook::create_project_module(
            &leaf_project, ProjectModule{"leaf", "modules/leaf.am", {}},
            "must not overwrite\n");
      },
      "module leaf symlink should be rejected");
  expect(read_text(outside / "leaf.am") == "outside leaf\n" &&
             read_text(leaf_directory / "project.json") == leaf_project.baseline,
         "module leaf symlink should preserve outside and manifest bytes");
}

void test_symlink_parent_dotdot_uses_actual_filesystem_root() {
  TempDirectory temporary;
  const std::filesystem::path parent_a = temporary.path / "A";
  const std::filesystem::path parent_b = temporary.path / "B";
  std::error_code directory_error;
  std::filesystem::create_directories(parent_a, directory_error);
  expect(!directory_error, "first symlink-parent fixture should be creatable");
  directory_error.clear();
  std::filesystem::create_directories(parent_b / "child", directory_error);
  expect(!directory_error, "second symlink-parent fixture should be creatable");

  ProjectDocument a_document = valid_document();
  a_document.title = "project in A";
  amber::notebook::create_project(parent_a / "book", a_document);
  ProjectDocument b_document = valid_document();
  b_document.title = "project in B";
  amber::notebook::create_project(parent_b / "book", b_document);

  const std::filesystem::path parent_link = parent_a / "link";
  std::error_code symlink_error;
  std::filesystem::create_symlink(parent_b / "child", parent_link,
                                  symlink_error);
  expect(!symlink_error, "symlinked parent fixture should be creatable");

  const std::filesystem::path through_link = parent_link / ".." / "book";
  amber::notebook::LoadedProject selected =
      amber::notebook::load_project(through_link);
  expect(selected.document.title == "project in B",
         "link/.. must select B/book on the actual filesystem");
  ProjectDocument modified = selected.document;
  modified.title = "modified project in B";
  amber::notebook::save_project(&selected, modified);
  expect(amber::notebook::load_project(parent_a / "book").document.title ==
             "project in A",
         "saving through link/.. must not modify A/book");
  expect(amber::notebook::load_project(parent_b / "book").document.title ==
             "modified project in B",
         "saving through link/.. must modify B/book");

  const std::filesystem::path root_link = temporary.path / "root-link";
  symlink_error.clear();
  std::filesystem::create_symlink(parent_b / "book", root_link, symlink_error);
  expect(!symlink_error, "symlinked project root fixture should be creatable");
  expect_throws(
      [&] {
        amber::notebook::load_project(
            std::filesystem::path(root_link.string() + "/"));
      },
      "a symlinked project root with a trailing slash must be rejected");
  expect_throws([&] { amber::notebook::load_project(root_link / "."); },
                "a symlinked project root with /. must be rejected");
}

} // namespace

int main() {
  test_document_roundtrip_preserves_semantics_and_raw_metadata();
  test_json_unicode_escapes_and_nested_raw_values();
  test_document_id_boundaries_and_validation();
  test_module_and_auto_import_validation();
  test_malformed_json_and_schema_errors();
  test_filesystem_create_save_and_reopen();
  test_create_refuses_existing_targets();
  test_external_modification_conflict_preserves_disk();
  test_traversal_and_symlinked_members_are_rejected();
  test_load_has_no_execution_effects_and_save_preserves_modules();
  test_module_source_persistence_conflicts_and_independent_baselines();
  test_module_project_mapping_and_project_conflicts();
  test_module_symlink_components_and_nonblocking_project_lock();
  test_create_project_module_publishes_source_before_manifest();
  test_create_project_module_rejects_conflicts_and_symlinks();
  test_text_content_roundtrip_and_strict_formatting();
  test_v2_rich_text_headings_lists_and_tables();
  test_symlink_parent_dotdot_uses_actual_filesystem_root();
  std::cout << "notebook_project_tests ok\n";
  return 0;
}
