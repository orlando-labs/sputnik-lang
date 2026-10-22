# catch/throw currently uses the VM backend; keep this regression separate
# from the fixture that requires complete native coverage.
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
