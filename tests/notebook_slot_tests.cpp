#include "notebook/slot_table.h"

#include "runtime/heap.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using amber::notebook::BindingKey;
using amber::notebook::NotebookSlotTable;
using amber::notebook::SlotProviderMetadata;
using amber::runtime::Value;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

void test_publish_and_equal_write_revision() {
  NotebookSlotTable table;
  const BindingKey key{11, "answer"};
  const SlotProviderMetadata provider{11, "answer", "cell.am", "main", 3};

  const auto first = table.publish(key, Value::integer(42), provider);
  expect(first.committed && first.events.size() == 1U,
         "first write should publish one event");
  expect(first.events.front().old_revision == 0U &&
             first.events.front().new_revision == 1U,
         "first publication should advance a slot revision");

  const auto snapshot = table.find(key);
  expect(snapshot.has_value() && snapshot->initialized && !snapshot->stale &&
             snapshot->published.is_integer() &&
             snapshot->published.as_integer() == 42 &&
             snapshot->revision == 1U && snapshot->provider == provider,
         "published value and provider metadata should be retained");

  const auto equal =
      table.publish(key, Value::integer(42),
                    SlotProviderMetadata{11, "answer", "new.am", "main", 4});
  expect(equal.committed && equal.events.empty(),
         "equal write should not emit a publication event");
  const auto after_equal = table.find(key);
  expect(after_equal.has_value() && after_equal->revision == 1U &&
             after_equal->provider.file == "new.am" &&
             after_equal->provider.generation == 4U,
         "equal write should retain metadata without advancing revision");

  const auto different = table.publish(key, Value::integer(43));
  expect(different.events.size() == 1U &&
             different.events.front().new_revision == 2U,
         "different write should advance revision");
}

void test_transaction_is_atomic_and_rollback_is_inert() {
  NotebookSlotTable table;
  const BindingKey first{1, "first"};
  const BindingKey second{2, "second"};

  auto transaction = table.begin_transaction();
  expect(transaction.stage(first, Value::integer(1)),
         "transaction should accept first staged write");
  expect(transaction.stage(second, Value::integer(2)),
         "transaction should accept second staged write");
  expect(!table.contains(first) && !table.contains(second),
         "staged writes must remain private until commit");

  const auto result = transaction.commit();
  expect(result.committed && result.events.size() == 2U,
         "batch commit should publish both writes");
  expect(table.read(first)->as_integer() == 1 &&
             table.read(second)->as_integer() == 2,
         "batch commit should make all writes visible");

  const BindingKey rolled_back{3, "rolled_back"};
  auto failed = table.begin_transaction();
  failed.stage(rolled_back, Value::integer(99));
  failed.rollback();
  expect(!table.contains(rolled_back),
         "rollback should discard unseen staged slots");
  expect(!failed.active() && !failed.commit().committed,
         "a rolled back transaction cannot commit later");
}

void test_stale_state_and_root_visitors() {
  NotebookSlotTable table;
  const BindingKey key{8, "value"};
  table.publish(key, Value::integer(8));
  expect(table.mark_stale(key), "existing slot should become stale");
  expect(table.find(key)->stale && table.find(key)->revision == 1U,
         "staleness should not change the published revision");
  table.publish(key, Value::integer(8));
  expect(!table.find(key)->stale && table.find(key)->revision == 1U,
         "equal successful publish should clear stale without revision bump");

  const BindingKey uninitialized{9, "never_run"};
  auto stale_transaction = table.begin_transaction();
  stale_transaction.stage_stale(uninitialized);
  stale_transaction.commit();
  expect(table.find(uninitialized).has_value() &&
             !table.find(uninitialized)->initialized &&
             table.find(uninitialized)->stale,
         "stale transaction should preserve an uninitialized slot state");

  const std::vector<Value> roots = table.gc_roots();
  expect(roots.size() == 1U && roots.front().as_integer() == 8,
         "only initialized values should be exposed as GC roots");
  std::vector<Value> appended;
  table.append_gc_roots(&appended);
  expect(appended.size() == 1U && appended.front().as_integer() == 8,
         "GC root append API should match root enumeration");
  std::size_t visited = 0;
  table.visit_values([&visited](const Value &value) {
    expect(value.is_integer() && value.as_integer() == 8,
           "visitor should receive published values");
    ++visited;
  });
  expect(visited == 1U, "visitor should visit every initialized slot once");
}

void test_noexcept_stale_batch_marks_only_existing_slots() {
  NotebookSlotTable table;
  const BindingKey first{10, "first"};
  const BindingKey second{11, "second"};
  table.publish(first, Value::integer(1));
  table.publish(second, Value::integer(2));

  expect(table.mark_stale_batch({first, {99, "missing"}}),
         "prepared commit batch should acquire and mutate atomically");
  expect(table.find(first)->stale && !table.find(second)->stale,
         "stale batch should ignore missing keys and preserve unrelated slots");
  expect(table.mark_stale_batch({first}, false) && !table.find(first)->stale,
         "stale batch should also clear an existing stale marker");
}

void test_heap_value_is_kept_in_root_snapshot() {
  amber::runtime::RuntimeHeap heap;
  NotebookSlotTable table;
  const BindingKey key{20, "items"};
  table.publish(key, heap.make_list_value({Value::integer(1)}));
  const auto roots = table.gc_roots();
  expect(roots.size() == 1U && roots.front().is_list() &&
             roots.front().as_list()->items.size() == 1U,
         "heap-backed published values should be enumerable as roots");
}

} // namespace

int main() {
  test_publish_and_equal_write_revision();
  test_transaction_is_atomic_and_rollback_is_inert();
  test_stale_state_and_root_visitors();
  test_noexcept_stale_batch_marks_only_existing_slots();
  test_heap_value_is_kept_in_root_snapshot();
  std::cout << "notebook_slot_tests ok\n";
  return 0;
}
