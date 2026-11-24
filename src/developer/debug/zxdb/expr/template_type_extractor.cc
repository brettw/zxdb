// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/template_type_extractor.h"

#include <lib/syslog/cpp/macros.h>

#include "src/developer/debug/zxdb/expr/expr_token.h"
#include "src/developer/debug/zxdb/expr/expr_tokenizer.h"
#include "src/developer/debug/zxdb/expr/operator_keyword.h"

namespace zxdb {

namespace {

// Tracks the level of nesting of brackets.
struct Nesting {
  Nesting(size_t i, ExprTokenType e) : opening_index(i), end(e) {}

  size_t opening_index = 0;                     // Index of opening bracket.
  ExprTokenType end = ExprTokenType::kInvalid;  // Expected closing bracket.
};

bool IsNamelikeToken(const ExprToken& token) {
  return token.type() == ExprTokenType::kName || token.type() == ExprTokenType::kTrue ||
         token.type() == ExprTokenType::kFalse || token.type() == ExprTokenType::kConst ||
         token.type() == ExprTokenType::kVolatile;
}

// Returns true if the token at the given index needs a space before it to separate it from the
// previous token. The first_index is the index of the first token being considered for type
// extraction (so we don't consider the boundary before this).
bool NeedsSpaceBefore(const std::vector<ExprToken>& tokens, size_t first_index, size_t index) {
  FX_DCHECK(first_index <= index);
  if (first_index == index)
    return false;  // Also catches index == 0.

  // Names always need a space between then. A name here is any word, so "const Foo" would be an
  // example.
  if (IsNamelikeToken(tokens[index - 1]) && IsNamelikeToken(tokens[index]))
    return true;

  // Put a space after a comma. This is undesirable in the case of "operator," appearing as in
  // "template<CmpOp a = operator,>" but not a big deal.
  if (tokens[index - 1].type() == ExprTokenType::kComma)
    return true;

  // Most other things can go next to each other as far as valid C++ goes. These are some cases that
  // this does incorrectly, see the comment above DoExtractTemplateType() for why this isn't so bad
  // and how it could be improved.
  return false;
}

// When stop_at_scope is true, stop before the top-level "::" that ends a name component. For
// example, Box<ns::T>::f yields Box<ns::T>. A conversion operator keeps its qualified target type,
// as in operator ns::T.
//
// Currently it assumes all operators can be put next to each other without affecting meaning. When
// we're canonicalizing types for the purposes of string comparisons, this is almost certainly the
// case. If we start using the output from this function for more things, we'll want to handle these
// cases better.
TemplateTypeResult DoExtractTemplateType(const std::vector<ExprToken>& tokens, size_t begin_token,
                                         bool stop_at_scope) {
  TemplateTypeResult result;

  bool inhibit_next_space = false;

  std::vector<Nesting> nesting;
  size_t i = begin_token;
  for (; i < tokens.size(); i++) {
    ExprTokenType type = tokens[i].type();
    if (type == ExprTokenType::kLeftSquare) {
      // [
      nesting.emplace_back(i, ExprTokenType::kRightSquare);
    } else if (type == ExprTokenType::kLeftParen) {
      // (
      nesting.emplace_back(i, ExprTokenType::kRightParen);
    } else if (type == ExprTokenType::kLess) {
      // < (the sequences "operator<" and "operator<<" were handled when we got the "operator"
      //    token).
      nesting.emplace_back(i, ExprTokenType::kGreater);
    } else if (nesting.empty() &&
               (type == ExprTokenType::kGreater || type == ExprTokenType::kRightParen ||
                type == ExprTokenType::kComma ||
                (stop_at_scope && type == ExprTokenType::kColonColon))) {
      // These tokens mark the end of a type when seen without nesting. Usually this marks the end
      // of the enclosing cast or template.
      break;
    } else if (!nesting.empty() && type == nesting.back().end) {
      // Found the closing token for a previous opening one.
      nesting.pop_back();
    } else if (type == ExprTokenType::kOperator) {
      // Possible space before "operator".
      if (NeedsSpaceBefore(tokens, begin_token, i))
        result.canonical_name.push_back(' ');

      if (OperatorKeywordResult op_result = ParseOperatorKeyword(tokens, i); op_result.success) {
        result.canonical_name.append(op_result.canonical_name);
        i = op_result.end_token - 1;
        inhibit_next_space = true;
        continue;  // Skip the code that appends the token at the bottom. We already did it.
      }
      // A conversion operator includes its target type, which can itself be qualified.
      if (stop_at_scope && nesting.empty()) {
        result.canonical_name += "operator ";
        stop_at_scope = false;
        continue;
      }
      // Fall through to append the keyword as a literal.
    }

    if (!inhibit_next_space && NeedsSpaceBefore(tokens, begin_token, i))
      result.canonical_name.push_back(' ');
    inhibit_next_space = false;

    result.canonical_name += tokens[i].value();
  }

  if (nesting.empty()) {
    result.success = true;
    result.end_token = i;
  } else {
    // Unterminated thing, tell the caller where it started.
    result.success = false;
    result.unmatched_error_token = nesting.back().opening_index;
    result.canonical_name.clear();
    result.end_token = tokens.size();
  }
  return result;
}

}  // namespace

TemplateTypeResult ExtractTemplateType(const std::vector<ExprToken>& tokens, size_t begin_token) {
  return DoExtractTemplateType(tokens, begin_token, false);
}

std::optional<Identifier> ParseQualifiedFunctionName(const std::string& name) {
  ExprTokenizer tokenizer(name);
  if (!tokenizer.Tokenize())
    return std::nullopt;
  const auto& tokens = tokenizer.tokens();
  Identifier result(IdentifierQualification::kGlobal);
  size_t pos = !tokens.empty() && tokens.front().type() == ExprTokenType::kColonColon ? 1 : 0;
  while (pos < tokens.size()) {
    auto component = DoExtractTemplateType(tokens, pos, true);
    if (!component.success || component.end_token == pos)
      return std::nullopt;
    if (component.canonical_name == "(anonymous namespace)")
      result.AppendComponent(IdentifierComponent(SpecialIdentifier::kAnon));
    else
      result.AppendComponent(IdentifierComponent(std::move(component.canonical_name)));
    pos = component.end_token;
    if (pos == tokens.size())
      return result;
    if (tokens[pos++].type() != ExprTokenType::kColonColon)
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace zxdb
