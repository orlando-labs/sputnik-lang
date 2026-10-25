# Dynamic throw must exit a large lazy counter and execute ensure in both backends.
finalized = []
stopped = catch(:done):
  9223372036854775807.times |i|:
    try:
      throw :done, i
    ensure:
      finalized.push!(i)
if stopped != 0 or finalized != [0]:
  raise ValueError("times throw/ensure regression")
print "PASS times counter throw and ensure"
