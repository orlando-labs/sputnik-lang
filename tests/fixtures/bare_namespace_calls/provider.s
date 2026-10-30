package bare_namespace_provider

export answer, call_count, defaults, rest, required, factory, invoke, Box

calls = []

def answer():
  calls.push!(1)
  42

def call_count(): calls.count
def defaults(value = 41, offset: 1): value + offset
def rest(*values, **keywords, &block): values.count + keywords.count + (if &block == null then 42 else block())
def required(value): value
def factory(): &answer
def invoke(&block): block()

class Box:
  class_method def build(): Box()
  def answer(): 9
  def through_self(): answer
