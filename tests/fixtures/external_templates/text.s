package app.text
export finish
export macro text
export macro source

def finish(parts): parts.join("")

macro def _parts(parts):
  return parts.map |part|:
    case part.kind:
      when "AstStringFor":
        Ast.node("AstStringFor", {iteration: part.iteration, parts: _parts(part.parts)})
      when "AstStringExpr":
        quote:
          Sputnik.stringify(#{part.expr})
      else:
        Ast.lift(part.value)

string_tag macro def text(t as Ast.StringTemplate):
  finish = Ast.node("AstQualifiedRef", {module: "app.text", name: "finish"})
  return Ast.node("AstTemplateRender", {parts: _parts(t.parts), finish: finish})

string_tag macro def source(t as Ast.StringTemplate):
  return Ast.lift(t.source)
