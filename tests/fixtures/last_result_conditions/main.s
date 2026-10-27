package last_result_conditions
export main

def identity(value): value

def skipped(): raise ValueError("evaluated skipped logical operand")

def check(value, label):
  raise ValueError(label) unless value

def caller_result():
  999
  42

def main():
  "previous"
  if identity(42):
    check($_ == 42, "truthy if condition")
  else:
    raise ValueError("unreachable truthy else")

  99
  if identity(false):
    raise ValueError("unreachable false branch")
  else:
    check($_ == false, "false condition in else")

  99
  if identity(null):
    raise ValueError("unreachable null branch")
  else:
    check($_ == null, "null condition in else")

  # Logical operands retain the incoming last result, even when nested.
  10
  if identity(20) and identity($_ + 2):
    check($_ == 12, "and publishes only the completed condition")
  10
  if identity(null) or identity($_ + 3):
    check($_ == 13, "or publishes only the completed condition")
  10
  if (false or identity(20)) and identity($_ + 4):
    check($_ == 14, "nested logical expressions preserve incoming last")

  if identity(43) or skipped():
    check($_ == 43, "short-circuited or result")
  if identity(false) and skipped():
    raise ValueError("unreachable and branch")
  else:
    check($_ == false, "short-circuited and result")

  # Ordinary logical expressions retain their existing statement semantics.
  10
  value = identity(20) and identity($_ + 5)
  check(value == 15, "and outside conditions")

  if false:
    raise ValueError("unreachable first branch")
  else if identity($_ == false):
    check($_ == true, "else if sees the preceding completed condition")
  else:
    raise ValueError("else if lost last result")

  unless identity(null):
    check($_ == null, "unless preserves source value before synthetic negation")
  unless identity(44):
    raise ValueError("unreachable unless branch")
  else:
    check($_ == 44, "unless else preserves source value")

  value = if identity(45) then $_ else 0
  check(value == 45, "inline if condition")
  value = if identity(false) then 0 else $_
  check(value == false, "inline if else condition")

  captured = null
  captured = $_ if identity(46)
  check(captured == 46, "postfix if condition")
  captured = $_ unless identity(null)
  check(captured == null, "postfix unless condition")

  values = [$_ if identity(47)]
  check(values == [47], "conditional collection element")

  n = 2
  values = []
  while (if n > 0 then n else null):
    values.push!($_)
    n -= 1
  check(values == [2, 1], "while condition on every iteration")

  until identity(null):
    check($_ == null, "until source condition")
    break

  case 1:
    when 1 if identity(48):
      check($_ == 48, "case guard condition")
    else:
      raise ValueError("case guard failed")

  10
  if caller_result():
    check($_ == 42, "condition return value across call frames")

  missing = null
  if missing.?.value:
    raise ValueError("unreachable safe navigation branch")
  else:
    check($_ == null, "safe navigation publishes the final source condition")

  if "port=8080" =~ Regexp.compile("(\\w+)=(\\d+)"):
    check($_[1] == "port" and $_[2] == "8080", "regexp Match in condition")
  else:
    raise ValueError("regexp did not match")

  if "port" !~ Regexp.compile("\\d+"):
    check($_ == true, "regexp non-match boolean in condition")

  print("PASS last result conditions")
  42
