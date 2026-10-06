def request(value, &success:, &error: null):
  success(value)

def handler(value):
  value

request(21, error: &handler) with:
  success |response|:
    response * 2
  error:
    _1
