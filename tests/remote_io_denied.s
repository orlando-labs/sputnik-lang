package test.remote_io_denied
export main

def main():
  parser = ArgParser(name: "IO denial check")
  parser.arg("--operation", required: true)
  operation = parser.parse_or_raise()["operation"]
  case operation:
    when "atomic": fs.atomic_write_text("/tmp/sp-must-not-exist", "denied")
    when "sync": fs.sync("/tmp")
    when "entries": fs.entries("/tmp")
    when "lock": fs.lock("/tmp/sp-must-not-exist.lock")
    when "listen": net.unix.listen("/tmp/sp-must-not-exist.sock")
    when "connect": net.unix.connect("/tmp/sp-must-not-exist.sock")
  raise ValueError("operation must have been denied")
