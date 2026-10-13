export main

class Source:
  def value(): 42

class OtherSource:
  def unknown_method(): 42

class Forwarder:
  def init(@target)
  def value(): @target.unknown_method

def main(): Forwarder(Source()).value

main()
