package native_private_helpers
export make_hidden, Failure

class Failure < NativeError

class Hidden:
  attr value
  def init(@value)

def make_hidden(value): Hidden(value)
