class Request:
  class_method def call(value, &success:, &error: null):
    success(value)

def main():
  Request.call(20) with:
    success |response|:
      response * 2 + 2
    error:
      _1
