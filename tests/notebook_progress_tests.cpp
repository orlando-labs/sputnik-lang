#include "tools/iamber/tabs.h"
#include "runtime/value_display.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <unistd.h>

static void require(bool value, const std::string &message) {
  if (!value) { std::cerr << "progress: " << message << '\n'; std::exit(1); }
}
int main() {
  using namespace amber::notebook;
  auto plain = compile_source_text("notebook.progress(\"cli\", 1, 2, \"CLI\", false)\n", "<progress-cli>", "cli");
  require(plain.ok, "standalone hook compiles");
  amber::runtime::RuntimeWorld world(plain.module);
  const auto outside = world.execute(plain.module.init.entry_code_id);
  require(outside.ok() && outside.value.is_bool() && !outside.value.as_bool(), "outside notebook selects console receiver");
  std::ifstream file("packages/progressbar/src/progressbar.am");
  const std::string package_source((std::istreambuf_iterator<char>(file)), {});
  auto console = compile_source_text(package_source +
      "\nconsole_probe = ProgressBar(total: 3, desc: \"console regression\", throttle: 0)\nconsole_probe.inc!(2)\nconsole_probe.end\nconsole_probe.current\n",
      "<progress-package-cli>", "progressbar");
  require(console.ok, "console package compiles: " + console.error);
  amber::runtime::RuntimeWorld console_world(console.module);
  const auto console_result = console_world.execute(console.module.init.entry_code_id);
  require(console_result.ok() && console_result.value.is_integer() && console_result.value.as_integer() == 2,
          "console renderer: " + (console_result.fault ? console_result.fault->message : "incorrect count"));

  // A method name used as a local used to overwrite parameter slot zero.
  const auto shadow = compile_source_text(
      "class Bar:\n  def total(): 10\n  def publish(done):\n    total = 8\n    [done, total]\nBar().publish(false)\n",
      "<progress-shadow>", "shadow");
  require(shadow.ok, "method/local shadow compiles");
  amber::runtime::RuntimeWorld shadow_world(shadow.module);
  const auto shadow_result = shadow_world.execute(shadow.module.init.entry_code_id);
  require(shadow_result.ok() && amber::runtime::value_to_debug_string(shadow_result.value) == "[false, 8]",
          "a method-shadowing local must not alias a parameter");

  char pattern[] = "/tmp/amber-progress-XXXXXX";
  const auto root = mkdtemp(pattern); require(root, "temporary project");
  auto doc = make_project_document("Progress package"); doc.version = 3;
  doc.dependencies.push_back({std::filesystem::canonical("packages/progressbar").string(), false, {}});
  doc.sheets[0].cells.resize(1);
  auto project = create_project(std::filesystem::path(root) / "Progress.amberbook", doc);
  ProjectTabs tabs(project);
  auto &session = tabs.active().session;
  auto run = [&](const std::string &source, bool success) -> const Cell & {
    session.cells[0].source = "from progressbar import ProgressBar, with_progress\n" + source;
    session.cells[0].dirty = true;
    evaluate_from(&session, 0, true, false, false);
    const auto &cell = session.cells[0];
    require(cell.ok == success && session.environment_error.empty(), cell.error + session.environment_error);
    return cell;
  };
  const auto &first = run(
      "p = ProgressBar(total: 10, id: \"explicit\", throttle: 10)\n"
      "p.inc!(5)\np.inc!()\np.end\np.end\n"
      "values = [1, 2, 3].with_progress(id: \"each\", throttle: 0).each |n|: n * 2\n"
      "p.current == 6 and values == [1, 2, 3]\n", true);
  require(first.result == "true" && first.progress.size() == 2, "package and collection adapter execute");
  require(first.progress[0].done && first.progress[0].current == 6 && first.progress[0].total == 10,
          "end flushes exact partial count and is idempotent");
  require(first.progress[1].done && first.progress[1].current == 3 && first.progress[1].total == 3,
          "collection hook counts successful completions");
  const auto &lazy = run("with_progress((1..3).lazy, id: \"lazy\").each |n|: n\n", true);
  require(lazy.progress.size() == 1 && lazy.progress[0].done && lazy.progress[0].current == 3 && lazy.progress[0].total == 0,
          "lazy adapter never exhausts a source to guess its size");
  const auto &failed = run("[1, 0, 3].with_progress(id: \"failure\", throttle: 0).each |n|: 1 / n\n", false);
  require(failed.error.find("ZeroDivisionError") != std::string::npos && failed.progress.size() == 1 &&
          failed.progress[0].current == 1 && failed.progress[0].done,
          "failed block closes receiver but never increments the failed item");
  std::filesystem::remove_all(root);
  std::cout << "notebook_progress_tests: ok\n";
}
