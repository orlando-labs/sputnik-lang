// Structured string-template controls. Providers receive the tree, transform
// its leaves according to their context, and opt into the common runtime fold
// by returning AstTemplateRender {parts, finish}.
struct TemplateLine {
  std::vector<std::unique_ptr<ast::Expr>> parts;
  std::string text;
  lexer::Span span;
  std::size_t indent = 0;
  bool marker = false;
  std::vector<const ast::Expr *> holes;
};

void template_shift_spans(ast::Expr &expr, const lexer::Position &base,
                          const std::string &file) {
  auto shift = [&](lexer::Position &p) {
    p.offset += base.offset;
    if (p.line == 1)
      p.col += base.col - 1;
    p.line += base.line - 1;
  };
  expr.span.file = file;
  shift(expr.span.start);
  shift(expr.span.end);
  for (auto &field : expr.node_fields)
    if (field.value)
      template_shift_spans(*field.value, base, file);
  for (auto &field : expr.list_fields)
    for (auto &child : field.values)
      if (child)
        template_shift_spans(*child, base, file);
}

class StringTemplateParser {
public:
  explicit StringTemplateParser(const ast::Expr &literal) : literal_(literal) {}

  std::unique_ptr<ast::Expr> parse(std::string *error) {
    error_ = error;
    const auto *parts = list_field(literal_, "parts");
    if (!parts)
      return ast::clone_expr(literal_);
    TemplateLine line;
    auto finish_line = [&] {
      if (line.parts.empty())
        return;
      // Only literal leading whitespace is layout. Escaped whitespace and
      // interpolated values are data and cannot introduce control syntax.
      for (const auto &part : line.parts) {
        if (part->kind != "AstStringText")
          break;
        const auto &value = *string_field(*part, "value");
        const auto first = value.find_first_not_of(" \t");
        line.indent += first == std::string::npos ? value.size() : first;
        if (first != std::string::npos) {
          line.marker = value[first] == '%';
          break;
        }
      }
      lines_.push_back(std::move(line));
      line = TemplateLine{};
    };
    for (const auto &part : parts->values) {
      if (part->kind == "AstStringExpr") {
        if (line.parts.empty())
          line.span = part->span;
        line.text += "#{__template_header_hole_" +
                     std::to_string(line.holes.size()) + "}";
        line.holes.push_back(node_field(*part, "expr"));
        line.parts.push_back(ast::clone_expr(*part));
        continue;
      }
      const auto *value = string_field(*part, "value");
      if (!value)
        continue;
      if (part->kind == "AstStringEscape") {
        if (line.parts.empty())
          line.span = part->span;
        // A directive header is parsed as source again. Retain escape
        // spelling there; cooked newlines/quotes would change its grammar.
        const auto *source = string_field(*part, "source");
        line.text += source ? *source : *value;
        line.parts.push_back(ast::clone_expr(*part));
        continue;
      }
      lexer::Position position = part->span.start;
      std::size_t start = 0;
      for (std::size_t i = 0; i < value->size(); ++i) {
        const bool newline = (*value)[i] == '\n';
        if (newline || i + 1 == value->size()) {
          auto text = ast::make_expr("AstStringText", part->span);
          text->span.start = position;
          text->string_field("value", value->substr(start, i + 1 - start));
          if (line.parts.empty())
            line.span = text->span;
          line.text += *string_field(*text, "value");
          line.parts.push_back(std::move(text));
          position.offset += i + 1 - start;
          position.col += i + 1 - start;
          if (newline) {
            ++position.line;
            position.col = 1;
          }
          start = i + 1;
          if (newline)
            finish_line();
        }
      }
    }
    finish_line();
    // Preserve the original flat parts (including escapes and source spans)
    // for templates with no control/escape lines.
    bool controls = false;
    for (const auto &entry : lines_)
      controls = controls || entry.marker;
    if (!controls)
      return ast::clone_expr(literal_);
    if (*string_field(literal_, "quote_kind") != "block") {
      fail("control lines require a multiline tagged template", literal_.span);
      return nullptr;
    }
    std::size_t cursor = 0;
    auto result = ast::clone_expr(literal_);
    auto *out = mutable_list_field(*result, "parts");
    out->values = parse_parts(cursor, -1, 0, 0);
    return error_->empty() ? std::move(result) : nullptr;
  }

private:
  void fail(const std::string &message, const lexer::Span &span) {
    if (error_->empty())
      *error_ = "STRING_TEMPLATE_CONTROL: " + message + " at " + span.file +
                ":" + std::to_string(span.start.line) + ":" +
                std::to_string(span.start.col);
  }

