def check_counts(condition, message):
  if not condition:
    raise ValueError(message)

def main():
  words = ["a", "b", "a"]
  check_counts(words.counts == {"a": 2, "b": 1}, "bare counts")
  check_counts(words.counts == words.tally, "bare alias")

  users = [{role: "admin"}, {role: "member"}, {role: "admin"}]
  roles = users.counts: $it[:role]
  tallied_roles = users.tally: $it[:role]
  groups = users.group: $it[:role]
  sizes = groups.transform_values: $it.size
  check_counts(roles == {"admin": 2, "member": 1}, "key block")
  check_counts(roles == tallied_roles and roles == sizes, "group equivalence")
  check_counts(roles.keys == ["admin", "member"], "key order")
  check_counts(users.size == 3 and users[0][:role] == "admin", "receiver unchanged")

  visited = []
  divisor = 2
  parity = [3, 2, 5, 4, 7].counts |value|:
    visited.push!(value)
    value % divisor
  check_counts(parity == {1: 3, 0: 2}, "captured key block")
  check_counts(parity.keys == [1, 0], "computed key order")
  check_counts(visited == [3, 2, 5, 4, 7], "one call per item in order")

  empty = [].counts:
    raise ValueError("empty counts called its block")
  empty_tally = [].tally:
    raise ValueError("empty tally called its block")
  check_counts(empty == {} and empty_tally == {}, "empty blocks")
  check_counts([].counts == {} and [].tally == {}, "empty values")

  names = [:admin, "admin", :member].counts: $it
  check_counts(names == {admin: 2, member: 1}, "normalized name keys")
  check_counts(names["admin"] == 2 and names[:admin] == 2 and names.size == 2, "name lookup")
  check_counts(names.keys == [:admin, :member], "first name keys")
  numeric = [1, 1.0, 2].tally: $it
  check_counts(numeric == {1: 2, 2: 1}, "normalized numeric keys")
  composite = [1, 2, 3].counts: [$it % 2]
  check_counts(composite == {[1]: 2, [0]: 1}, "composite keys")
  nulls = [1, 2, 3].counts: null
  check_counts(nulls == {null: 3}, "null key")

  tuple_counts = (1, 2, 3).counts: $it % 2
  set_counts = {1, 2, 3}.tally: $it % 2
  range_counts = (1..3).counts: $it % 2
  check_counts(tuple_counts == {1: 2, 0: 1}, "tuple")
  check_counts(set_counts == tuple_counts and range_counts == tuple_counts, "set and range")
  check_counts((1, 2, 1).counts == {1: 2, 2: 1}, "bare tuple")
  check_counts({1, 2}.counts == {1: 1, 2: 1}, "bare set")
  check_counts((1..2).tally == {1: 1, 2: 1}, "bare range")

  calls = []
  rescued = false
  try:
    [1, 2, 3].counts |value|:
      calls.push!(value)
      if value == 2:
        raise ValueError("key failure")
      value
  rescue ValueError:
    rescued = true
  check_counts(rescued and calls == [1, 2], "key exception stops iteration")
  singular = [1, 2, 3].count: $it > 1
  check_counts(singular == 2, "count remains a predicate count")
  42
