package test.blocking.progress

export main

native def reset(mode) from "blocking.pressure_reset"
native def arrivals(mode) from "blocking.pressure_arrivals"
native def wait_native() from "blocking.pressure_wait" blocking
native def release_native() from "blocking.pressure_release"
native def release_ffi() from "blocking.pressure_release_ffi" blocking
native def batch(callback) from "blocking.batch" blocking

def main():
  parser = ArgParser(name: "blocking progress")
  parser.arg("--case", choices: ["native", "callback", "ffi-producer"], default: "native")
  selected = parser.parse_or_raise["case"]
  mode = if selected == "native" then 0 else if selected == "callback" then 1 else 2
  width = reset(mode)
  channel = Channel.new(capacity: 0)
  readers = Array.of(width):
    task.async:
      if mode == 0:
        wait_native()
      elif mode == 1:
        batch(| |: channel.recv(timeout: 2.0))
      else:
        batch(| |: wait_native())
  started = Time.monotonic
  until arrivals(mode) == width:
    raise ValueError("not all foreign waits started") if (Time.monotonic - started).total_nanoseconds > 3000000000
    task.sleep(1)
  producer = task.async:
    if mode == 0:
      release_native()
    elif mode == 1:
      width.times: channel.send(42, timeout: 2.0)
    else:
      release_ffi()
    true
  readers.each:
    raise ValueError("blocking result") unless $it.wait == 42
  raise ValueError("producer result") unless producer.wait
  print "PASS blocking #{mode} progress with #{width} waits"
  0
