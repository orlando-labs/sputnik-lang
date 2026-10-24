export main

def check(condition):
  raise ValueError("transform lost distinct name keys") unless condition

def main():
  source = {account: "42", visits: 1}
  copied = source.transformed |key, value|: (key.str, value)
  check(copied.count == 2 and copied[:account] == "42" and copied[:visits] == 1)
  renamed = source.transformed_keys |key, value|: key.str + "_copy"
  check(renamed.count == 2 and renamed[:account_copy] == "42" and renamed[:visits_copy] == 1)
  mixed = {1: 10, 2: 20}.transformed |key, value|: (key.str, value)
  check(mixed.count == 2 and mixed["1"] == 10 and mixed["2"] == 20)
  collision = StrictMap{name: 1, "name": 2}.transformed |key, value|: (key.str, value)
  check(collision.count == 1 and collision["name"] == 2)
  strict = StrictMap{name: 1, "name": 2}
  preserved = strict.transformed |key, value|: (key, value)
  check(StrictMap === preserved and preserved[:name] == 1 and preserved["name"] == 2)
  renamed_strict = strict.transformed_keys |key, value|: key
  check(StrictMap === renamed_strict and renamed_strict.count == 2)
  values_strict = strict.transformed_values |value|: value + 1
  check(StrictMap === values_strict and values_strict[:name] == 2 and values_strict["name"] == 3)
  check(source.count == 2 and source[:account] == "42")
  # All pure forms return separate containers, including on frozen receivers.
  original = {a: 1, b: 2}
  pure = original.transformed |key, value|: (key, value * 10)
  pure[:a] = 99
  check(original[:a] == 1 and pure[:a] == 99)
  frozen = {a: 1}.freeze
  frozen_copy = frozen.transformed |key, value|: (key, value + 1)
  check(frozen_copy == {a: 2})
  keys = original.transformed_keys |key, value|: key.str + "_key"
  values = original.transformed_values |value, key|: value + 10
  check(keys[:a_key] == 1 and values[:b] == 12 and original[:b] == 2)

  # Mutations keep receiver identity: existing aliases and the return value see it.
  target = {a: 1, b: 2}
  alias = target
  returned = target.transform! |key, value|: (key.str + "_new", value * 10)
  returned[:proof] = 7
  check(alias[:a_new] == 10 and target[:b_new] == 20 and alias[:proof] == 7)
  target_names = target.keys.map: _1.str
  check(not target.has_key?(:a) and target_names == ["a_new", "b_new", "proof"])
  calls = []
  swapped = {a: 1, b: 2}
  swapped.transform! |key, value|:
    calls.push!(key.str)
    next_key = if key.str == "a" then "b" else "a"
    (next_key, value)
  check(calls == ["a", "b"] and swapped.count == 2 and swapped[:b] == 1 and swapped[:a] == 2)
  collapsed = {a: 1, b: 2}
  collapsed.transform! |key, value|: (:same, value)
  collapsed_names = collapsed.keys.map: _1.str
  check(collapsed.count == 1 and collapsed[:same] == 2 and collapsed_names == ["same"])
  strict.transform! |key, value|: [key, value + 10]
  check(StrictMap === strict and strict[:name] == 11 and strict["name"] == 12)
  strict.transform! |key, value|: (key.str, value)
  check(strict.count == 1 and strict["name"] == 12)
  empty = {}
  untouched = empty.transform! |key, value|: (key, value)
  untouched[:added] = 1
  check(empty[:added] == 1)

  # Results are staged: failures do not apply a partially transformed receiver.
  stable = {a: 1, b: 2}
  failed = false
  try:
    stable.transform! |key, value|:
      raise ValueError("stop") if key.str == "b"
      (key.str + "_new", value * 2)
  rescue ValueError:
    failed = true
  check(failed and stable == {a: 1, b: 2})
  [null, 1, [], [1], [1, 2, 3], {a: 1}].each |bad_pair|:
    failed = false
    try:
      stable.transform! |key, value|: bad_pair
    rescue TypeError:
      failed = true
    check(failed and stable == {a: 1, b: 2})
    failed = false
    try:
      stable.transformed |key, value|: bad_pair
    rescue TypeError:
      failed = true
    check(failed)
  failed = false
  try:
    stable.transform! |key, value|: ({bad: "key"}, value)
  rescue TypeError:
    failed = true
  check(failed and stable == {a: 1, b: 2})
  failed = false
  try:
    frozen.transform! |key, value|: (key, value)
  rescue FrozenError:
    failed = true
  check(failed and frozen[:a] == 1)
  [{}, {a: 1}].each |map|:
    failed = false
    try:
      map.transformed
    rescue TypeError:
      failed = true
    check(failed)
    failed = false
    try:
      map.transformed_keys
    rescue TypeError:
      failed = true
    check(failed)
    failed = false
    try:
      map.transformed_values
    rescue TypeError:
      failed = true
    check(failed)
    failed = false
    try:
      map.transform!
    rescue TypeError:
      failed = true
    check(failed)

  # Specialized mutators have the same staged-commit behavior in both backends.
  failed = false
  try:
    stable.transform_values! |value, key|:
      raise ValueError("stop") if key.str == "b"
      value * 2
  rescue ValueError:
    failed = true
  check(failed and stable == {a: 1, b: 2})
  failed = false
  try:
    stable.transform_keys! |key, value|:
      raise ValueError("stop") if key.str == "b"
      key.str + "_new"
  rescue ValueError:
    failed = true
  check(failed and stable == {a: 1, b: 2})
  print "PASS map transform names"
  0

main()
