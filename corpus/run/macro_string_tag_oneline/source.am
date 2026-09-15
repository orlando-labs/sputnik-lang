package conformance.run

string_tag macro def template(t as Ast.StringTemplate):
  text = ""
  holes = []
  t.parts.each |part|:
    if part.kind == "AstStringText":
      text = text + part.value
    elif part.kind == "AstStringEscape":
      text = text + part.value
    else:
      text = text + "?"
      holes.push!(part.expr)
  return Ast.node("AstTupleLiteral", {
    elements: [
      Ast.lift(t.quote_kind), Ast.lift(text),
      Ast.node("AstListLiteral", {elements: holes})
    ]
  })

def once(hits):
  hits.push!(1)
  "a b"

def probe():
  hits = []
  double = template"run #{once(hits)}"
  single = template'--eval "#{"x" + 'y'}"'
  escaped = template'it\'s "quoted"; \#{literal}'
  plain = '#{not an expression}'
  if double[0] == "double" and double[1] == "run ?" and double[2] == ["a b"] and hits.count() == 1 and single[0] == "single" and single[1] == "--eval \"?\"" and single[2] == ["xy"] and escaped[1] == "it's \"quoted\"; \#{literal}" and plain == "\#{not an expression}":
    42
  else:
    0
