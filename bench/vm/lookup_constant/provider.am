package bench.vm.lookup_constant.provider

export answer, LookupProbe, Particle

captured = 42

def answer(): captured

class Particle:
  def answer(): 42

# A method has no lexical capture of the module function: `answer` lowers to
# LOOKUP_CONST bench.vm.lookup_constant.provider.answer on every loop iteration.
class LookupProbe:
  def local(iterations, value):
    result = null
    i = 0
    while i < iterations:
      result = value
      i = i + 1
    result

  def qualified(iterations):
    result = null
    i = 0
    while i < iterations:
      result = answer
      i = i + 1
    result

  def klass(iterations):
    result = null
    i = 0
    while i < iterations:
      result = Particle
      i = i + 1
    result

  def builtin(iterations):
    result = null
    i = 0
    while i < iterations:
      result = Math
      i = i + 1
    result

  def error(iterations):
    result = null
    i = 0
    while i < iterations:
      result = ValueError
      i = i + 1
    result

  def tasks(iterations):
    result = null
    i = 0
    while i < iterations:
      result = task
      i = i + 1
    result
