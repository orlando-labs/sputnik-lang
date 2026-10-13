package bench.vm.send_arguments

export main

# Generic SEND with positional and keyword values. Values and the target stay
# tiny so the benchmark exposes call-buffer and binding overhead.
class KernelStub:
  def apply(a, b, c, scale:, bias:): (a + b + c) * scale + bias

def main():
  target = KernelStub()
  result = 0
  100000.times |index|:
    result = target.apply(index, 2, 3, scale: 2, bias: 1)
  result

main()
