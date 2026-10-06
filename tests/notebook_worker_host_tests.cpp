#include "tools/notebook-macos/NotebookBridge.h"
#include "tools/notebook-macos/NotebookExecution.h"
#include "notebook/project.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace std::chrono_literals;
static void require(bool value, const std::string &message) {
  if (!value) { std::cerr << "worker host: " << message << '\n'; std::exit(1); }
}
static std::string snapshot(AmberNotebook *host) {
  char *error = nullptr; char *raw = amber_notebook_snapshot(host, &error);
  require(raw && !error, error ? error : "snapshot failed");
  std::string result(raw); amber_notebook_string_free(raw); return result;
}
static std::string command(AmberNotebook *host, const char *action, const char *cell = nullptr,
                           const char *text = nullptr, const char *tab = "sheet:main") {
  char *error = nullptr;
  const auto ok = amber_notebook_command(host, action, tab, cell, text, &error);
  const std::string result = error ? error : ok ? "" : "operation failed";
  amber_notebook_string_free(error); return result;
}
static void succeeds(AmberNotebook *host, const char *action, const char *cell = nullptr,
                     const char *text = nullptr) { const auto error = command(host, action, cell, text); require(error.empty(), error); }
static void wait_running(AmberNotebookControl *control) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (amber_notebook_control_phase(control) < 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  require(amber_notebook_control_phase(control) >= 2, "execution starts while UI/control thread is responsive");
}
static std::string first_cell(const std::string &state, const std::string &tab) {
  const auto start = state.find("\"id\":\"" + tab + "\"");
  require(start != std::string::npos, "tab exists");
  const std::string marker = "\"cells\":[{\"id\":\"";
  const auto cell = state.find(marker, start);
  require(cell != std::string::npos, "tab cell exists");
  const auto from = cell + marker.size();
  return state.substr(from, state.find('"', from) - from);
}
static void module_apply_tests(const std::filesystem::path &root, const char *worker) {
  auto document = amber::notebook::make_project_document("Apply worker");
  document.sheets[0].cells.resize(1);
  document.sheets[0].cells[0].source = "answer()\n";
  document.sheets.push_back(document.sheets[0]);
  document.sheets[1].id = "second";
  document.sheets[1].cells[0].id = amber::notebook::allocate_cell_id();
  auto project = amber::notebook::create_project(root / "Apply.amberbook", document);
  document.modules.push_back({"helpers", "helpers.am", {}}); document.auto_imports = {"helpers"};
  std::ofstream(project.directory / "helpers.am") << "package helpers\nexport answer\ndef answer(): 41\n";
  amber::notebook::save_project(&project, document);
  auto *host = amber_notebook_open(project.directory.c_str(), 0, nullptr, nullptr);
  require(host, "open module fixture");
  auto *control = amber_notebook_enable_worker(host, worker, nullptr);
  require(control, "module control");
  succeeds(host, "run_all");
  require(command(host, "run_all", nullptr, nullptr, "sheet:second").empty(), "independent second sheet worker");
  const auto before = snapshot(host);
  require(before.find("\"result\":\"41\"") != std::string::npos, "saved module executes");
  const auto module_cell = first_cell(before, "module:helpers");
  require(command(host, "source", module_cell.c_str(), "package helpers\nexport answer\ndef answer(): 99\n", "module:helpers").empty(), "edit module inertly");
  require(command(host, "apply").find("Save modified") != std::string::npos, "Apply refuses unsaved module buffer");
  require(command(host, "run", module_cell.c_str(), nullptr, "module:helpers").find("sheet execution only") != std::string::npos,
          "module Run never falls back to an in-process runtime");
  require(command(host, "save", nullptr, nullptr, "module:helpers").empty(), "host saves module");
  succeeds(host, "apply");
  auto state = snapshot(host);
  require(state.find("\"result\":\"99\"") != std::string::npos && state.find("\"environment_stale\":true") != std::string::npos,
          "Apply installs saved module and marks other sheet stale");
  require(command(host, "apply", nullptr, nullptr, "sheet:second").empty(), "second worker independently applies environment");
  state = snapshot(host);
  require(state.find("\"result\":\"41\"") == std::string::npos, "both sheets now have new environment results");
  // Root module initialization, not a specially annotated cell, must stop too.
  require(command(host, "source", module_cell.c_str(),
      "package helpers\nexport answer\ndef answer(): 123\nwhile true:\n  x = 1\n", "module:helpers").empty(), "edit slow module");
  require(command(host, "save", nullptr, nullptr, "module:helpers").empty(), "save slow module");
  std::string apply_error;
  std::thread owner([&] { apply_error = command(host, "apply"); });
  wait_running(control); std::this_thread::sleep_for(80ms);
  require(amber_notebook_control_stop(control, 0), "Stop addresses module initialization");
  owner.join(); require(apply_error.empty(), apply_error);
  state = snapshot(host);
  require(state.find("Cancelled") != std::string::npos && state.find("\"result\":\"99\"") != std::string::npos,
          "cancelled Apply retains prior results and reports cancellation");
  succeeds(host, "new_input", nullptr, "{\"id\":\"gain\",\"title\":\"Gain\",\"type\":\"number\",\"default\":1}");
  require(command(host, "run_all").find("project inputs") != std::string::npos &&
          command(host, "set_input", "gain", "2").find("not yet supported") != std::string::npos,
          "input execution cannot silently fall back to local VM");
  amber_notebook_destroy(host); amber_notebook_control_destroy(control);
}
int main(int argc, char **argv) {
  require(argc == 2, "worker path"); alarm(40);
  char pattern[] = "/tmp/amber-worker-host-XXXXXX";
  const char *root = mkdtemp(pattern); require(root, "temporary directory");
  auto document = amber::notebook::make_project_document("Worker host");
  document.sheets[0].cells.resize(1); document.sheets[0].cells[0].source = "1\n";
  const auto cell = std::to_string(document.sheets[0].cells[0].id);
  const auto project = amber::notebook::create_project(std::filesystem::path(root) / "Host.amberbook", document);
  char *error = nullptr;
  auto *host = amber_notebook_open(project.directory.c_str(), 0, nullptr, &error);
  require(host && !error, "host open");
  auto *control = amber_notebook_enable_worker(host, argv[1], &error);
  require(control && !error, "enable worker preview");
  require(amber_notebook_control_phase(control) == 0 && !amber_notebook_control_stop(control, 1), "inert open has no process to kill");
  succeeds(host, "source", cell.c_str(), "answer = 42\nanswer\n");
  succeeds(host, "run_all");
  auto state = snapshot(host);
  require(state.find("\"result\":\"42\"") != std::string::npos && state.find("\"modified\":true") != std::string::npos,
          "unsaved source executes but remains host-owned/modified");
  succeeds(host, "add_text", cell.c_str());
  succeeds(host, "run_all"); // Unsaved structure, not present in the disk bootstrap.
  require(snapshot(host).find("\"kind\":\"text\"") != std::string::npos, "Sync accepts unsaved structure without changing it");
  succeeds(host, "source", cell.c_str(), "try:\n  while true:\n    x = 1\nensure:\n  print(\"STOPPED\")\n");
  std::string run_error;
  std::thread owner([&] { run_error = command(host, "run", cell.c_str()); });
  wait_running(control); std::this_thread::sleep_for(80ms);
  require(amber_notebook_control_stop(control, 0), "cooperative Stop uses independent control lifetime");
  owner.join(); require(run_error.empty(), run_error);
  state = snapshot(host);
  require(state.find("Cancelled") != std::string::npos && state.find("STOPPED") != std::string::npos, "Stop result and ensure output merged");
  succeeds(host, "source", cell.c_str(), "99\n"); succeeds(host, "run_all");
  require(snapshot(host).find("\"result\":\"99\"") != std::string::npos, "ordinary Stop does not require restart");
  succeeds(host, "source", cell.c_str(),
      "notebook.progress(\"training\", 3, 10, \"MNIST batches\", false)\n"
      "task.sleep(600)\nnotebook.progress(\"training\", 10, 10, \"MNIST batches\", true)\n99\n");
  owner = std::thread([&] { run_error = command(host, "run", cell.c_str()); });
  wait_running(control);
  std::string telemetry;
  const auto live_deadline = std::chrono::steady_clock::now() + 4s;
  while (telemetry.empty() && std::chrono::steady_clock::now() < live_deadline) {
    if (char *json = amber_notebook_control_live(control)) {
      telemetry = json; amber_notebook_string_free(json);
    }
    if (telemetry.empty()) std::this_thread::sleep_for(10ms);
  }
  require(telemetry.find("MNIST batches") != std::string::npos &&
          telemetry.find("\"current\":3.") != std::string::npos && amber_notebook_control_phase(control) > 0,
          "UI receives actual progress while owner is still executing");
  owner.join(); require(run_error.empty(), run_error);
  require(snapshot(host).find("\"done\":true") != std::string::npos, "final progress survives terminal merge");
  succeeds(host, "source", cell.c_str(), "while true:\n  x = 1\n");
  owner = std::thread([&] { run_error = command(host, "run", cell.c_str()); });
  wait_running(control);
  require(amber_notebook_control_stop(control, 1), "Force Stop targets active worker");
  owner.join(); require(!run_error.empty(), "forced world cannot publish success");
  state = snapshot(host);
  require(state.find("\"result\":\"99\"") != std::string::npos && state.find("Restart required") != std::string::npos,
          "Force Stop preserves last result with interrupted/stale label");
  require(amber_notebook_control_phase(control) == 0 && !amber_notebook_control_stop(control, 1), "confirmed exit closes control generation");
  require(command(host, "run", cell.c_str()).find("Restart") != std::string::npos, "never restart/replay implicitly");
  succeeds(host, "restart_worker");
  require(snapshot(host).find("No cells replayed") != std::string::npos, "restart is inert");
  succeeds(host, "source", cell.c_str(), "import task\nchild = task.spawn:\n  task.sleep(100)\n  1 / 0\n17\n");
  succeeds(host, "run_all");
  require(snapshot(host).find("unobserved child task") != std::string::npos, "late child failure reaches native host status");
  const auto disk = amber::notebook::load_project(project.directory);
  require(disk.document.sheets[0].cells.size() == 1 && disk.document.sheets[0].cells[0].source == "1\n",
          "worker never saves source or unsaved sheet structure");
  amber_notebook_destroy(host);
  require(amber_notebook_control_phase(control) == 0 && !amber_notebook_control_stop(control, 1), "control safely outlives its retired host");
  amber_notebook_control_destroy(control);
  module_apply_tests(root, argv[1]);
  NotebookExecutionControl publication;
  publication.begin(); require(publication.commit() && !publication.stop(true), "late Force Stop cannot kill committed runtime");
  publication.begin(); require(publication.stop(true) && !publication.commit(), "Force Stop before commit prevents publication");
  publication.finish();
  std::filesystem::remove_all(root); alarm(0);
  std::cout << "notebook_worker_host_tests: ok\n";
}
