from system import cmd

macro def _text_parts(parts):
  result = []
  parts.each |part|:
    if part.kind == "AstStringFor":
      result.push!(Ast.node("AstStringFor", {iteration: part.iteration, parts: _text_parts(part.parts)}))
    elif part.kind == "AstStringExpr":
      value = quote:
        Sputnik.stringify(#{part.expr})
      result.push!(value)
    else:
      result.push!(Ast.lift(part.value))
  return result

string_tag macro def text(t as Ast.StringTemplate):
  finish = quote:
    finish_text
  return Ast.node("AstTemplateRender", {parts: _text_parts(t.parts), finish: finish})

def finish_text(parts): parts.join("")

def check(value, message):
  raise ValueError("template loops: " + message) unless value

def items(calls):
  calls.push!("source")
  ["<a>", "b & c"]

def render(calls):
  text"""
    start
      %for value with_index i in items(calls):
        #{i}:#{value}
    end
    """

calls = []
__string_template_parts = "caller"
result = render(calls)
check(result == "start\n  0:<a>\n  1:b & c\nend\n", "runtime iteration, whitespace and index: " + result)
check(calls == ["source"] and __string_template_parts == "caller", "once-only source and hygienic builder")
nested = text"""
  %for n in [1, 2]:
    %for value in ["a", "b"]:
      #{n}:#{value}
  """
check(nested == "1:a\n1:b\n2:a\n2:b\n", "nested loops")
empty = text"""
  before
  %for item in []:
    #{item}
  after
  """
check(empty == "before\nafter\n", "empty loop emits nothing")
pairs = text"""
  %for (left, right) in [(1, 2), (3, 4)]:
    #{left}:#{right}
  """
check(pairs == "1:2\n3:4\n", "block parameter patterns")
mapping = text"""
  %for key, value with_index i in {a: 10, b: 20}:
    #{i}:#{key}:#{value}
  """
check(mapping == "0:a:10\n1:b:20\n", "Map argument shape")
comments = text"""
  %%for is literal
  %# invisible
  done
  """
check(comments == "%for is literal\ndone\n", "literal percent and comments")
plain = """
  %for value in [1, 2]:
    ordinary text
  """
check(plain.contains?("%for"), "ordinary strings stay value-only")
command = cmd"""
  echo
  %for value in ["a b", "x;$(evil)", "", "%for fake:"]:
    --item=#{value}
  """
check(command.argv == ["echo", "--item=a b", "--item=x;$(evil)", "--item=", "--item=%for fake:"], "one command, opaque interpolants")
no_args = cmd"""
  echo
  %for value in []:
    #{value}
  """
check(no_args.argv == ["echo"], "empty command argument loop")
suffix = "runtime"
header = text"""
  %for value in ["#{suffix}"]:
    #{value}
  """
check(header == "runtime\n", "ordinary interpolated string expression in header")
escaped_space = text"""
  %for value in [1]:
    \u{20}#{value}
  """
check(escaped_space == " 1\n", "escaped indentation is data")
escaped_percent = text"""
  \u{25}for is data
  %for value in [1]:
    #{value}
  """
check(escaped_percent == "%for is data\n1\n", "escaped percent is not a directive")
comment_only = text"""
  %for value in [1, 2]:
    %# no output
  """
check(comment_only == "", "comment-only loop body")
def last_value():
  "prior"
  text"#{$_}:#{$_}"
check(last_value() == "prior:prior", "accumulator does not overwrite caller last value")

def return_from_template():
  text"""
    %for value in [1, 2]:
      #{return "returned"}
    """
  raise ValueError("return was captured by template scaffolding")
check(return_from_template() == "returned", "interpolant return keeps its lexical target")
header_escapes = text"""
  %for value in ["a\nb", "a\"b", "\u{41}"]:
    #{value}
  """
check(header_escapes == "a\nb\na\"b\nA\n", "quoted header expressions preserve escape spelling")
print "PASS string template loops"
