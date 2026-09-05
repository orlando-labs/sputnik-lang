#include "frontend/lexer/lexer.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using amber::lexer::Lexer;
using amber::lexer::Token;
using amber::lexer::TokenKind;

std::vector<Token> lex_ok(const std::string &source) {
  Lexer lexer(source, "<test>");
  amber::lexer::LexResult result = lexer.lex();
  if (!result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(result.diagnostics);
    std::exit(1);
  }
  return result.tokens;
}

amber::lexer::LexResult lex_raw(const std::string &source) {
  Lexer lexer(source, "<test>");
  return lexer.lex();
}

void expect_kinds(const std::string &name, const std::string &source,
                  const std::vector<TokenKind> &expected) {
  const std::vector<Token> tokens = lex_ok(source);
  std::vector<TokenKind> actual;
  actual.reserve(tokens.size());
  for (const Token &token : tokens) {
    actual.push_back(token.kind);
  }
  if (actual != expected) {
    std::cerr << "lexer test failed: " << name << "\nexpected:";
    for (TokenKind kind : expected) {
      std::cerr << " " << amber::lexer::token_kind_name(kind);
    }
    std::cerr << "\nactual:  ";
    for (TokenKind kind : actual) {
      std::cerr << " " << amber::lexer::token_kind_name(kind);
    }
    std::cerr << "\n";
    std::exit(1);
  }
}

void expect_identifier_lexemes(const std::string &name,
                               const std::string &source,
                               const std::vector<std::string> &expected) {
  const std::vector<Token> tokens = lex_ok(source);
  std::vector<std::string> actual;
  for (const Token &token : tokens) {
    if (token.kind == TokenKind::Identifier) {
      actual.push_back(token.lexeme);
    }
  }
  if (actual != expected) {
    std::cerr << "lexer test failed: " << name << "\nexpected:";
    for (const std::string &value : expected) {
      std::cerr << " " << value;
    }
    std::cerr << "\nactual:  ";
    for (const std::string &value : actual) {
      std::cerr << " " << value;
    }
    std::cerr << "\n";
    std::exit(1);
  }
}

void test_block_tokens() {
  expect_kinds(
      "indent/dedent",
      "def todo()\n"
      "if active?:\n"
      "  null\n"
      "else:\n"
      "  log \"disabled\"\n",
      {TokenKind::KeywordDef,  TokenKind::Identifier,  TokenKind::LParen,
       TokenKind::RParen,      TokenKind::Newline,
       TokenKind::KeywordIf,   TokenKind::Identifier,
       TokenKind::Colon,       TokenKind::Newline,     TokenKind::Indent,
       TokenKind::KeywordNull, TokenKind::Newline,     TokenKind::Dedent,
       TokenKind::KeywordElse, TokenKind::Colon,       TokenKind::Newline,
       TokenKind::Indent,      TokenKind::Identifier,  TokenKind::String,
       TokenKind::Newline,     TokenKind::Dedent,      TokenKind::Eof});

  expect_kinds("pass and noop are ordinary identifiers", "pass = noop\n",
               {TokenKind::Identifier, TokenKind::Equal,
                TokenKind::Identifier, TokenKind::Newline, TokenKind::Eof});
}

void test_safe_and_chain_dot() {
  expect_kinds(
      "safe-dot and chain-dot",
      "numbers.map: _1.email.downcase() .uniq()\n"
      "obj.?.field\n",
      {TokenKind::Identifier, TokenKind::Dot,         TokenKind::Identifier,
       TokenKind::Colon,      TokenKind::Placeholder, TokenKind::Dot,
       TokenKind::Identifier, TokenKind::Dot,         TokenKind::Identifier,
       TokenKind::LParen,     TokenKind::RParen,      TokenKind::ChainDot,
       TokenKind::Identifier, TokenKind::LParen,      TokenKind::RParen,
       TokenKind::Newline,    TokenKind::Identifier,  TokenKind::SafeDot,
       TokenKind::Identifier, TokenKind::Newline,     TokenKind::Eof});
}

void test_case_bang_and_last_value() {
  expect_kinds("case bang and last value",
               "case! value:\n"
               "  when _:\n"
               "    $_\n",
               {TokenKind::KeywordCaseBang, TokenKind::Identifier,
                TokenKind::Colon, TokenKind::Newline, TokenKind::Indent,
                TokenKind::KeywordWhen, TokenKind::Identifier, TokenKind::Colon,
                TokenKind::Newline, TokenKind::Indent, TokenKind::LastValue,
                TokenKind::Newline, TokenKind::Dedent, TokenKind::Dedent,
                TokenKind::Eof});
}

