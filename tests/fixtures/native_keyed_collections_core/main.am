package native.keyed_collections_core

export main

def main():
  score = 0

  names = {"user_id": 1, user_id: 2}
  if names.count() == 1 and names["user_id"] == 2 and names[:user_id] == 2 and names.keys()[0] == "user_id":
    score = score + 1

  strict = StrictMap{name: 1, "name": 2}
  if strict.count() == 2 and strict[:name] == 1 and strict["name"] == 2 and strict.has_key?(:name) and strict.has_key?("name"):
    score = score + 1

  strict_hash = StrictHashMap{name: 1, "name": 2}
  if strict_hash.count() == 2 and strict_hash[:name] == 1 and strict_hash["name"] == 2:
    score = score + 1

  composite = {1: 10, 1.0: 20, (1, 2): 30, [1, 2]: 40, (1..3): 50}
  if composite.count() == 3 and composite[1] == 20 and composite[1.0] == 20 and composite[(1, 2)] == 40 and composite[[1, 2]] == 40 and composite[(1..3)] == 50:
    score = score + 1

  base = {a: 1, b: 2, c: 2}
  merged = base.merge({"b": 20, d: 4})
  if merged.count() == 4 and merged[:b] == 20 and merged["d"] == 4 and merged.keys().count() == 4 and merged.values().contains?(20):
    score = score + 1

  entry = merged.entries()[1]
  if entry[0] == :b and entry[1] == 20:
    score = score + 1

  picked = merged.slice("b", :d)
  if picked.count() == 2 and picked[:b] == 20 and picked["d"] == 4:
    score = score + 1

  changed = merged.with(:e, 5).except("a", :c)
  if changed.count() == 3 and changed[:e] == 5 and changed.has_key?(:a) == false:
    score = score + 1

  compacted = {a: 1, b: 2, c: null}.compact()
  if compacted.count() == 2 and compacted.has_key?(:c) == false:
    score = score + 1

  doubled = merged.transform_values |v, k|: v * 2
  if doubled[:a] == 2 and doubled[:b] == 40 and doubled[:d] == 8:
    score = score + 1

  rekeyed = {1: 10, 2: 20}.transform_keys |k, v|: k * 10
  if rekeyed[10] == 10 and rekeyed[20] == 20:
    score = score + 1

  filtered = merged.select |k, v|: v > 2
  if filtered.count() == 2 and filtered[:b] == 20 and filtered[:d] == 4:
    score = score + 1

  stored = {a: 1}
  stored_result = stored.store!(:b, 2)
  stored_result.store!(:c, 3)
  if stored[:b] == 2 and stored[:c] == 3:
    score = score + 1

  frozen_map = {a: 1}
  frozen_map_result = frozen_map.freeze()
  map_frozen = false
  try:
    frozen_map.store!(:b, 2)
  rescue FrozenError |error|:
    map_frozen = error.message == "cannot modify frozen map"
  mutable_map_copy = frozen_map.copy()
  mutable_map_copy.store!(:b, 2)
  if frozen_map_result[:a] == 1 and map_frozen and frozen_map.has_key?(:b) == false and mutable_map_copy[:b] == 2:
    score = score + 1

  frozen_list = [1]
  frozen_list_result = frozen_list.freeze()
  list_frozen = false
  try:
    frozen_list.push!(2)
  rescue FrozenError |error|:
    list_frozen = error.message == "cannot modify frozen list"
  mutable_list_copy = frozen_list.copy()
  mutable_list_copy.push!(2)
  if frozen_list_result[0] == 1 and list_frozen and frozen_list.count() == 1 and mutable_list_copy.count() == 2:
    score = score + 1

  frozen_set = Set{1}
  frozen_set_result = frozen_set.freeze()
  set_frozen = false
  try:
    frozen_set.add!(2)
  rescue FrozenError |error|:
    set_frozen = error.message == "cannot modify frozen set"
  mutable_set_copy = frozen_set.copy()
  mutable_set_copy.add!(2)
  if frozen_set_result.contains?(1) and set_frozen and frozen_set.contains?(2) == false and mutable_set_copy.contains?(2):
    score = score + 1

  frozen_tuple = (1, 2)
  frozen_tuple_result = frozen_tuple.freeze()
  if frozen_tuple_result.count() == 2 and frozen_tuple_result[0] == 1:
    score = score + 1

  indexed = []
  [10, 20].each_with_index |value, index|:
    indexed.push!(value + index)
  if indexed[0] == 10 and indexed[1] == 21:
    score = score + 1

  indexed_pairs = [10, 20].each_with_index()
  if indexed_pairs[0][0] == 10 and indexed_pairs[0][1] == 0 and indexed_pairs[1][0] == 20 and indexed_pairs[1][1] == 1:
    score = score + 1

  deleted_map = {name: 1, keep: 2, absent: null}
  deleted_alias = deleted_map
  removed_name = deleted_map.delete!("name")
  missing_value = deleted_map.delete!(:missing)
  deleted_map.compact!()
  deleted_map.merge!({extra: 3})
  deleted_map.update!({keep: 20})
  deleted_map.select! |key, value|: value >= 3
  deleted_map.transform_values! |value, key|: value * 2
  deleted_map.transform_keys! |key, value|: key.to_str.upcase
  shifted = deleted_map.shift!()
  deleted_map.replace!({done: 42})
  if removed_name == 1 and missing_value == null and deleted_alias[:done] == 42 and shifted[1] >= 6:
    score = score + 1

  cleared_map = {a: 1, b: 2}
  cleared_map.reject! |key, value|: value == 1
  cleared_map.keep_if! |key, value|: value == 2
  cleared_map.delete_if! |key, value|: false
  cleared_map.clear!()
  if cleared_map.empty?():
    score = score + 1

  nested_map = {a: {b: [10, {c: 20}]}}
  fetched = nested_map.fetch(:a)
  fallback = nested_map.fetch(:missing, 99)
  if fetched[:b][0] == 10 and fallback == 99 and nested_map.dig(:a, :b, 1, :c) == 20 and nested_map.dig(:missing) == null:
    score = score + 1

  mapped_values = {a: 1, b: 2, c: 3}.filter_map |key, value|:
    if value > 1:
      value * 10
    else:
      null
  transformed_pairs = {a: 1, b: 2}.transform |key, value|: [key.to_str.upcase, value * 2]
  selected_keys = {a: 1, b: 2}.deconstruct_keys([:b])
  if mapped_values == [20, 30] and transformed_pairs["A"] == 2 and transformed_pairs["B"] == 4 and selected_keys[:b] == 2 and selected_keys.count() == 1:
    score = score + 1

  plus_merged = {a: 1} + {b: 2}
  union_merged = {a: 1} | {a: 3, c: 4}
  resolved_merge = {a: 2}.merge({a: 5}) |key, old_value, new_value|: old_value + new_value
  if plus_merged[:b] == 2 and union_merged[:a] == 3 and union_merged[:c] == 4 and resolved_merge[:a] == 7:
    score = score + 1

  pair_rows = {a: 1, b: 2}.each_pair()
  pair_total = 0
  {a: 1, b: 2}.each_pair |key, value|:
    pair_total = pair_total + value
  if pair_rows == [(:a, 1), (:b, 2)] and pair_total == 3:
    score = score + 1

  path_payload = Json.parse("{\"items\":[{\"id\":1},{\"id\":2},{\"id\":3}]}")
  path_ids = Json.paths(path_payload, "$.items[*].id")
  path_first = path_payload.path("$.items[*].id")
  path_fold = path_payload.paths("$.items[*]", 0) |element, accumulator|:
    if element[:id] == 2:
      Json.stop(accumulator + 20)
    accumulator + element[:id]
  strict_path = StrictMap{name: 1, "name": 2}.path("$.name")
  if path_ids == [1, 2, 3] and path_first == 1 and path_fold == 21 and strict_path == 2:
    score = score + 1

  score
