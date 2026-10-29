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

  def missing_observer(&block):
    missing = false
    try:
      block()
    rescue NoMethodError:
      missing = true
    raise ValueError("missing optional observer must raise NoMethodError") unless missing

def main():
  dispatcher = ProtocolDispatcher()
  dispatcher.missing_observer(): null.start_request(null)
  dispatcher.missing_observer(): null.authorized_request(null, null)
  dispatcher.missing_observer(): null.record_error(null, null, null, null)
  dispatcher.missing_observer(): null.finish_exception(null, null, null, null)
  dispatcher.missing_observer(): null.finish_response(null, null, null, null)
  dispatcher.missing_observer(): null.attach_server(null)
  dispatcher.missing_observer(): null.record_shutdown(true, 0, {})
  dispatcher.missing_observer(): null.metrics()
  dispatcher.missing_observer(): null.set_integration_pool(null)
  dispatcher.missing_observer(): null.rand!([2], device: "cpu")
  dispatcher.missing_observer(): null.randn!([2], dtype: "float32", device: "cpu")
  dispatcher.missing_observer(): null.randperm!(2, device: "cpu")
  malformed_url = false
  try:
    Url.percent_decode("%XX")
  rescue UrlDecodeError:
    malformed_url = true
  raise ValueError("malformed percent escape must raise UrlDecodeError") unless malformed_url
  result = "unexpected-open-protocol-result"
  try:
    null.around_controller(1, 2)
  rescue NoMethodError |error|:
    if error.message == "selector `around_controller` is not implemented in current runtime baseline":
      result = "open-protocol-ok"
  if result == "open-protocol-ok":
    if dispatcher.request(ProtocolProvider()) and dispatcher.request(AlternateProvider()) == false:
      result = "unexpected-user-protocol-result"
      try:
        MissingProtocol().request()
      rescue NoMethodError:
        result = "native-open-protocol-ok"
    else:
      result = "unexpected-polymorphic-cache-result"
  result
