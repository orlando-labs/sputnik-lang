package test.super_base
export Parent

class Parent:
  attr value
  def init(@value)
  def adjusted(offset: 1): @value + offset
