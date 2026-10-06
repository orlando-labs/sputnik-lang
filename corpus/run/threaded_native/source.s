package conformance.run

from sync import Channel

def require(value):
  if not value:
    raise ValueError.new("threaded native regression")

def probe():
  [:atomic, :dynamic, :chunks, :fixed, :stride, :items].each |policy|:
    items = [3, 1, 4, 2]
    parallel = items.threaded(3, scatter: policy)
    require((parallel.map |value, index|: value * 10 + index) == [30, 11, 42, 23])
    require((parallel.select: $it % 2 == 0) == [4, 2])
    require((parallel.reject: $it % 2 == 0) == [3, 1])
    require((parallel.filter_map: if $it % 2 == 0: $it * 2 else: null) == [8, 4])
    require((parallel.flat_map: [$it, $it + 1]) == [3, 4, 1, 2, 4, 5, 2, 3])
    require((parallel.each: [$it]) == items)
    require((items.parallel(2, scatter: policy).collect: $it * 2) == [6, 2, 8, 4])
    require((items.threaded(2, scatter: policy).filter: $it > 2) == [3, 4])
    require((items.threaded(2, scatter: policy).collect_concat: ($it, $it)) == [3, 3, 1, 1, 4, 4, 2, 2])
  require(([].threaded().map: 1 / 0) == [])
  require((3.times.threaded(0).map: $it) == [0, 1, 2])
  require(((1, 2, 3).threaded(2).map: $it + 1) == [2, 3, 4])
  require(({1, 2, 3}.threaded(2).map: $it * 2).sorted == [2, 4, 6])
  require([1, 2, 3].threaded(2).combination(2) == [[1, 2], [1, 3], [2, 3]])
  require([1, 2].threaded(2).permutation(2) == [[1, 2], [2, 1]])
  require([1, 2].threaded(2).combination(0) == [[]])
  require([1, 2].threaded(2).combination(3) == [])
  seed = 40
  detached = [1, 2, 3].threaded(1).map:
    seed += 1
    seed
  require(detached == [41, 42, 43] and seed == 43)
  nested = [1, 2, 3].threaded(2).map |outer|:
    ([4, 5].threaded(2).map |inner|: outer + inner).sum
  require(nested == [11, 13, 15])
  gate = Channel.new(capacity: 0)
  rendezvous = [1, 2].threaded(2).map |item|:
    if item == 1:
      gate.send(40, timeout: 2.0)
      1
    else:
      gate.recv(timeout: 2.0)
  require(rendezvous == [1, 40])
  context = task.local(inherit: true)
  context.set!(40)
  require(([1, 2].threaded(2).map: context.get() + $it) == [41, 42])
  require(context.get() == 40)
  shared = "start"
  300.times.threaded(8).each |index|:
    shared = "value#{index}"
    64.times:
      cycle = []
      cycle.push!(cycle)
  require(shared.size >= 6)
  frozen = (7, 8)
  require(([frozen].threaded(1).map: $it)[0] == frozen)
  buffer = io.Buffer.new
  print("native", {answer: 42}, to: buffer)
  require(buffer.to_str == "native\n{answer: 42}\n")
  42
