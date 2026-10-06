package native.implicit_self_cli.main

from native.implicit_self_cli.framework import Controller

export main

class Items < Controller:
  def action():
    params + request

def main():
  parser = ArgParser()
  parser.arg("--count", type: Int, required: true)
  Items().action() + parser.parse_or_raise()["count"]
