package bench.vm.lookup_constant.main

from bench.vm.lookup_constant.provider import answer, LookupProbe, Particle

export main

def measure(name, iterations, repeats, expected, &block):
  rows = []
  value = null
  (repeats + 1).times |repeat|:
    started = Time.monotonic
    value = block()
    elapsed = (Time.monotonic - started).total_nanoseconds
    rows.push!(elapsed) if repeat > 0
  raise ValueError("lookup result changed") unless value == expected
  print "LOOKUP_ATOM #{Json.generate({name:, iterations:, nanoseconds: rows, valid: true})}"

def main():
  parser = ArgParser(name: "LOOKUP_CONST benchmark")
  parser.arg("--iterations", type: Int, default: 300000)
  parser.arg("--repeats", type: Int, default: 5)
  args = parser.parse_or_raise
  iterations = args["iterations"]
  repeats = args["repeats"]
  raise ValueError("counts must be positive") if iterations <= 0 or repeats <= 0
  callback = answer
  probe = LookupProbe()
  measure("local_function", iterations, repeats, callback): probe.local(iterations, callback)
  measure("qualified_function", iterations, repeats, callback): probe.qualified(iterations)
  measure("sputnik_class", iterations, repeats, Particle): probe.klass(iterations)
  measure("builtin_type", iterations, repeats, Math): probe.builtin(iterations)
  measure("error_class", iterations, repeats, ValueError): probe.error(iterations)
  measure("task_module", iterations, repeats, task): probe.tasks(iterations)
  raise ValueError("imported captures changed") unless callback() == 42