  bool blank(const TemplateLine &line) const {
    for (const auto &part : line.parts)
      if (part->kind != "AstStringText")
        return false;
    return line.text.find_first_not_of(" \t\r\n") == std::string::npos;
  }

  std::vector<std::unique_ptr<ast::Expr>> parse_parts(std::size_t &cursor,
                                                      int parent_indent,
                                                      std::size_t strip,
                                                      int depth) {
    std::vector<std::unique_ptr<ast::Expr>> result;
    if (depth > 64) {
      fail("nesting exceeds 64 levels", literal_.span);
      return result;
    }
    while (cursor < lines_.size() && error_->empty()) {
      const auto &line = lines_[cursor];
      if (!blank(line) && static_cast<int>(line.indent) <= parent_indent)
        break;
      if (!blank(line) && line.indent < strip) {
        fail("inconsistent template body indentation", line.span);
        break;
      }
      const std::string lead = line.text.substr(line.indent);
      if (line.marker && lead.rfind("%#", 0) == 0) {
        ++cursor;
        continue;
      }
      if (line.marker && lead.rfind("%%", 0) != 0) {
        if (lead.rfind("%for ", 0) != 0 && lead.rfind("%for\t", 0) != 0) {
          fail("unsupported directive (expected %for or %#)", line.span);
          break;
        }
        // Parse the real Sputnik for grammar, including patterns and
        // with_index. Only its dummy body is replaced after provider expansion.
        std::string header = lead.substr(1);
        while (!header.empty() &&
               (header.back() == '\n' || header.back() == '\r'))
          header.pop_back();
        lexer::Lexer lexer(header + "\n  null\n", line.span.file);
        auto lexed = lexer.lex();
        parser::Parser parser(lexed.tokens);
        auto parsed = parser.parse_expression_unit();
        if (!lexed.ok() || !parsed.ok() || !parsed.expr ||
            parsed.expr->kind != "AstPostfixChain") {
          fail("invalid %for header", line.span);
          break;
        }
        auto base = line.span.start;
        base.col += line.indent + 1;
        base.offset += line.indent + 1;
        template_shift_spans(*parsed.expr, base, line.span.file);
        // The outer literal already parsed interpolation inside a quoted
        // header expression. Restore those ASTs after parsing the for grammar.
        const auto restore = [&](auto &&self, ast::Expr &node) -> void {
          for (auto &field : node.node_fields) {
            if (!field.value)
              continue;
            if (node.kind == "AstStringExpr" && field.name == "expr" &&
                field.value->kind == "AstName") {
              const auto *name = string_field(*field.value, "name");
              bool restored = false;
              for (std::size_t i = 0; name && i < line.holes.size(); ++i)
                if (*name == "__template_header_hole_" + std::to_string(i)) {
                  field.value = ast::clone_expr(*line.holes[i]);
                  restored = true;
                  break;
                }
              if (restored)
                continue;
            }
            self(self, *field.value);
          }
          for (auto &field : node.list_fields)
            for (auto &child : field.values)
              if (child)
                self(self, *child);
        };
        restore(restore, *parsed.expr);
        const std::size_t header_indent = line.indent;
        ++cursor;
        std::size_t first = cursor;
        while (first < lines_.size() && blank(lines_[first]))
          ++first;
        if (first == lines_.size() || lines_[first].indent <= header_indent) {
          fail("%for requires an indented body", line.span);
          break;
        }
        const std::size_t extra = lines_[first].indent - header_indent;
        auto body = parse_parts(cursor, static_cast<int>(header_indent),
                                strip + extra, depth + 1);
        auto loop = ast::make_expr("AstStringFor", line.span);
        loop->node_field("iteration", std::move(parsed.expr));
        loop->list_field("parts", std::move(body));
        result.push_back(std::move(loop));
        continue;
      }
      std::size_t remaining = std::min(strip, line.indent);
      bool escaped = line.marker && lead.rfind("%%", 0) == 0;
      std::size_t source_offset = 0;
      for (const auto &part : line.parts) {
        auto copy = ast::clone_expr(*part);
        if (copy->kind == "AstStringText") {
          std::string text = *string_field(*copy, "value");
          if (escaped && source_offset <= line.indent &&
              source_offset + text.size() > line.indent) {
            text.erase(line.indent - source_offset, 1);
            escaped = false;
          }
          source_offset += string_field(*part, "value")->size();
          const auto count =
              std::min({remaining, text.size(), text.find_first_not_of(" \t")});
          text.erase(0, count);
          remaining -= count;
          set_string_field(*copy, "value", text);
          if (text.empty())
            continue;
        }
        result.push_back(std::move(copy));
      }
      ++cursor;
    }
    return result;
  }

