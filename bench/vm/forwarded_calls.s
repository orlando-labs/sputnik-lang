package bench.vm.forwarded_calls

export main

# Repeated *args/**options forwarding through two short method activations.
# The target observes the forwarded keyword and positional values.
class Relay:
  def call(*args, **options): forward(*args, **options)

  def forward(*args, **options): target(*args, **options)

  def target(value, extra: 1): value + extra

def main():
  relay = Relay()
  result = 0
  100000.times |index|:
    result = relay.call(index, extra: 2)
  result

main()
