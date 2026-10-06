package conformance.run

class Counter:
  attr count

  def init(): @count = 0

  def tick!():
    @count += 1
    @count

  def value(x = tick!, y: x + 1): x + y

  def empty(*xs, **kw, &block):
    xs.count + kw.count + (if block == null then 1 else 0)

  def rest_defaults(x = 1, *xs, y = 2): x + xs.count + y

class Box < Counter:
  def via_self(): value

  def required(x:): x

  class_method def answer(x: 42): x

def probe():
  box = Box()
  values = []
  8.times: values.push!(box.value)
  ordinary = box.value()
  implicit = box.via_self
  (values == [3, 5, 7, 9, 11, 13, 15, 17] and ordinary == 19 and
    implicit == 21 and box.count == 10 and Box.answer == 42 and
    box.empty == 1 and box.rest_defaults == 3 and box.?.value == 23)
