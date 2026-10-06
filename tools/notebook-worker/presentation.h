#pragma once

#include "tools/notebook-worker/protocol.h"
#include "tools/iamber/session.h"
#include "runtime/context.h"

namespace amber::notebook::worker {
constexpr std::size_t kMaxPresentationBytes = 128U * 1024U * 1024U;
constexpr std::size_t kPresentationChunkBytes = 1024U * 1024U;

// Execution-owned, inert data only. Never contains editor source, formatting,
// selection, project inputs or runtime Values. Hosts retain their own drafts.
struct CellPresentation {
  CellId id = 0;
  std::uint64_t source_revision = 0;
  bool dirty = true, ok = false;
  std::string result, error;
  ResultFormat result_format;
  std::vector<LocalView> locals;
  std::vector<runtime::RuntimeTextOutputEvent> output;
  std::vector<runtime::NotebookDisplay> displays;
  std::vector<runtime::NotebookLiveEvent> progress;
  std::uint64_t watch_epoch = 0, watch_event_count = 0;
  std::vector<CodeErrorRange> error_ranges;
  std::vector<CellErrorView> errors;
};
struct Presentation {
  // Root cell status remains independent from descendant outcomes. A host must
  // check cancellation and these failures as well as root diagnostics. Reading
  // task status never consumes a failure; wait/result/failure retrieval does.
  bool cancelled = false;
  std::string status, environment_error;
  bool environment_stale = false;
  std::vector<runtime::RuntimeTextOutputEvent> environment_output;
  std::vector<runtime::RuntimeRunTaskFailure> unobserved_task_failures;
  std::vector<CellPresentation> cells;
};
Presentation presentation_from_session(
    const Session &, const std::map<CellId, std::uint64_t> &source_revisions,
    bool cancelled);
std::string encode_presentation(const Presentation &);
Presentation decode_presentation(const std::string &);
void send_presentation(Channel &, std::uint64_t generation, std::uint64_t request,
                       const Presentation &, std::chrono::milliseconds timeout,
                       bool live = false);

// Discard this assembler on EOF/timeout abandonment/process death. Nothing is
// published before a complete, validated Result. Any malformed sequence poisons
// it permanently, including a stale generation/request or duplicate chunk.
class PresentationAssembler {
public:
  PresentationAssembler(std::uint64_t generation, std::uint64_t request, bool live = false);
  std::optional<Presentation> accept(const Message &);
private:
  enum class State { Empty, Receiving, Complete, Failed } state_ = State::Empty;
  std::uint64_t generation_, request_;
  bool live_;
  std::size_t expected_ = 0;
  std::string bytes_;
};

// Captures sinks, not Session pointers, in callbacks. Call finish only after
// run drain, on the Session owner thread. Last attempt per cell replaces its
// earlier output; text and source/stream/order metadata survive child cleanup.
class RunOutputCapture {
public:
  explicit RunOutputCapture(Session &);
  ~RunOutputCapture();
  void finish();
  RunOutputCapture(const RunOutputCapture &) = delete;
  RunOutputCapture &operator=(const RunOutputCapture &) = delete;
private:
  using Sinks = std::pair<std::shared_ptr<runtime::RuntimeTextWriter>,
                          std::shared_ptr<runtime::RuntimeTextWriter>>;
  Session &session_;
  SessionOutputObserver previous_;
  std::shared_ptr<std::vector<std::pair<CellId, Sinks>>> sinks_;
};
}
