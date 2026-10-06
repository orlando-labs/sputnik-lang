def mark(log, tag, value):
  log.push!(tag)
  value

def guarded_return(enabled):
  [1]
    .each if enabled |n|:
      return n + 8
  3

class ConditionalArgs:
  def init(@log)

  def accept(*args, value: 0, **options):
    @log.push!(value)
    self

def probe():
  checks = []
  log = []
  original = [1, 2, 3]
  result = mark(log, 1, original)
    .push!(mark(log, 3, 9)) if mark(log, 2, false)
    .reversed() if mark(log, 4, true)
    .take(mark(log, 6, 2)) unless mark(log, 5, false)
  checks.push!(log == [1, 2, 4, 5, 6])
  checks.push!(result == [3, 2])
  checks.push!(original == [1, 2, 3])

  # A skipped segment preserves identity, not just equal contents.
  same = original .push!(9) if false
  same.push!(4)
  checks.push!(original == [1, 2, 3, 4])
  checks.push!((original .take(1 / 0) if false) == original)
  checks.push!((false .take(1) if false) == false)
  checks.push!(([] .first() if true) == null)
  checks.push!(([] .any? if true) == false)

  # Keyword and spread operands remain inside the guard in both branches.
  arg_log = []
  receiver = ConditionalArgs(arg_log)
  checks.push!((receiver .accept(value: mark(arg_log, 99, 1)) if false) == receiver)
  checks.push!((receiver .accept(*mark(arg_log, 99, []), **mark(arg_log, 99, {})) if false) == receiver)
  receiver
    .accept(value: mark(arg_log, 2, 7)) if mark(arg_log, 1, true)
    .accept(*mark(arg_log, 4, [1]), **mark(arg_log, 5, {value: 8})) unless mark(arg_log, 3, false)
  checks.push!(arg_log == [1, 2, 7, 3, 4, 5, 8])

  # Adjacent calls form one segment; separated calls are independent.
  checks.push!((original .reversed().take(1) if false .size()) == 4)
  checks.push!((original .reversed().take(1) if true .first()) == 4)
  checks.push!((original .reversed() if false .take(2) unless false) == [1, 2])
  checks.push!((original .reversed() unless true) == original)
  checks.push!((original .reversed() if null) == original)
  checks.push!((original .reversed() if 0) == [4, 3, 2, 1])

  # Guard expressions are outside block-parameter scope; closures still capture.
  x = true
  offset = 10
  mapped = original
    .map if x |x|:
      x + offset
    .take(2)
  checks.push!(mapped == [11, 12])
  mapped_inline = original .map if x |n|: n + offset .take(2)
  checks.push!(mapped_inline == mapped)
  checks.push!((original .map() unless false |n|: n * 2 .first()) == 2)
  checks.push!((original .map if true: $it + 1 .first()) == 2)
  skipped = original
    .map if false |n|:
      log.push!(99)
      1 / 0
    .first()
  checks.push!(skipped == 1)
  checks.push!(log == [1, 2, 4, 5, 6])

  checks.push!(guarded_return(true) == 9)
  checks.push!(guarded_return(false) == 3)
  caught = false
  try:
    original .map if true |n|:
      1 / 0
  rescue ZeroDivisionError:
    caught = true
  checks.push!(caught)

  # Conditions may contain ordinary member chains and nested spaced chains.
  checks.push!((original .reversed() if original.size() > 2 and (original .any?) .first()) == 4)
  checks.push!((null .?.take(mark(log, 99, 1)) if true) == null)
  checks.push!(log == [1, 2, 4, 5, 6])

  # Top-level collection guards control presence; grouping opts into value guards.
  pairs = {absent: original .reversed() if false,
           kept: (original .reversed() if false)}
  checks.push!(pairs.keys() == [:kept])
  checks.push!(pairs[:kept] == original)
  checks.push!([original .reversed() if false] == [])
  checks.push!([(original .reversed() if false)] == [original])
  checks.push!({**{absent: 1} unless true, kept: 2} == {kept: 2})

  # Ordinary adjacent postfix statements keep their original guard semantics.
  ordinary = 7
  ordinary = mark(log, 99, original).reversed() if false
  checks.push!(ordinary == 7)
  checks.push!(log == [1, 2, 4, 5, 6])

  # Internal receiver storage must not replace the enclosing last value.
  7
  last_guard = original .reversed() if $_ == 7
  checks.push!(last_guard == [4, 3, 2, 1])

  checks.all? |ok|: ok
