from sync import Atomic

def rendezvous(counter):
  counter.update: _1 + 1
  spins = 0
  while counter.get < 2 and spins < 20000000:
    spins += 1
  if counter.get == 2:
    1
  else:
    0

def main():
  counter = Atomic.new(0)
  first = task.spawn:
    rendezvous(counter)
  second = task.spawn:
    rendezvous(counter)
  first.wait() + second.wait()
