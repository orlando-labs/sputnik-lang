found = ((0..).lazy.select |x|: x % 2 == 0).find_with_index.with_index |x, i|: i == 3
if found != (6, 3): raise ValueError("lazy indexed search regression")
print "PASS lazy indexed search"
seen = []
for x with_index i in (0.0..1.0:0.5):
  seen.push!((x, i))
if seen != [(0.0, 0), (0.5, 1), (1.0, 2)]:
  raise ValueError("float range iteration regression")

def check(value): raise ValueError("effect unwind regression") unless value

result = catch(:done):
  [1, 2].for_each |x|:
    throw :done, x + 40
check(result == 41)
result = catch(:outer):
  catch(:other):
    throw :outer, 42
check(result == 42)
finalized = []
found = catch(:replace_break):
  for x in [1]:
    try:
      try:
        break
      ensure:
        throw :replace_break, 44
    ensure:
      finalized.push!(45)
check(found == 44 and finalized == [45])
