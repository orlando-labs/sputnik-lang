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
  parser.arg("--case", choices: ["native", "callback", "ffi-producer", "ffi-overload", "ffi-sequential"], default: "native")
  selected = parser.parse_or_raise["case"]
  if selected == "ffi-sequential":
    256.times:
      raise ValueError("sequential FFI result") unless release_ffi()
    print "PASS sequential FFI calls at a one-thread limit"
    return 0
  mode = if selected == "native" then 0 else if selected == "callback" then 1 else if selected == "ffi-producer" then 2 else 3
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
    elif mode == 2:
      release_ffi()
    else:
      rejected = false
      cleaned = false
      try:
        release_ffi()
      rescue ResourceBusyError |error|:
        rejected = error.message.contains?("pool is full")
      ensure:
        cleaned = true
        release_native()
      raise ValueError("full FFI pool must raise a catchable error and run ensure") unless rejected and cleaned
    true
  readers.each:
    raise ValueError("blocking result") unless $it.wait == 42
  raise ValueError("producer result") unless producer.wait
  release_ffi() if mode == 3
  print "PASS blocking #{mode} progress with #{width} waits"
  0
