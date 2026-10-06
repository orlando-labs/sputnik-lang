package native.set_core

export main

def main():
  score = 0

  normalized = Set{1, 1.0, [1, 2], (1, 2), 3}
  if normalized.count() == 3 and normalized.contains?(1.0) and normalized.contains?((1, 2)) and normalized.contains?(3):
    score = score + 1

  hashy = HashSet{1, 1.0, 2}
  if hashy.count() == 2 and hashy.contains?(1) and hashy.contains?(2):
    score = score + 1

  spread = Set{0, *[1, 2], *Set{2, 3}}
  if spread.count() == 4 and spread.contains?(0) and spread.contains?(3):
    score = score + 1

  base = Set{1, 2, 3}
  added = base.added(4)
  deleted = base.deleted(2)
  if base.count() == 3 and added.count() == 4 and added.contains?(4) and deleted.count() == 2 and deleted.contains?(2) == false and deleted.to_str().starts_with?("Set{"):
    score = score + 1

  other = Set{3, 4}
  unioned = base.union(other)
  common = base.intersection(Set{2, 3, 5})
  only_base = base.difference(Set{2, 4})
  if unioned.count() == 4 and unioned.contains?(4) and common.count() == 2 and common.contains?(2) and only_base.count() == 2 and only_base.contains?(1):
    score = score + 1

  symmetric = base.symmetric_difference(Set{2, 3, 4})
  if symmetric.count() == 2 and symmetric.contains?(1) and symmetric.contains?(4) and symmetric.contains?(2) == false:
    score = score + 1

  small = Set{1, 2}
  if small.subset?(base) and small.proper_subset?(base) and base.superset?(small) and base.proper_superset?(small):
    score = score + 1

  if base.disjoint?(Set{8, 9}) and base.disjoint?(Set{3, 9}) == false:
    score = score + 1

  mutated = Set{1, 2}
  mutated.add!(3)
  mutated.subtract!(Set{1, 4})
  if mutated.count() == 2 and mutated.contains?(2) and mutated.contains?(3) and mutated.contains?(1) == false:
    score = score + 1

  changed = Set{1, 2, 3, 4}
  changed_alias = changed
  changed.delete!(1)
  changed.merge!(Set{4, 5})
  changed.select!: _1 >= 3
  changed.reject!: _1 == 4
  changed.delete_if!: _1 == 99
  changed.keep_if!: _1 <= 5
  changed.replace!(Set{7, 8})
  if changed_alias.count() == 2 and changed_alias.contains?(7) and changed_alias.contains?(8):
    score = score + 1

  filtered = Set{1, 2, 3, 4}
  filtered.filter_map! |value|:
    if value % 2 == 0:
      value * 10
    else:
      null
  filtered.clear!()
  if filtered.empty?():
    score = score + 1

  operator_union = Set{1, 2} | Set{2, 3}
  operator_common = Set{1, 2, 3} & Set{2, 4}
  operator_difference = Set{1, 2, 3} - Set{2}
  operator_symmetric = Set{1, 2} ^ Set{2, 3}
  if operator_union == Set{1, 2, 3} and operator_common == Set{2} and operator_difference == Set{1, 3} and operator_symmetric == Set{1, 3} and Set{1, 2} < Set{1, 2, 3} and Set{1, 2, 3} >= Set{2, 1}:
    score = score + 1

  score
