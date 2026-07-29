package native.higher_order_core

export main

def main():
  score = 0
  factor = 10
  sum = 0
  [1, 2, 3].each |x|:
    sum = sum + x
  if sum == 6:
    score = score + 1
  scaled = [1, 2, 3, 4].map |x|: x * factor
  if scaled[0] == 10 and scaled[3] == 40:
    score = score + 1
  evens = scaled.select |x|: x % 20 == 0
  if evens.count() == 2 and evens[0] == 20 and evens[1] == 40:
    score = score + 1
  odds = scaled.reject |x|: x % 20 == 0
  if odds.count() == 2 and odds[0] == 10 and odds[1] == 30:
    score = score + 1
  first_large = scaled.find |x|: x > 25
  if first_large == 30:
    score = score + 1
  total = scaled.reduce(0) |a, b|: a + b
  if total == 100:
    score = score + 1
  folded = [2, 3, 4].reduce |a, b|: a * b
  if folded == 24:
    score = score + 1
  if [false, 0, null].any?() == true and [true, 1, "x"].all?():
    score = score + 1
  has_two = [1, 2, 3].any? |x|: x == 2
  if has_two:
    score = score + 1
  all_even = [2, 4, 6].all? |x|: x % 2 == 0
  if all_even:
    score = score + 1
  tuple = (1, 2, 3).map |x|: x + 1
  if tuple[0] == 2 and tuple[2] == 4:
    score = score + 1
  range_sum = (1..4).reduce(0) |a, b|: a + b
  if range_sum == 10:
    score = score + 1
  found = Range.new(2, 8, step: 2).find |x|: x > 4
  if found == 6:
    score = score + 1

  filtered = [0, 1, 2, 3].filter_map |x|:
    if x % 2 == 0:
      x * 10
    else:
      null
  flattened_map = [1, 2, 3].flat_map |x|: [x, x * 10]
  if filtered == [0, 20] and flattened_map == [1, 10, 2, 20, 3, 30]:
    score = score + 1

  grouped = [1, 2, 3, 4].group |x|: x % 2
  partitioned = [1, 2, 3, 4].partition |x|: x > 2
  if grouped[0] == [2, 4] and grouped[1] == [1, 3] and partitioned == [[3, 4], [1, 2]]:
    score = score + 1

  index = [4, 8, 12].find_index |x|: x == 8
  taken = [1, 2, 3, 1].take_while |x|: x < 3
  dropped = [1, 2, 3, 1].drop_while |x|: x < 3
  if index == 1 and taken == [1, 2] and dropped == [3, 1]:
    score = score + 1

  counted = [1, 2, 2, 3].count(2)
  counted_block = [1, 2, 3, 4].count |x|: x % 2 == 0
  unique = [1, 2, 3, 4].uniq |x|: x % 2
  if counted == 2 and counted_block == 2 and unique == [1, 2]:
    score = score + 1

  if [1, 2, 3].sum() == 6 and [2, 3, 4].product() == 24 and [1, 2].sum(10) == 13:
    score = score + 1

  nested = [[1, [2]], 3].flattened()
  tally = ["a", "b", "a"].tally()
  if nested == [1, 2, 3] and tally["a"] == 2 and tally["b"] == 1:
    score = score + 1

  pairs = [1, 2, 3].each_pair()
  windows = [1, 2, 3, 4].each_cons(3)
  if pairs == [[1, 2], [2, 3]] and windows == [[1, 2, 3], [2, 3, 4]]:
    score = score + 1

  combinations = [1, 2, 3].combination(2)
  permutations = [1, 2, 3].permutation(2)
  if combinations.count() == 3 and permutations.count() == 6 and combinations[0] == [1, 2]:
    score = score + 1

  collected = [1, 2].collect |x|: x + 10
  selected_alias = [1, 2, 3].find_all |x|: x > 1
  detected = [1, 2, 3].detect |x|: x == 2
  injected = [1, 2, 3].inject(0) |a, b|: a + b
  if collected == [11, 12] and selected_alias == [2, 3] and detected == 2 and injected == 6:
    score = score + 1

  if [1, 2, 3].last() == 3 and [1, 2, 3].last(2) == [2, 3] and [1, 2].to_array() == [1, 2]:
    score = score + 1

  minimum_by_abs = [-5, 2, 3].min: _1.abs
  if [3, 1, 2].min() == 1 and [3, 1, 2].max() == 3 and [3, 1, 2].minmax() == [1, 3] and minimum_by_abs == 2:
    score = score + 1

  all_permutations = [1, 2, 3].permutation()
  consumed_windows = []
  [1, 2, 3].each_cons(2) |window|: consumed_windows.push!(window)
  if all_permutations.count() == 6 and consumed_windows == [[1, 2], [2, 3]]:
    score = score + 1
  score
