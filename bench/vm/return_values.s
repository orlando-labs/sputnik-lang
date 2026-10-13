class Relay:
  def pass(value):
    if value:
      value
    else:
      null

  def relay(value): pass(value)

relay = Relay()
value = [7, 11]
result = null
300000.times:
  result = relay.relay(value)
result[0] + result[1]
