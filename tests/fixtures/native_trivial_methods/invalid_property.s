export main

class PropertySource:
  prop value: 42

class Forwarder:
  def init(@target)
  def explicit(): @target.value()

def main(): Forwarder(PropertySource()).explicit

main()
