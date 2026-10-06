#include "runtime/notebook_live.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

using amber::runtime::NotebookDisplay;
using amber::runtime::NotebookLiveEventKind;
using amber::runtime::NotebookLivePublishResult;
using amber::runtime::NotebookLiveScope;
using amber::runtime::NotebookLiveStore;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "notebook live test failed: " << message << '\n';
    std::exit(1);
  }
}

void test_replacement_and_immutable_snapshot() {
  auto store = std::make_shared<NotebookLiveStore>();
  const auto generation = store->begin_cell(7);
  expect(store->publish_progress(7, "loss", 1.0, 4.0, "epoch 1", false,
                                 100, generation) ==
             NotebookLivePublishResult::Accepted,
         "first progress update should be accepted");
  expect(store->publish_progress(7, "loss", 2.0, 4.0, "epoch 2", false,
                                 125, generation) ==
             NotebookLivePublishResult::Accepted,
         "same progress id should replace");
  auto figure = NotebookDisplay{"image/png", "png", "loss plot", 2, 2};
  figure.plot_scene = "{\"version\":1}";
  expect(store->publish_figure(7, "loss-plot", figure, 0, 125, generation) ==
             NotebookLivePublishResult::Accepted,
         "figure should be accepted");
  const auto snapshot = store->snapshot_cell(7);
  expect(snapshot.size() == 2U &&
             snapshot[0].kind == NotebookLiveEventKind::Progress &&
             snapshot[0].current == 2.0 && snapshot[0].elapsed_ms == 25U &&
             snapshot[1].kind == NotebookLiveEventKind::Figure,
         "snapshot should contain latest progress and figure panels");
  expect(snapshot[0].sequence < snapshot[1].sequence,
         "accepted replacements should retain monotonic sequences");
  figure.plot_scene = "{\"version\":2}";
  expect(store->publish_figure(7, "loss-plot", figure, 0, 150, generation) == NotebookLivePublishResult::Accepted,
         "scene replacement is accepted");
  expect(snapshot[1].display.plot_scene == "{\"version\":1}" &&
             store->snapshot_cell(7)[1].display.plot_scene == "{\"version\":2}",
         "live scenes are immutable copies, replaced with their image");
  figure.plot_scene.assign(4U * 1024U * 1024U + 1, 'x');
  expect(store->publish_figure(7, "too-large", figure, 0, 175, generation) == NotebookLivePublishResult::Invalid,
         "live scenes enforce their own byte limit");
}

void test_throttle_terminal_frame_and_stale_generation() {
  auto store = std::make_shared<NotebookLiveStore>();
  const auto generation = store->begin_cell(3);
  NotebookDisplay display{"image/png", "one", "", 1, 1};
  expect(store->figure_due(3, "plot", 100, 100),
         "first frame should be due");
  expect(store->publish_figure(3, "plot", display, 100, 100, generation) ==
             NotebookLivePublishResult::Accepted,
         "first frame should be accepted");
  expect(!store->figure_due(3, "plot", 100, 150),
         "recent frame should be throttled before rendering");
  expect(store->publish_figure(3, "plot", display, 100, 150, generation) ==
             NotebookLivePublishResult::Throttled,
         "publish should close the concurrent throttle race");
  expect(store->publish_figure(3, "plot", display, 0, 150, generation) ==
             NotebookLivePublishResult::Accepted,
         "zero throttle should accept terminal frame");
  const auto next_generation = store->begin_cell(3);
  expect(next_generation != generation &&
             store->publish_progress(3, "loss", 1, 2, "stale", false, 200,
                                     generation) == NotebookLivePublishResult::Closed,
         "late event from a restarted cell should be rejected");
  expect(store->publish_progress(3, "loss", 1, 2, "fresh", false, 200,
                                 next_generation) ==
             NotebookLivePublishResult::Accepted,
         "new generation should be accepted");
  std::uint64_t revision = 0;
  std::vector<amber::runtime::NotebookLiveEvent> snapshot;
  expect(store->snapshot_if_changed(&revision, &snapshot) && snapshot.size() == 1,
         "first revision snapshot");
  expect(!store->snapshot_if_changed(&revision, &snapshot), "unchanged store skips PNG copying");
  store->begin_cell(3);
  expect(store->snapshot_if_changed(&revision, &snapshot) && snapshot.empty(),
         "reset invalidates snapshot even without a subsequent event");
}

void test_tls_scope_and_close() {
  auto store = std::make_shared<NotebookLiveStore>();
  const auto generation = store->begin_cell(9);
  {
    NotebookLiveScope scope(store, 9, generation);
    expect(NotebookLiveScope::current() == store &&
               NotebookLiveScope::current_cell_id() == 9,
           "scope should expose the current run sink");
    expect(NotebookLiveScope::progress("batch", 1, 2, "loading") ==
               NotebookLivePublishResult::Accepted,
           "scope progress should publish");
  }
  expect(!NotebookLiveScope::current(), "scope should restore prior TLS state");
  store->close();
  expect(store->publish_progress(9, "late", 1, 1, "", true, 500,
                                 generation) == NotebookLivePublishResult::Closed &&
             store->snapshot().empty(),
         "closed store should reject and release late events");
}

void test_payload_limits() {
  NotebookLiveStore store;
  expect(store.publish_progress(1, "p", std::numeric_limits<double>::infinity(), 1, "", false) == NotebookLivePublishResult::Invalid,
         "infinite values never enter UI telemetry");
  expect(store.publish_progress(1, "p", -1, 1, "", false) == NotebookLivePublishResult::Invalid,
         "negative counts are rejected");
  for (std::size_t i = 0; i < NotebookLiveStore::kMaxEntries; ++i)
    expect(store.publish_progress(1, std::to_string(i), 0, 10, "", false) == NotebookLivePublishResult::Accepted,
           "bounded entries fit");
  expect(store.publish_progress(1, "overflow", 0, 10, "", false) == NotebookLivePublishResult::LimitExceeded,
         "new entries do not grow past the quota");
  expect(store.publish_progress(1, "0", 2, 10, "", false) == NotebookLivePublishResult::Accepted,
         "replacement remains possible at quota");
}

} // namespace

int main() {
  test_replacement_and_immutable_snapshot();
  test_throttle_terminal_frame_and_stale_generation();
  test_tls_scope_and_close();
  test_payload_limits();
  return 0;
}
