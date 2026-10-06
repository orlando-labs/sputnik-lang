package conformance.run
from system import cmd

def probe():
  channel = Channel.new(capacity: 0)
  first = task.async:
    cmd'cat'.capture(timeout: 2.0) with:
      stdin |input|:
        input.write_all!(channel.recv())
      stdout |output|:
        output.read_all!().to_str()
  second = task.async:
    channel.send("async payload")
  second.wait()
  result = first.wait()
  if result.stdout_value == "async payload" and result.success?():
    42
  else:
    0
