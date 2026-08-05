def request(value, mode:, &success:, &error: null):
  success(value)

request(21, mode: :fast) with:
  success |response|:
    response * 2
  error:
    _1
