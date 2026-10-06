#include "tools/notebook-macos/NotebookBridge.h"
#include "runtime/notebook_display.h"
#include "notebook/project.h"
#include "tools/iamber/session.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <unistd.h>

static void require(bool ok, const std::string &message) {
  if (!ok) { std::cerr << "notebook plot test failed: " << message << '\n'; std::exit(1); }
}
static std::string state(AmberNotebook *host) {
  char *error = nullptr;
  char *json = amber_notebook_snapshot(host, &error);
  require(json != nullptr, error ? error : "snapshot");
  std::string result(json); amber_notebook_string_free(json); return result;
}
static void command(AmberNotebook *host, const char *action, const char *text = nullptr) {
  char *error = nullptr;
  int ok = amber_notebook_command(host, action, "sheet:main", "1", text, &error);
  require(ok != 0, error ? error : action);
}
static std::string displays(const std::string &json) {
  const auto start = json.find("\"displays\":[");
  require(start != std::string::npos, "snapshot displays field");
  bool quoted = false, escaped = false;
  int depth = 0;
  for (auto i = start + std::string("\"displays\":").size(); i < json.size(); ++i) {
    const auto c = json[i];
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else if (c == '"') quoted = true;
    else if (c == '[') ++depth;
    else if (c == ']' && --depth == 0) return json.substr(start, i - start + 1);
  }
  require(false, "unterminated displays"); return {};
}
static void collector_tests() {
  using namespace amber::runtime;
  NotebookDisplayCollector collector;
  require(collector.active(), "collector starts active");
  std::thread worker([&] { require(!collector.active(), "parallel child cannot publish a figure"); }); worker.join();
  NotebookDisplayOrder first; first.int_value = 9007199254740993LL;
  NotebookDisplayOrder second; second.integer = false; second.float_value = 9007199254740992.0;
  require(first.compare(second) > 0, "mixed numeric order must not round large integers");
  collector.append({"image/png", "one", "second"}, first);
  collector.append({"image/png", "two", "first"}, second);
  collector.append({"image/png", "three", "equal"}, second);
  auto result = collector.take();
  require(result.size() == 3 && result[0].caption == "first" && result[1].caption == "equal", "stable numeric ordering");
  require(!collector.active(), "closed collector cannot publish after cell completion");
  bool rejected = false;
  try { collector.append({}, first); } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "late detached publications rejected");
  NotebookDisplayCollector failed;
  failed.append({"image/png", "discard", "partial"}, first);
  failed.close();
  require(failed.take().empty(), "closing a failed run releases staged bytes");
  NotebookDisplayCollector limit;
  for (unsigned i = 0; i < 64; ++i) limit.append({}, second);
  rejected = false;
  try { limit.append({}, second); } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected && limit.take().size() == 64, "panel limit must reject without partial append");
}
static void scope_tests() {
  auto compiled = compile_source_text("notebook.show(42)\n", "<outside-notebook>", "outside");
  require(compiled.ok, "builtin should compile outside notebook too");
  amber::runtime::RuntimeWorld world(compiled.module);
  const auto result = world.execute(compiled.module.init.entry_code_id);
  require(!result.ok() && result.fault->error_name == "NotebookCellError" && result.displays.empty(),
          "ordinary module execution cannot create unowned display output");
}
int main(int argc, char **argv) {
  collector_tests();
  scope_tests();
  require(argc == 2 || argc == 3, "pass amber-plot src/plot.am and optional snapshot output");
  const auto package = std::filesystem::canonical(argv[1]).parent_path().parent_path();
  char pattern[] = "/tmp/amber-notebook-plot-XXXXXX";
  const auto temp = mkdtemp(pattern);
  require(temp != nullptr, "temporary project directory");
  const auto directory = std::filesystem::path(temp);
  auto document = amber::notebook::make_project_document("Plot regression");
  document.sheets[0].cells.resize(1);
  document.sheets[0].cells[0].id = 1;
  document.sheets[0].cells[0].source =
      "p = figure(width: 320, height: 240).line([[0, 0], [1, 1]])\n"
      "notebook.show(p, caption: \"late\", order: 2)\n"
      "notebook.show(p, caption: \"early\", order: 1)\n"
      "notebook.show(p, caption: \"equal\", order: 1)\n42\n";
  auto project = amber::notebook::create_project(directory / "Plots.amberbook", document);
  std::ofstream(project.directory / "display_helpers.am") <<
      "package display_helpers\n"
      "export same_thread_show, parallel_show, nested_show\n"
      "def same_thread_show():\n"
      "  h = task.async: notebook.show(42)\n"
      "  h.wait()\n"
      "def parallel_show():\n"
      "  h = task.spawn: notebook.show(42)\n"
      "  h.wait()\n"
      "def nested_show(p):\n"
      "  [1, 2].each |n|: notebook.show(p, caption: \"nested\", order: n)\n";
  document.version = 3;
  document.dependencies.push_back({package.string(), true, {}});
  document.modules.push_back({"display_helpers", "display_helpers.am", {}});
  document.auto_imports = {"display_helpers"};
  amber::notebook::save_project(&project, document);
  char *error = nullptr;
  auto *host = amber_notebook_open(project.directory.c_str(), 0, nullptr, &error);
  require(host != nullptr, error ? error : "open");
  command(host, "run_all");
  auto json = state(host);
  const auto baseline = displays(json);
  if (baseline == "\"displays\":[]") {
    std::cerr << json.substr(0, 5000) << '\n';
  }
  require(json.find("\"result\":\"42\"") != std::string::npos && baseline.find("iVBOR") != std::string::npos,
          "real amber-plot Figure must render PNG through notebook.show");
  require(baseline.find("early") < baseline.find("equal") && baseline.find("equal") < baseline.find("late"), "ordered subfigures");
  require(baseline.find("\\\"version\\\":1") != std::string::npos && baseline.find("plot_scene") != std::string::npos,
          "real Figure exports an immutable interactive scene through JSON");
  command(host, "run_all");
  require(displays(state(host)) == baseline, "rerun replaces, never appends duplicate panels");
  command(host, "source", "p = figure(width: 320, height: 240).line([[0, 0], [1, 1]])\nnotebook.show(p, caption: \"discard me\")\n1 / 0\n");
  command(host, "run_all");
  if (displays(state(host)) != baseline) std::cerr << state(host).substr(0, 1600) << '\n';
  require(displays(state(host)) == baseline, "failed cell preserves previous successful figure");
  require(state(host).find("ZeroDivisionError") != std::string::npos, "failure must occur after rendering, not during compilation");
  for (const char *source : {"same_thread_show()\n", "parallel_show()\n"}) {
    command(host, "source", source); command(host, "run_all");
    require(state(host).find("active synchronous notebook cell") != std::string::npos && displays(state(host)) == baseline,
            "scheduler tasks must not inherit synchronous display publication");
  }
  command(host, "source", "notebook.show(42)\n"); command(host, "run_all");
  require(state(host).find("currently supports amber-plot") != std::string::npos, "unsupported object gives a clear error");
  command(host, "source", "notebook.show(figure(width: 9000), order: 1)\n"); command(host, "run_all");
  require(state(host).find("dimensions must fit") != std::string::npos, "oversized images rejected before rendering");
  command(host, "source", "notebook.show(figure(width: 320, height: 240), caption: 42)\n"); command(host, "run_all");
  require(state(host).find("caption must be Str") != std::string::npos, "caption type checked before rendering");
  command(host, "source", "p = figure(width: 320, height: 240)\nnotebook.show(p, order: 1)\nnotebook.show(p, order: :other)\n"); command(host, "run_all");
  require(state(host).find("not comparable") != std::string::npos && displays(state(host)) == baseline,
          "incompatible order keys reject the whole new batch");
  command(host, "source", "p = figure(width: 320, height: 240)\nnotebook.show(p, caption: \"first\")\nnotebook.show(p, caption: \"second\")\n42\n"); command(host, "run_all");
  const auto default_order = displays(state(host));
  require(default_order.find("first") < default_order.find("second"), "omitted order retains call order");
  command(host, "source", "p = figure(width: 320, height: 240)\nnotebook.show(p.render(format: :png), caption: \"Rendered PNG\")\n42\n"); command(host, "run_all");
  require(displays(state(host)).find("Rendered PNG") != std::string::npos, "explicit Rendered PNG accepted");
  command(host, "source", "nested_show(figure(width: 320, height: 240))\n42\n"); command(host, "run_all");
  const auto nested = displays(state(host));
  const auto first_nested = nested.find("nested");
  require(first_nested != std::string::npos && nested.find("nested", first_nested + 1) != std::string::npos,
          "ordinary synchronous methods and blocks share the cell's display batch");
  command(host, "source", "q = figure3d(width: 320, height: 240).surface([[0, 1], [1, 0]])\nnotebook.show(q, caption: \"3D\", order: [:surface, 1])\n42\n");
  command(host, "run_all");
  require(displays(state(host)).find("3D") != std::string::npos, "real Figure3D PNG rendering");
  require(displays(state(host)).find("\"plot_scene\":\"\"") != std::string::npos, "3D stays a raster presentation");
  command(host, "source", "p = figure(width: 320, height: 240).line([[0, 0], [1, 1], [2, 4]])\n"
      "notebook.show(p, id: \"loss\", caption: \"first live frame\", throttle_ms: 0)\n"
      "notebook.show(p.view(x: [0, 1], y: [0, 2]), id: \"loss\", caption: \"latest live frame\", throttle_ms: 0)\n42\n");
  command(host, "run_all");
  const auto live = displays(state(host));
  require(live.find("latest live frame") != std::string::npos && live.find("first live frame") == std::string::npos &&
              live.find("\\\"version\\\":1") != std::string::npos,
          "live replacements retain the latest interactive scene: " + state(host).substr(0, 1200));
  command(host, "source", "42\n"); command(host, "run_all");
  require(displays(state(host)) == "\"displays\":[]", "successful empty display clears previous figure");
  command(host, "source",
      "p = figure(width: 800, height: 540).title(\"Viewport regression\").axes(x_label: \"Step\", y_label: \"Loss\")\n"
      "p = p.line([[0, 0], [1, 1], [2, 4], [3, 9], [4, 16]]).vline(2, label: \"x=2\", dy: 8)\n"
      "p = p.annotate([2, 4], \"visible\").annotate([4, 16], \"outside\")\n"
      "q = p.view(x: [1, 3], y: [0, 10])\n"
      "notebook.show(p, caption: \"Original\")\nnotebook.show(q, caption: \"Data view\")\n"
      "p.domain_value == null and q.domain_value == [1, 0, 3, 10] and q.layers.count() == p.layers.count()\n");
  command(host, "run_all");
  const auto views = state(host);
  require(views.find("\"result\":\"true\"") != std::string::npos && displays(views).find("Data view") != std::string::npos,
          "view preserves original figure, layers, axes and annotations: " + views.substr(0, 1600));
  if (argc == 3) std::ofstream(argv[2]) << views;
  command(host, "source", "figure().line([[0, 0], [3, 9]]).view(x: [3, 1])\n"); command(host, "run_all");
  require(state(host).find("maximum must exceed minimum") != std::string::npos, "reversed limits rejected");
  command(host, "source", "figure().view(x: [1])\n"); command(host, "run_all");
  require(state(host).find("minimum, maximum") != std::string::npos, "invalid axis pair rejected");
  amber_notebook_destroy(host);
  std::filesystem::remove_all(directory);
  std::cout << "notebook plot tests passed\n";
}
