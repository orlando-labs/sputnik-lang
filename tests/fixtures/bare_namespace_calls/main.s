package bare_namespace_calls

import "./provider" as provider
from "./provider" import answer

def through_parameter(provider): provider.answer

raise ValueError("namespace bare call") unless provider.answer == 42
reference = &provider.answer
imported = &answer
raise ValueError("reference construction invoked") unless provider.call_count == 1
raise ValueError("namespace reference") unless reference() == 42
raise ValueError("import remains a value") unless imported() == 42
returned = provider.factory
raise ValueError("callable result invoked twice") unless provider.call_count == 3
raise ValueError("callable result") unless returned() == 42
raise ValueError("dot-call of result") unless provider.factory.() == 42
raise ValueError("chained bare result") unless provider.answer.to_str == "42"
raise ValueError("safe namespace bare call") unless provider.?.answer == 42
raise ValueError("exactly once per read") unless provider.call_count == 7
raise ValueError("default argument binding") unless provider.defaults == 42
raise ValueError("rest and optional block") unless provider.rest == 42
raise ValueError("explicit namespace call") unless provider.defaults(40, offset: 2) == 42
with_block = provider.invoke: 42
raise ValueError("namespace block call") unless with_block == 42
box_class = provider.Box
box = box_class()
raise ValueError("class export read") unless box.answer == 9
raise ValueError("implicit self bare call") unless box.through_self == 9
raise ValueError("lexical namespace shadow") unless through_parameter(box) == 9
rejected = false
try:
  provider.required
rescue ArgumentError:
  rejected = true
raise ValueError("required argument accepted") unless rejected

local_calls = []
def local_answer():
  local_calls.push!(1)
  42

def through_callback(callback): callback
def through_capture(callback):
  invoke = ||: callback
  invoke

def with_defaults(value = 41, offset: 1): value + offset
def with_rest(*values, **keywords, &block): values.count + keywords.count + (if &block == null then 42 else block())
def invoke_block(&block): block
def forward_block(&block): invoke_block(&block)
def required(value): value

callback = &local_answer
copy = &callback
raise ValueError("reference read invoked") unless local_calls.count == 0
raise ValueError("free bare call") unless local_answer == 42
raise ValueError("callback bare call") unless callback == 42
raise ValueError("copied reference call") unless copy() == 42
raise ValueError("parameter bare call") unless through_callback((&callback)) == 42
raise ValueError("capture bare call") unless through_capture((&callback)) == 42
raise ValueError("explicit call double invoked") unless local_answer() == 42
raise ValueError("dot-call double invoked") unless local_answer.() == 42
raise ValueError("grouped call double invoked") unless (local_answer)() == 42
raise ValueError("function reads execute once") unless local_calls.count == 8
raise ValueError("free defaults") unless with_defaults == 42
raise ValueError("free rest") unless with_rest == 42
forwarded = forward_block: 42
raise ValueError("block forwarding") unless forwarded == 42
raise ValueError("imported bare function") unless answer == 42
rejected = false
try:
  required
rescue ArgumentError:
  rejected = true
raise ValueError("bare required function accepted") unless rejected
print "PASS bare namespace calls, references, defaults, blocks, classes and shadowing"
