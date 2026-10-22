package test.native_system_spread
export main

def main():
  child = task.spawn:
    arguments = ["%s:%s", "hello", "world"]
    options = {check: false}
    result = system.command("/usr/bin/printf", *arguments).capture(**options)
    raise ValueError("spread command failed") unless result.success? and result.stdout.to_str == "hello:world"
    42
  raise ValueError("lost task context") unless child.wait() == 42
  print("PASS native system spread")
  0