void test_identifier_forms() {
  expect_kinds("identifier forms",
               "active? clear! _tmp __cache _1 $_ $it $it1 $it2\n",
               {TokenKind::Identifier, TokenKind::Identifier,
                TokenKind::Identifier, TokenKind::Identifier,
                TokenKind::Placeholder, TokenKind::LastValue,
                TokenKind::Placeholder, TokenKind::Placeholder,
                TokenKind::Placeholder,
                TokenKind::Newline, TokenKind::Eof});

  const std::vector<Token> aliases = lex_ok("$it $it1 $it12\n");
  if (aliases[0].lexeme != "$it" || aliases[1].lexeme != "$it1" ||
      aliases[2].lexeme != "$it12") {
    std::cerr << "lexer test failed: placeholder aliases preserve lexemes\n";
    std::exit(1);
  }

  for (const std::string &invalid :
       {"$it0\n", "$it01\n", "$item\n", "$it_1\n", "$it2x\n",
        "$it?\n", "$it!\n", "$it1?\n", "$it?[0]\n", "$itя\n", "$it1é\n"}) {
    if (lex_raw(invalid).ok()) {
      std::cerr << "lexer test failed: invalid placeholder alias accepted: "
                << invalid;
      std::exit(1);
    }
  }

  expect_kinds("placeholder alias followed by inequality", "$it!=0\n",
               {TokenKind::Placeholder, TokenKind::BangEqual,
                TokenKind::Integer, TokenKind::Newline, TokenKind::Eof});
  expect_kinds("placeholder alias followed by regexp operator",
               "$it1!~pattern\n",
               {TokenKind::Placeholder, TokenKind::BangTilde,
                TokenKind::Identifier, TokenKind::Newline, TokenKind::Eof});
  expect_kinds("placeholder alias followed by optional index", "$it2[?0]\n",
               {TokenKind::Placeholder, TokenKind::LBracket,
                TokenKind::Question, TokenKind::Integer, TokenKind::RBracket,
                TokenKind::Newline, TokenKind::Eof});
}

void test_contextual_keywords_remain_identifiers() {
  expect_kinds("contextual keywords", "as pattern with\n",
               {TokenKind::Identifier, TokenKind::Identifier,
                TokenKind::Identifier, TokenKind::Newline, TokenKind::Eof});
}

void test_pattern_punctuation() {
  expect_kinds("pattern punctuation", "^x | y -> T ?\n",
               {TokenKind::Caret, TokenKind::Identifier, TokenKind::Pipe,
                TokenKind::Identifier, TokenKind::Arrow, TokenKind::Identifier,
                TokenKind::Question, TokenKind::Newline, TokenKind::Eof});
}

void test_effect_row_punctuation() {
  expect_kinds("effect row punctuation", "def f() -> Int !{time, fs}:\n",
               {TokenKind::KeywordDef, TokenKind::Identifier, TokenKind::LParen,
                TokenKind::RParen, TokenKind::Arrow, TokenKind::Identifier,
                TokenKind::Bang, TokenKind::LBrace, TokenKind::Identifier,
                TokenKind::Comma, TokenKind::Identifier, TokenKind::RBrace,
                TokenKind::Colon, TokenKind::Newline, TokenKind::Eof});
}

void test_property_keywords() {
  expect_kinds("property keywords",
               "prop answer: 42\n"
               "class_prop version: \"20.3\"\n",
               {TokenKind::KeywordProp, TokenKind::Identifier,
                TokenKind::Colon, TokenKind::Integer, TokenKind::Newline,
                TokenKind::KeywordClassProp, TokenKind::Identifier,
                TokenKind::Colon, TokenKind::String, TokenKind::Newline,
                TokenKind::Eof});
}

void test_attribute_keyword() {
  expect_kinds("attribute keyword",
               "attr email\n"
               "attr var value from @raw_value\n",
               {TokenKind::KeywordAttr, TokenKind::Identifier,
                TokenKind::Newline, TokenKind::KeywordAttr,
                TokenKind::Identifier, TokenKind::Identifier,
                TokenKind::KeywordFrom, TokenKind::At, TokenKind::Identifier,
                TokenKind::Newline, TokenKind::Eof});
}

