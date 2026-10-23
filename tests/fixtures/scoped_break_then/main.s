export main

first = 20 then second = 22
def module_sum(): first + second

def check(value): raise ValueError("scoped break / then regression") unless value

def scope(&blk):
  blk()
  -1

def forward(&blk): scope(&blk)

def saved(&blk): blk

def identity(x): x

def scope_arg(value, &blk):
  check(value == 68)
  blk()
  -1

def retain_and_fail(holder, &blk):
  holder.push!(blk)
  raise ValueError("intentional failure")

def named_scope(&action:):
  action()
  -1

def recurse(depth, events):
  result = scope():
    if depth > 0:
      check(recurse(depth - 1, events) == depth - 1)
    break depth
  events.push!(result)
  result

def return_owner(events):
  scope():
    try:
      break 53
    ensure:
      events.push!(54)
      return 55
  -1

def main():
  check(module_sum() == 42)
  check((68 then scope_arg($_): break 69) == 69)
  check((false then 42) == 42)
  check((null then 43) == 43)
  a = 1 then b = 2
  check(a == 1 and b == 2)
  check((10 then $_ + 2 then $_ * 3) == 36)
  events = []
  events.push!(1) then events.push!(2) if false
  events.push!(3) then events.push!(4) unless true
  check(events == [])
  events.push!(5) then events.push!(6) if true
  check(events == [5, 6])
  check(identity(1 then $_ + 1) == 2)
  (left, right) = (1, 2) then check(left == 1 and right == 2)
  a = 0
  identity(null) then a = 5
  check(a == 5)
  events.push!(0) then events .push!(1) if false
  check(events == [5, 6])
  check(((if true then 10 else 20) then $_ + 1) == 11)
  check((if true then (20 then $_ + 2) else 0) == 22)
  check((scope(): break 42) == 42)
  check((scope(): break) == null)
  result = forward():
    events.push!(7) then break 44 if true
    raise ValueError("unreachable after break")
  check(result == 44)
  events = []
  result = scope():
    try:
      try:
        break 45
      rescue:
        events.push!(99)
      ensure:
        events.push!(1)
    ensure:
      events.push!(2)
  check(result == 45 and events == [1, 2])
  result = scope():
    check((loop: break 3) == 3)
    break 46
  check(result == 46)
  events = []
  check(recurse(3, events) == 3)
  check(events == [0, 1, 2, 3])
  check(return_owner(events) == 55 and events == [0, 1, 2, 3, 54])
  block = saved(): break 47
  expired = false
  try:
    block()
  rescue LocalJumpError:
    expired = true
  check(expired)
  # A fresh accepting invocation does not retarget a previously saved block.
  expired = false
  try:
    scope(&block)
  rescue LocalJumpError:
    expired = true
  check(expired)
  result = scope():
    inner = scope(): break 48
    check(inner == 48)
    break 49
  check(result == 49)
  # next completes only this block invocation; the accepting method continues.
  check((scope(): next 50) == -1)
  check((scope(): false then next 51) == -1)
  holder = []
  try:
    retain_and_fail(holder): break 60
  rescue ValueError:
    null
  expired = false
  try:
    holder[0]()
  rescue LocalJumpError:
    expired = true
  check(expired)
  result = named_scope() with:
    action: break 61
  check(result == 61)
  result = scope(): false then break 63 if false
  check(result == -1)
  events = []
  result = [1, 2, 3].for_each |x|:
    events.push!(x) then break if x == 2
  check(result == null and events == [2])
  mutex = Mutex.new()
  result = mutex.synchronize(): break 66
  check(result == 66)
  check((mutex.synchronize(): 67) == 67)
  worker = task.spawn: break 62
  foreign_task = false
  try:
    worker.wait()
  rescue LocalJumpError:
    foreign_task = true
  check(foreign_task)
  print "PASS scoped break and then"

main()
