package native.open_protocol_core

export main

class ProtocolProvider:
  def request():
    true

class AlternateProvider:
  def request():
    false

class MissingProtocol:
  def marker():
    true

class ProtocolDispatcher:
  def request(provider):
    provider.request()

def main():
  result = "unexpected-open-protocol-result"
  try:
    null.around_controller(1, 2)
  rescue NoMethodError |error|:
    if error.message == "selector `around_controller` is not implemented in current runtime baseline":
      result = "open-protocol-ok"
  if result == "open-protocol-ok":
    dispatcher = ProtocolDispatcher()
    if dispatcher.request(ProtocolProvider()) and dispatcher.request(AlternateProvider()) == false:
      result = "unexpected-user-protocol-result"
      try:
        MissingProtocol().request()
      rescue NoMethodError:
        result = "native-open-protocol-ok"
    else:
      result = "unexpected-polymorphic-cache-result"
  result
