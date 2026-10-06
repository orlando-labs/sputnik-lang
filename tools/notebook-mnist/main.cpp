#include "notebook/project.h"
#include "frontend/lexer/token.h"
#include "tools/notebook-worker/process.h"
#include "tools/notebook-worker/presentation.h"
#include <fstream>
#include <algorithm>
#include <iostream>
#include <set>
#include <thread>
#include <sys/wait.h>

using namespace amber::notebook;
using namespace amber::notebook::worker;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

static std::string read(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read " + path.string());
  return {std::istreambuf_iterator<char>(stream), {}};
}
static std::string quoted(const std::string &text) {
  return serialize_project_input_value(amber::runtime::NotebookInputValue{text});
}
static void prepare(const fs::path &torch, const fs::path &plot, const fs::path &out, bool smoke) {
  const auto base = fs::current_path();
  auto document = make_project_document("MNIST · Amber Notebook");
  document.version = 3; document.sheets[0].title = "Train a digit recognizer";
  document.sheets[0].cells.clear();
  auto cell = [&](std::string kind, std::string source) -> CellId {
    ProjectCell value; value.id = allocate_cell_id(); value.kind = kind;
    value.mode = CellMode::Manual;
    if (kind == "text") {
      // Showcase headings are ASCII; formatting ranges are UTF-16 offsets.
      value.formatting = "{\"version\":2,\"runs\":[],\"paragraphs\":[{\"start\":0,\"length\":" +
          std::to_string(source.find('\n') + 1) + ",\"style\":\"" +
          (document.sheets[0].cells.empty() ? "heading1" : "heading2") + "\"}],\"tables\":[]}";
    }
    value.source = std::move(source);
    document.sheets[0].cells.push_back(value); return value.id;
  };
  cell("text", "MNIST / A small network, learning live\n"
       "Train ambertorch’s 19,706-parameter residual CNN on real handwritten digits. "
       "The original package supplies the model and IDX data reader; this notebook adds the presentation.\n"
       "Run All to begin. Loss and progress update while training. Stop cooperatively; Force Stop terminates the isolated worker.");
  cell("code", "dataset = " + quoted((torch / "build/datasets/mnist").string()) +
       "\ndevice = \"mps\"\nepochs = " + (smoke ? "1" : "5") +
       "\nbatch_size = 128\nplot_throttle_ms = 500\ntrain_limit = " + (smoke ? "1024" : "null") +
       "\ntest_limit = " + (smoke ? "256" : "null") + "\n");
  cell("text", "Live training\nOrange: minibatch cross-entropy. Green: held-out test accuracy after each epoch. "
       "Plot rendering is throttled before PNG generation; the final frame is always explicitly flushed. "
       "Changing parameters does not silently restart training.");
  const auto training = cell("code", "metrics = train_mnist(dataset, device:, epochs:, batch_size:, train_limit:, test_limit:, plot_throttle_ms:)\n"
       "accuracy = metrics[:accuracy]\ntest_loss = metrics[:test_loss]\nseconds = metrics[:seconds]\nmetrics\n");
  cell("text", "Results\nTest accuracy: {{accuracy}}% · Test loss: {{test_loss}} · Training time: {{seconds}} s\n"
       "Click a figure to zoom, save or share it. Rerun with five full epochs for the showcase; a small smoke run only verifies the pipeline.");
  document.modules.push_back({"mnist_demo", "modules/training.am", {}});
  document.auto_imports = {"mnist_demo"};
  const auto library = torch / "build/native/libambertorch_notebook.so";
  document.dependencies.push_back({torch.string(), false,
      {{"manifest", quoted("build/ambertorch.showcase.build.json")},
       {"native_library", quoted("build/native/libambertorch_notebook.so")},
       {"native_sha256", quoted(amber::lexer::sha256_hex(read(library)))}}});
  document.dependencies.push_back({plot.string(), false, {}});
  document.dependencies.push_back({(base / "packages/progressbar").string(), false, {}});
  ProjectBoard board; board.id = "training"; board.title = "Training dashboard";
  board.sheet = document.sheets[0].id; board.columns = 1;
  board.components.push_back({"loss", "plot", "Live learning curves", {}, training, {}, {}});
  board.components.push_back({"accuracy", "text", "Accuracy (%)", {}, training, "accuracy", {}});
  document.boards.push_back(std::move(board));
  // Create with no module declarations first; source then manifest atomically.
  auto initial = document; initial.modules.clear(); initial.auto_imports.clear();
  auto project = create_project(out, initial);
  fs::create_directories(project.directory / "modules");
  std::ofstream(project.directory / "modules/training.am") << read(base / "tools/notebook-mnist/training.am");
  save_project(&project, document);
  std::cout << project.directory << '\n';
}
struct ForcedStopVerified {};
static Presentation receive(Process &process, RequestId request, std::size_t *live_count = nullptr,
                            std::set<std::string> *frames = nullptr, bool stop = false, bool force = false) {
  PresentationAssembler terminal(request.generation, request.request);
  std::optional<PresentationAssembler> live;
  const auto deadline = std::chrono::steady_clock::now() + 20min;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto message = process.receive(1s);
    if (!message) continue;
    if (message->kind == Kind::Error) throw std::runtime_error(message->fields.at(0));
    if (message->kind == Kind::Started || message->kind == Kind::Draining) continue;
    if (message->kind == Kind::LiveBegin || message->kind == Kind::LiveChunk || message->kind == Kind::LiveEnd) {
      if (!live) live.emplace(request.generation, request.request, true);
      if (auto snapshot = live->accept(*message)) {
        if (live_count) ++*live_count;
        for (const auto &cell : snapshot->cells) {
          for (const auto &progress : cell.progress)
            std::cout << "live progress " << progress.id << ": " << progress.current << '/' << progress.total << (progress.done ? " done" : "") << '\n';
          for (const auto &panel : cell.displays)
            if (frames && panel.caption.rfind("Training cross-entropy", 0) == 0)
              frames->insert(amber::lexer::sha256_hex(panel.bytes));
        }
        if (stop && live_count && *live_count >= 2 && frames && !frames->empty()) {
          if (force) {
            if (!process.request_force_stop(request.generation)) throw std::runtime_error("Force Stop rejected");
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline) {
              if (const auto status = process.poll_exit()) {
                if (!WIFSIGNALED(*status) || WTERMSIG(*status) != SIGKILL) throw std::runtime_error("worker did not exit by SIGKILL");
                throw ForcedStopVerified{};
              }
              std::this_thread::sleep_for(10ms);
            }
            throw std::runtime_error("worker exit was not confirmed");
          }
          process.request_stop(request); stop = false;
        }
        live.reset();
      }
    } else if (auto result = terminal.accept(*message)) return std::move(*result);
  }
  throw std::runtime_error("showcase exceeded 20 minute test timeout");
}
static void verify(const std::string &worker, const fs::path &path, bool stop, bool force) {
  auto project = load_project(path);
  Process process; process.start(worker);
  auto ready = process.receive(5s);
  if (!ready || ready->kind != Kind::Ready) throw std::runtime_error("worker did not start");
  auto load = process.send(Kind::Load, {project.directory.string(), "", "1"});
  receive(process, load);
  const auto &cells = project.document.sheets[0].cells;
  auto first = std::find_if(cells.begin(), cells.end(), [](const auto &c) { return c.kind == "code"; });
  auto run = process.send(Kind::Run, {std::to_string(first->id), "1"});
  std::size_t live = 0; std::set<std::string> frames;
  Presentation result;
  try { result = receive(process, run, &live, &frames, stop || force, force); }
  catch (const ForcedStopVerified &) { std::cout << "VERIFIED Force Stop during real MNIST training; worker SIGKILL exit confirmed\n"; return; }
  std::cout << "Status: " << result.status << "\n" << result.environment_error << '\n';
  for (const auto &cell : result.cells) {
    if (!cell.error.empty()) std::cout << "ERROR " << cell.error << '\n';
    for (const auto &event : cell.output) std::cout << event.text;
    if (!cell.locals.empty()) std::cout << "CELL " << cell.id << ": " << cell.result << '\n';
  }
  if (!result.environment_error.empty()) throw std::runtime_error(result.environment_error);
  if (stop) {
    if (!result.cancelled || live < 2) throw std::runtime_error("MNIST Stop verification failed");
  } else {
    for (const auto &cell : result.cells) if (!cell.ok && !cell.locals.empty()) throw std::runtime_error(cell.error);
    for (const auto &cell : result.cells) if (!cell.error.empty()) throw std::runtime_error(cell.error);
    if (live < 2 || frames.size() < 2) throw std::runtime_error("missing dynamic progress/loss plot updates");
    bool complete = false;
    for (const auto &cell : result.cells) for (const auto &p : cell.progress)
      if (p.id == "mnist-training" && p.done && p.current == p.total && p.total > 0) complete = true;
    if (!complete) throw std::runtime_error("progressbar never completed in UI transport");
  }
  std::cout << "VERIFIED " << live << " live snapshots, " << frames.size() << " distinct PNG frames\n";
  auto shutdown = process.send(Kind::Shutdown); (void)shutdown; process.receive(5s);
}
int main(int argc, char **argv) {
  try {
    if (argc >= 5 && std::string(argv[1]) == "prepare")
      prepare(fs::canonical(argv[2]), fs::canonical(argv[3]), argv[4], argc > 5 && std::string(argv[5]) == "--smoke");
    else if (argc >= 4 && std::string(argv[1]) == "verify") verify(argv[2], argv[3], argc > 4 && std::string(argv[4]) == "--stop", argc > 4 && std::string(argv[4]) == "--force-stop");
    else throw std::runtime_error("usage: notebook-mnist prepare TORCH_ROOT PLOT_ROOT OUT [--smoke] | verify WORKER PROJECT [--stop|--force-stop]");
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
