package native_private_classes
from native_private_helpers import make_hidden, Failure
export main

class Hidden:
  def init(@value)
  def value(): @value * 2

def main():
  if Hidden(7).value != 14 or make_hidden(7).value != 7:
    raise ValueError("private classes lost their module identity")
  caught = false
  try:
    raise Failure("declared native error")
  rescue NativeError |error|:
    caught = true
  if not caught: raise ValueError("native error class became an ordinary class")
  print "PASS native private classes"
  0
