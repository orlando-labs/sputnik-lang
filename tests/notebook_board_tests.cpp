#include "notebook/project.h"
#include "tools/iamber/tabs.h"
#include "tools/notebook-macos/NotebookBridge.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <thread>

using namespace amber::notebook;
static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template<class F> static void rejects(F f, const char *message) {
  bool rejected = false; try { f(); } catch (const std::exception &) { rejected = true; }
  check(rejected, message);
}
static ProjectDocument fixture() {
  auto doc = make_project_document("Board test"); doc.version = 2;
  doc.inputs = {{"gain", "Gain", "number", 2.0, 0.0, 100.0, {}},
                {"enabled", "Enabled", "boolean", true, {}, {}, {}},
                {"label", "Label", "string", std::string("Привет"), {}, {}, {}}};
  doc.sheets[0].cells = {
      {1, "code", "gain = notebook.input(\"gain\")\ngain\n", CellMode::Watch, {}, {}},
      {2, "code", "value = gain * 21\nvalue\n", CellMode::Watch, {}, {}},
      {3, "code", "manual = notebook.input(\"gain\") * 100\nmanual\n", CellMode::Manual, {}, {}},
      {4, "code", "manual + 1\n", CellMode::Watch, {}, {}},
      {5, "code", "42\n", CellMode::Watch, {}, {}}};
  doc.boards = {{"dashboard", "Dashboard", "main", 2,
                 {{"gain", "input", "Gain", "gain", 0, "", {}},
                  {"value", "text", "Result", "", 2, "value", {}},
                  {"run", "run", "Recompute", "", 3, "", {}}}, {}}};
  return doc;
}
int main() {
  char pattern[] = "/tmp/amber-board-tests-XXXXXX";
  const auto temp = mkdtemp(pattern); check(temp, "mkdtemp");
  const auto root = std::filesystem::path(temp);
  try {
    auto values = std::make_shared<amber::runtime::NotebookInputSnapshot>();
    values->emplace("gain", 2.0);
    amber::runtime::NotebookInputCapture capture(values);
    std::thread worker([&] { check(!capture.read("gain"), "foreign thread must not read root inputs"); });
    worker.join();
    check(capture.read("gain").has_value(), "root thread input capture");
    check(capture.read("gain").has_value() && capture.finish().size() == 1, "duplicate reads collapse");
    check(!capture.read("gain"), "closed capture rejects delayed reads");
    auto doc = fixture();
    auto encoded = serialize_project_document(doc);
    auto decoded = parse_project_document(encoded);
    check(decoded.version == 2 && decoded.boards.size() == 1 && decoded.inputs.size() == 3, "v2 roundtrip");
    check(std::get<bool>(decoded.inputs[1].initial), "true must survive JSON roundtrip");
    check(serialize_project_document(decoded) == encoded, "stable v2 serialization");
    check(std::get<std::int64_t>(parse_project_input_value("9223372036854775807")) == INT64_MAX, "exact int64 input");
    rejects([] { parse_project_input_value("9223372036854775808"); }, "overflow rejected");
    rejects([] { parse_project_input_value("1e999"); }, "nonfinite rejected");
    auto invalid = doc; invalid.version = 1;
    rejects([&] { serialize_project_document(invalid); }, "v1 must not silently carry executable inputs");
    invalid = doc; invalid.inputs[0].initial = 101.0;
    rejects([&] { serialize_project_document(invalid); }, "default bounds validated");
    invalid = doc; invalid.boards[0].components[0].input = "missing";
    rejects([&] { serialize_project_document(invalid); }, "missing input rejected");
    auto project = create_project(root / "Test.amberbook", doc);
    ProjectTabs tabs(project);
    auto &s = tabs.active().session;
    check(s.cells[0].dirty && !s.cells[0].ok, "opening must not execute");
    tabs.set_input("gain", "3");
    check(!s.cells[0].ok, "changing input must not start unopened runtime");
    evaluate_from(&s, 0, true, false, false);
    if (!s.cells[1].ok) std::cerr << s.cells[1].error << ' ' << s.status << '\n';
    check(s.cells[1].ok && s.cells[1].result == "63", "cell reads typed project input");
    s.cells[4].result = "unchanged";
    check(tabs.set_input("gain", "4.0"), "changed input accepted");
    check(s.cells[1].ok && s.cells[1].result == "84", "dependent Watch cell reruns");
    check(!s.cells[2].ok && !s.cells[3].ok, "Manual and descendants stay stale");
    check(s.cells[4].result == "unchanged", "unrelated cells must not rerun");
    check(!tabs.set_input("gain", "4"), "equal numeric publication is quiet");
    rejects([&] { tabs.set_input("gain", "101"); }, "invalid input rejected");
    check(std::get<double>(tabs.inputs().at("gain")) == 4.0, "invalid input retains committed value");
    tabs.save_active();
    check(std::get<double>(load_project(project.directory).document.inputs[0].initial) == 2.0, "runtime input not persisted as default");
    // Failed new read paths retry after another input event without dropping
    // the last successful capture or publishing a partial slot transaction.
    s.cells[0].source = "gain = notebook.input(\"gain\") / (notebook.input(\"gain\") - 4)\ngain\n";
    evaluate_from(&s, 0, true, false, false);
    check(!s.cells[0].ok, "faulting input cell");
    tabs.set_input("gain", "5");
    check(s.cells[0].ok && s.cells[1].ok && s.cells[1].result == "105", "input event recovers failed consumer");
    s.cells[0].source = "gain = 7\ngain\n";
    evaluate_from(&s, 0, true, false, false);
    s.cells[0].result = "capture-cleared";
    tabs.set_input("gain", "6");
    check(s.cells[0].result == "capture-cleared", "successful empty capture removes input edge");
    s.cells[0].source = "gain = notebook.input(\"gain\")\ngain\n";
    tabs.set_input("gain", "7");
    check(s.cells[0].dirty && s.cells[0].result == "capture-cleared", "input change cannot execute edited source");
    char *error = nullptr;
    auto host = amber_notebook_open(project.directory.c_str(), 0, nullptr, &error);
    check(host != nullptr, error ? error : "bridge open");
    auto json = amber_notebook_snapshot(host, &error);
    check(json && std::string(json).find("\"boards\":[{") != std::string::npos, "bridge exposes boards");
    amber_notebook_string_free(json);
    check(amber_notebook_command(host, "set_input", nullptr, "gain", "8", &error), "bridge input command");
    check(amber_notebook_command(host, "new_input", nullptr, nullptr,
          R"({"id":"large","title":"Large","type":"integer","default":9223372036854775807})", &error), "bridge creates exact integer input");
    json = amber_notebook_snapshot(host, &error);
    check(json && std::string(json).find("9223372036854775807") != std::string::npos, "snapshot preserves int64");
    amber_notebook_string_free(json);
    amber_notebook_destroy(host);
    // Editing understood properties must preserve exact opaque metadata.
    ProjectTabs reopened(load_project(project.directory));
    auto board = reopened.project().document.boards.front();
    board.extra["future"] = "1.23456789012345678901234567890";
    board.components[0].extra["future"] = "{\"x\":17}";
    reopened.save_board(board);
    board.extra.clear(); board.components[0].extra.clear(); board.title = "Changed";
    reopened.save_board(board);
    check(reopened.project().document.boards.front().extra.count("future"), "board editor preserves extras");
    check(reopened.project().document.boards.front().components[0].extra.count("future"), "component editor preserves extras");
    // Project values are shared, but each sheet executes in its own world.
    reopened.create_sheet("other");
    reopened.active().session.cells[0].source = "notebook.input(\"label\")\n";
    evaluate_from(&reopened.active().session, 0, true, false, false);
    reopened.set_input("label", "\"Мир\"");
    check(reopened.active().session.cells[0].ok && reopened.active().session.cells[0].result.find("Мир") != std::string::npos,
          "new sheets share portable project input snapshot");
    auto &other = reopened.active().session;
    other.cells[0].source = "notebook.input(\"large\")\n";
    evaluate_from(&other, 0, true, false, false);
    check(other.cells[0].ok && other.cells[0].result == "9223372036854775807", "Int64 reaches VM without truncation");
    other.cells[0].source = "notebook.input(\"enabled\")\n";
    evaluate_from(&other, 0, true, false, false);
    reopened.set_input("enabled", "false");
    check(other.cells[0].ok && other.cells[0].result == "false", "boolean input remains boolean in VM");
    other.cells[0].source = "notebook.input(42)\n";
    evaluate_from(&other, 0, true, false, false);
    check(!other.cells[0].ok, "non-string input key rejected by VM");
    other.cells[0].source = "notebook.input(\"unknown\")\n";
    evaluate_from(&other, 0, true, false, false);
    check(!other.cells[0].ok && other.cells[0].error.find("unknown") != std::string::npos, "unknown input rejected");
    std::filesystem::remove_all(root);
    std::cout << "notebook board tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << "notebook board tests failed: " << e.what() << '\n';
    return 1;
  }
}