void test_unicode_identifier_forms() {
  expect_kinds("unicode identifiers",
               "масса = α + β2\n"
               "объект.скорость!()\n",
               {TokenKind::Identifier, TokenKind::Equal, TokenKind::Identifier,
                TokenKind::Plus, TokenKind::Identifier, TokenKind::Newline,
                TokenKind::Identifier, TokenKind::Dot, TokenKind::Identifier,
                TokenKind::LParen, TokenKind::RParen, TokenKind::Newline,
                TokenKind::Eof});
  expect_identifier_lexemes("unicode identifier lexemes",
                            "масса = α + β2\nобъект.скорость!()\n",
                            {"масса", "α", "β2", "объект", "скорость!"});
}

void test_w13_comments_ranges_and_numbers() {
  expect_kinds("w13 shebang comments ranges and numeric forms",
               "#!/usr/bin/env amber\n"
               "x = 1..10 # inclusive range\n"
               "1_000 0xFF 0b1010_0101 0o755 1e9 1.2e-3\n",
               {TokenKind::Identifier, TokenKind::Equal, TokenKind::Integer,
                TokenKind::DotDot, TokenKind::Integer, TokenKind::Newline,
                TokenKind::Integer, TokenKind::Integer, TokenKind::Integer,
                TokenKind::Integer, TokenKind::Float, TokenKind::Float,
                TokenKind::Newline, TokenKind::Eof});

  expect_kinds("glued hash is an unbound-reference separator token",
               "foo#bar\n",
               {TokenKind::Identifier, TokenKind::Hash,
                TokenKind::Identifier, TokenKind::Newline, TokenKind::Eof});

  amber::lexer::LexResult bad_number = lex_raw("1__0\n");
  if (bad_number.ok()) {
    std::cerr << "lexer test failed: invalid numeric separators accepted\n";
    std::exit(1);
  }
}

void test_new_operator_tokens() {
  expect_kinds("floor division, modulo, spaceship, power, bitwise, shifts",
               "a // b % c <=> d ** e & f ^ g | h << i >> j\n",
               {TokenKind::Identifier, TokenKind::SlashSlash,
                TokenKind::Identifier, TokenKind::Percent,
                TokenKind::Identifier, TokenKind::LessEqualGreater,
                TokenKind::Identifier, TokenKind::StarStar,
                TokenKind::Identifier, TokenKind::Ampersand,
                TokenKind::Identifier, TokenKind::Caret,
                TokenKind::Identifier, TokenKind::Pipe,
                TokenKind::Identifier, TokenKind::LessLess,
                TokenKind::Identifier, TokenKind::GreaterGreater,
                TokenKind::Identifier, TokenKind::Newline, TokenKind::Eof});
}

void test_string_interpolation_lexing() {
  expect_kinds("interpolated string stays one token",
               "\"#{if ok then \"yes\" else \"no\"}\"\n",
               {TokenKind::String, TokenKind::Newline, TokenKind::Eof});

  amber::lexer::LexResult bad_escape = lex_raw("\"bad\\q\"\n");
  if (bad_escape.ok() || bad_escape.diagnostics.empty() ||
      bad_escape.diagnostics[0].code != "AMB_STRING_BAD_ESCAPE") {
    std::cerr << "lexer test failed: bad escape diagnostic code\n";
    std::exit(1);
  }

  amber::lexer::LexResult unterminated = lex_raw("\"#{value\"\n");
  if (unterminated.ok() || unterminated.diagnostics.empty() ||
      unterminated.diagnostics[0].code != "AMB_STRING_INTERP_UNTERMINATED" ||
      unterminated.diagnostics[0].phase != "lexer") {
    std::cerr << "lexer test failed: unterminated interpolation diagnostic\n";
    std::exit(1);
  }
}

} // namespace

int main() {
  test_block_tokens();
  test_safe_and_chain_dot();
  test_case_bang_and_last_value();
  test_identifier_forms();
  test_contextual_keywords_remain_identifiers();
  test_pattern_punctuation();
  test_effect_row_punctuation();
  test_property_keywords();
  test_attribute_keyword();
  test_unicode_identifier_forms();
  test_w13_comments_ranges_and_numbers();
  test_new_operator_tokens();
  test_string_interpolation_lexing();
  std::cout << "lexer_tests: ok\n";
  return 0;
}
