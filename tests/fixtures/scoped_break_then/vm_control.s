def scope(&blk):
  blk()
  -1

finalized = []
result = catch(:replacement):
  scope():
    try:
      try:
        break 42
      ensure:
        throw :replacement, 43
    ensure:
      finalized.push!(44)
if result != 43 or finalized != [44]:
  raise ValueError("ensure must replace scoped break and finish unwinding")

result = catch(:outer):
  scope():
    throw :outer, 45
if result != 45:
  raise ValueError("source throw must cross a scoped accepting call")
print "PASS scoped break replacement"
