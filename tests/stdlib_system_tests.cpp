#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/macro_expander.h"
#include "runtime/vm.h"
#include <cassert>
#include <iostream>

int main() {
  const std::string source = R"AMBER(
from system import cmd
def probe(workers):
  channel = Channel.new(capacity: 0)
  first = workers.async:
    cmd'cat'.capture(timeout: 2.0) with:
      stdin |input|:
        input.write_all!(channel.recv())
      stdout |output|:
        output.read_all!().to_str()
  second = workers.async:
    channel.send("async payload")
  second.wait()
  result = first.wait()
  scoped = workers.async:
    value = cmd'printf scoped'.spawn(timeout: 2.0) |process|:
      process.communicate().stdout.to_str()
    try:
      cmd'sleep 5'.spawn(timeout: 2.0) |process|:
        raise ValueError.new("scoped handler")
    rescue ValueError:
      1
    value + cmd'printf after'.output()
  scoped_result = scoped.wait()
  cancelled = workers.async:
    cmd'sleep 5'.capture()
  workers.sleep(0.03)
  cancelled.cancel()
  cancelled_ok = false
  try:
    cancelled.wait()
  rescue CancelledError:
    cancelled_ok = true
  if scoped_result == "scopedafter" and cancelled_ok and result.stdout_value == "async payload" and result.success?():
    42
  else:
    0
)AMBER";
  amber::lexer::Lexer lexer(source, "<system-test>");
  auto lex = lexer.lex();
  assert(lex.ok());
  amber::parser::Parser parser(lex.tokens);
  auto parsed = parser.parse_module_unit();
  assert(parsed.ok());
  const auto expansion = amber::macros::expand_macros(parsed.items, "", source);
  if (!expansion.ok) {
    std::cerr << expansion.error;
    return 1;
  }
  amber::ast::expand_quotes(parsed.items);
  auto bound = amber::binder::bind_module(parsed.items, "");
  assert(bound.ok());
  auto hir = amber::hir::lower_module(parsed.items, "", bound.graph);
  auto emitted = amber::bytecode::emit_program(hir, "");
  assert(emitted.ok());
  auto module = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(emitted.module));
  assert(module.ok());
  const auto before = amber::runtime::runtime_cooperative_task_park_count();
  auto workers = std::make_shared<amber::runtime::RuntimeTaskModule>(1);
  auto result = amber::runtime::execute_code(
      module.module, module.module.methods[0].entry_code_id,
      {amber::runtime::Value::task_module(workers)});
  if (result.fault) {
    std::cerr << result.fault->error_name << ": " << result.fault->message
              << '\n';
    return 1;
  }
  assert(result.value.is_integer() && result.value.as_integer() == 42);
  assert(amber::runtime::runtime_cooperative_task_park_count() > before);
  std::cout << "stdlib system tests passed (one worker)\n";
}
