package conformance.run

class Collection:
  def init(@count)

  def size(scale: 1):
    @count * scale

class Build:
  class_method def version(value = 20):
    value

class Counter:
  attr value

  def init():
    @value = 0

  def increment!(amount: 1):
    @value += amount

def probe():
  items = Collection(40)
  counter = Counter()
  counter.increment!
  items.size + items.size() + Build.version + Build.version() + counter.value
