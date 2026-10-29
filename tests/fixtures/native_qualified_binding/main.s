package native.qualified_binding

from net.http import Client as HttpClient

export main

class Client:
  def init(root): null

def with_block(&block): block()
def finish(value): value

def tuple_result():
  with_block:
    99
  (3, 4)

def bare_result():
  with_block:
    99
  finish 7

def main():
  client = HttpClient(timeout: 1.0, redirects: :off)
  raise ValueError("qualified native type resolved to a local class") unless HttpClient === client
  client.close!
  raise ValueError("tuple after block became call arguments") unless tuple_result() == (3, 4)
  raise ValueError("bare call after block became call arguments") unless bare_result() == 7
  print "PASS qualified binding and call line boundaries"
