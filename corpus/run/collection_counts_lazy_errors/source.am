def probe():
  lazy_counts = [1, 2, 3, 4].lazy.counts: $it % 2
  lazy_tally = (1..4).lazy.tally: $it % 2
  lazy_bare = ["a", "b", "a"].lazy.counts
  bounded = (1..).lazy.take(3).counts: $it % 2
  empty = [].lazy.tally:
    raise ValueError("empty lazy tally called its block")
  lazy_ok = lazy_counts == {1: 2, 0: 2} and lazy_tally == lazy_counts and lazy_bare == {"a": 2, "b": 1} and bounded == {1: 2, 0: 1} and empty == {}

  errors = 0
  try:
    [1, 2].tally:
      raise ValueError("tally key failure")
  rescue ValueError:
    errors += 1

  if lazy_ok and errors == 1:
    42
  else:
    0
