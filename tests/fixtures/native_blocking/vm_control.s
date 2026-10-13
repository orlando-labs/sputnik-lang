package test.blocking.vm_control

from test.blocking.bindings import batch, worker, inline_worker

export main

def main():
  child = task.spawn:
    caught = catch(:done): batch(| |: throw :done, 9)
    raise ValueError("callback throw") unless caught == 9
    raise ValueError("worker state after throw") unless worker() and not inline_worker()
    true
  raise ValueError("child completion") unless child.wait()
  print "PASS blocking callback throw"
  0
