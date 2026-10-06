package conformance.run
import system
from system import cmd

def probe():
  special = "a b; $(false)"
  text = cmd'printf "%s" #{special}'.output()
  command = system.command("/bin/sh", "-c", "cat; printf diagnostic >&2")
  result = command.capture(input: "payload", timeout: 3.0)
  streamed = command.capture(timeout: 3.0) with:
    stdin |input|:
      input.write_all!("header".bytes())
      input.write_all!("body".bytes())
      7
    stdout |output|:
      chunks = []
      output.each_chunk(size: 3) |chunk|:
        chunks.push!(chunk.to_str())
      chunks.join("")
    stderr |errors|:
      errors.read_all!().to_str()
  process = cmd'printf manual'.spawn()
  manual = process.communicate()
  status = process.wait()
  process.close()
  if text == special and result.stdout.to_str() == "payload" and result.stderr.to_str() == "diagnostic" and result.success?() and streamed.stdin_value == 7 and streamed.stdout_value == "headerbody" and streamed.stderr_value == "diagnostic" and manual.stdout.to_str() == "manual" and status.exit_code == 0:
    42
  else:
    0