  const ast::Expr &literal_;
  std::vector<TemplateLine> lines_;
  std::string *error_ = nullptr;
};

std::unique_ptr<ast::Expr>
template_call(std::unique_ptr<ast::Expr> callee,
              std::vector<std::unique_ptr<ast::Expr>> args,
              const lexer::Span &span) {
  auto call = ast::make_expr("AstTailCall", span);
  call->string_field("call_style", "paren");
  call->list_field("args", std::move(args));
  if (callee->kind != "AstPostfixChain") {
    auto chain = ast::make_expr("AstPostfixChain", span);
    chain->node_field("base", std::move(callee));
    chain->list_field("tails", {});
    callee = std::move(chain);
  }
  mutable_list_field(*callee, "tails")->values.push_back(std::move(call));
  return callee;
}

std::unique_ptr<ast::Expr> lower_template_render(const ast::Expr &node,
                                                 const std::string &mark,
                                                 std::string *error) {
  const auto *parts = list_field(node, "parts");
  const auto *finish = node_field(node, "finish");
  if (!parts || !finish) {
    *error = "AstTemplateRender requires parts and finish";
    return nullptr;
  }
  auto builder = [&] {
    auto name = ast::make_expr("AstName", node.span);
    name->string_field("name", "__string_template_parts");
    name->string_field("syntax_context", mark);
    return name;
  };
  auto emit = [&](const ast::Expr &value) {
    auto chain = ast::make_expr("AstPostfixChain", value.span);
    chain->node_field("base", builder());
    auto member = ast::make_expr("AstTailDotMember", value.span);
    member->string_field("name", "push!");
    member->bool_field("chain_boundary", false);
    std::vector<std::unique_ptr<ast::Expr>> tails;
    tails.push_back(std::move(member));
    chain->list_field("tails", std::move(tails));
    std::vector<std::unique_ptr<ast::Expr>> args;
    args.push_back(ast::clone_expr(value));
    return template_call(std::move(chain), std::move(args), value.span);
  };
  std::function<std::vector<std::unique_ptr<ast::Expr>>(const ast::ListField &)>
      lower;
  lower = [&](const ast::ListField &input) {
    std::vector<std::unique_ptr<ast::Expr>> out;
    for (const auto &part : input.values) {
      if (part->kind != "AstStringFor") {
        out.push_back(emit(*part));
        continue;
      }
      const auto *original = node_field(*part, "iteration");
      const auto *body = list_field(*part, "parts");
      if (!original || !body) {
        *error = "invalid AstStringFor";
        break;
      }
      auto loop = ast::clone_expr(*original);
      auto *tails = mutable_list_field(*loop, "tails");
      if (!tails || tails->values.empty() || !tails->values.back()) {
        *error = "invalid template loop";
        break;
      }
      auto *block =
          const_cast<ast::Expr *>(node_field(*tails->values.back(), "block"));
      if (!block || !mutable_list_field(*block, "body")) {
        *error = "invalid template loop block";
        break;
      }
      auto statements = lower(*body);
      std::vector<std::unique_ptr<ast::Expr>> wrapped;
      for (auto &expr : statements) {
        auto stmt = ast::make_expr("AstExprStmt", expr->span);
        stmt->bool_field("preserve_last", true);
        stmt->node_field("expr", std::move(expr));
        wrapped.push_back(std::move(stmt));
      }
      mutable_list_field(*block, "body")->values = std::move(wrapped);
      out.push_back(std::move(loop));
    }
    return out;
  };
  auto init = ast::make_expr("AstAssign", node.span);
  init->string_field("op", "=");
  init->node_field("left", builder());
  auto array = ast::make_expr("AstListLiteral", node.span);
  array->list_field("elements", {});
  init->node_field("right", std::move(array));
  auto items = lower(*parts);
  items.insert(items.begin(), std::move(init));
  std::vector<std::unique_ptr<ast::Expr>> args;
  args.push_back(builder());
  items.push_back(
      template_call(ast::clone_expr(*finish), std::move(args), node.span));
  auto result = ast::make_expr("AstThen", node.span);
  result->bool_field("preserve_last", true);
  result->list_field("items", std::move(items));
  return error->empty() ? std::move(result) : nullptr;
}
