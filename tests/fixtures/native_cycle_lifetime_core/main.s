package native.cycle_lifetime_core

from sync import Atomic, Mutex

export main

class CycleNode:
  def init():
    @next = null

  def link(value):
    @next = value

def discard_cycle(seed):
  cycle = [
    seed, seed, seed, seed, seed, seed, seed, seed,
    seed, seed, seed, seed, seed, seed, seed, seed,
    seed, seed, seed, seed, seed, seed, seed, seed,
    seed, seed, seed, seed, seed, seed, seed, seed,
    seed, seed, seed, seed, seed, seed, seed, seed,
    seed, seed, seed, seed, seed, seed, seed, seed,
    seed, seed, seed, seed, seed, seed, seed, seed,
    seed, seed, seed, seed, seed, seed, seed, seed,
  ]
  cycle.push!(cycle)

  # Exercise lazy candidate discovery beyond the one-node list fast case.
  # These are deliberately sparse so the fixture remains primarily a
  # high-volume RSS stress test.
  if seed % 1024 == 0:
    tail = []
    middle = [tail]
    root = [middle]
    tail.push!(root)

  if seed % 2048 == 0:
    callback = null
    callback = |ignored|: callback

  if seed % 4096 == 0:
    node = CycleNode()
    node.link(node)

  if seed % 8192 == 0:
    entries = {}
    entries.store!(:self, entries)
    atomic = Atomic.new(null)
    atomic.set(atomic)

  if seed % 16384 == 0:
    copy = cycle.deep_copy
    copy.push!(copy)

  null

def discard_cycles(offset, count):
  index = 0
  while index < count:
    discard_cycle(offset + index)
    index += 1
  count

class LockedCycles:
  def init():
    @mutex = Mutex.new()

  def discard(offset, count):
    @mutex.synchronize:
      discard_cycles(offset, count)

def main():
  locked = LockedCycles()
  first = task.spawn:
    locked.discard(0, 5000)
  second = task.spawn:
    locked.discard(5000, 5000)
  third = task.spawn:
    discard_cycles(10000, 145000)
  fourth = task.spawn:
    discard_cycles(155000, 145000)
  first.wait() + second.wait() + third.wait() + fourth.wait()
