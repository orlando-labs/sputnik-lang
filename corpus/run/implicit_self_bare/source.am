package conformance.run

mixin RequestAccess:
  def request(): 2

class Controller:
  include RequestAccess

  def params(): 40

class Items < Controller:
  def action():
    params + request

  def shadowed():
    params = 7
    params

def main():
  controller = Items()
  controller.action() + controller.shadowed()
