export main

def check(value): raise ValueError("missing-selector unwinding") unless value

def result_error?(value):
  try:
    value.err?
  rescue NoMethodError:
    false

def main():
  events = []
  for value in [null, 42, :plain, [], {}]:
    caught = false
    try:
      value.err?
    rescue NoMethodError:
      caught = true
    ensure:
      events.push!(1)
    check(caught)
    caught = false
    try:
      value.err?()
    rescue NoMethodError:
      caught = true
    ensure:
      events.push!(2)
    check(caught)
    check(not result_error?(value))
  check(events.count == 10)
  check(result_error?(Err("abort")))
  check(not result_error?(Ok(42)))
  print "PASS rescuable missing selector"
  0

main()
