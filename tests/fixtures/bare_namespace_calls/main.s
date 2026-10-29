package bare_namespace_calls

import "./provider" as provider
from "./provider" import answer

def through_parameter(provider): provider.answer

raise ValueError("namespace bare call") unless provider.answer == 42
reference = &provider.answer
imported = answer
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
print "PASS bare namespace calls, references, defaults, blocks, classes and shadowing"
