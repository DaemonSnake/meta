//===--- Parser.cpp - C Language Family Parser ----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements parsing for reflection facilities.
//
//===----------------------------------------------------------------------===//

#include "clang/Parse/Parser.h"
#include "clang/AST/ASTContext.h"
#include "clang/Parse/ParseDiagnostic.h"
#include "clang/Parse/RAIIObjectsForParser.h"
#include "clang/Sema/ParsedReflection.h"
using namespace clang;

/// Parse the operand of a reflexpr expression. This is almost exactly like
/// parsing a template argument, except that we also allow namespace-names
/// in this context.
ParsedReflectionOperand Parser::ParseCXXReflectOperand() {
  Sema::CXXReflectionScopeRAII ReflectionScope(Actions);

  // The operand is unevaluated.
  EnterExpressionEvaluationContext Unevaluated(
      Actions, Sema::ExpressionEvaluationContext::Unevaluated);

  // Perform the tentative parses first since isCXXTypeId tends to rewrite
  // tokens, which can subsequent parses a bit wonky.

  // Tentatively parse a template-name.
  {
    TentativeParsingAction TPA(*this);
    ParsedTemplateArgument T = ParseTemplateTemplateArgument();
    if (!T.isInvalid()) {
      TPA.Commit();
      return Actions.ActOnReflectedTemplate(T);
    }
    TPA.Revert();
  }

  // Otherwise, check for the global namespace
  if (Tok.is(tok::coloncolon) && NextToken().is(tok::r_paren)) {
    SourceLocation ColonColonLoc = ConsumeToken();
    return Actions.ActOnReflectedNamespace(ColonColonLoc);
  }

  // Otherwise, tentatively parse a namespace-name.
  {
    TentativeParsingAction TPA(*this);
    CXXScopeSpec SS;
    SourceLocation IdLoc;
    Decl *D = ParseNamespaceName(SS, IdLoc);
    if (D) {
      TPA.Commit();
      return Actions.ActOnReflectedNamespace(SS, IdLoc, D);
    }
    TPA.Revert();
  }

  // Otherwise, try parsing this as type-id.
  if (isCXXTypeId(TypeIdAsTemplateArgument)) {
    // FIXME: Create a new DeclaratorContext?
    TypeResult T =
      ParseTypeName(nullptr, DeclaratorContext::TemplateArgContext);
    if (T.isInvalid()) {
      return ParsedReflectionOperand();
    }
    return Actions.ActOnReflectedType(T.get());
  }

  // Parse an expression. template argument.
  ExprResult E = ParseExpression(NotTypeCast);
  if (E.isInvalid() || !E.get())
    return ParsedReflectionOperand();

  return Actions.ActOnReflectedExpression(E.get());
}


/// Parse a reflect-expression.
///
/// \verbatim
///       reflect-expression:
///         'reflexpr' '(' type-id ')'
///         'reflexpr' '(' template-name ')'
///         'reflexpr' '(' namespace-name ')'
///         'reflexpr' '(' id-expression ')'
/// \endverbatim
ExprResult Parser::ParseCXXReflectExpression() {
  assert(Tok.is(tok::kw_reflexpr) && "expected 'reflexpr'");
  SourceLocation KWLoc = ConsumeToken();

  BalancedDelimiterTracker T(*this, tok::l_paren);
  if (T.expectAndConsume(diag::err_expected_lparen_after, "reflexpr"))
    return ExprError();

  ParsedReflectionOperand PR = ParseCXXReflectOperand();
  if (PR.isInvalid())
    return ExprError();

  if (T.consumeClose())
    return ExprError();

  return Actions.ActOnCXXReflectExpr(KWLoc, PR,
                                     T.getOpenLocation(),
                                     T.getCloseLocation());
}

/// Parse an invalid reflection.
///
/// \verbatim
///  primary-expression:
///    __valid_reflection '(' error-message ')'
/// \endverbatim
ExprResult Parser::ParseCXXInvalidReflectionExpression() {
  assert(Tok.is(tok::kw___invalid_reflection) && "Not '__invalid_reflection'");

  SourceLocation BuiltinLoc = ConsumeToken();
  BalancedDelimiterTracker T(*this, tok::l_paren);

  if (T.expectAndConsume(diag::err_expected_lparen_after, "__invalid_reflection"))
    return ExprError();

  ExprResult MessageExpr = ParseConstantExpression();

  if (MessageExpr.isInvalid()) {
    SkipUntil(tok::r_paren, StopAtSemi);
    return ExprError();
  }

  if (T.consumeClose())
    return ExprError();

  return Actions.ActOnCXXInvalidReflectionExpr(MessageExpr.get(), BuiltinLoc,
                                               T.getCloseLocation());
}

/// Parse a reflection trait.
///
/// \verbatim
///   primary-expression:
///     __reflect '(' expression-list ')'
/// \endverbatim
ExprResult Parser::ParseCXXReflectionReadQuery() {
  assert(Tok.is(tok::kw___reflect) && "Not __reflect");
  SourceLocation Loc = ConsumeToken();

  // Parse any number of arguments in parens.
  BalancedDelimiterTracker Parens(*this, tok::l_paren);
  if (Parens.expectAndConsume())
    return ExprError();

  SmallVector<Expr *, 2> Args;
  do {
    ExprResult Expr = ParseConstantExpression();
    if (Expr.isInvalid()) {
      Parens.skipToEnd();
      return ExprError();
    }
    Args.push_back(Expr.get());
  } while (TryConsumeToken(tok::comma));

  if (Parens.consumeClose())
    return ExprError();

  SourceLocation LPLoc = Parens.getOpenLocation();
  SourceLocation RPLoc = Parens.getCloseLocation();
  return Actions.ActOnCXXReflectionReadQuery(Loc, Args, LPLoc, RPLoc);
}

/// Parse a reflective pretty print of integer and string values.
///
/// \verbatim
///   primary-expression:
///     __reflect_print '(' expression-list ')'
/// \endverbatim
ExprResult Parser::ParseCXXReflectPrintLiteralExpression() {
  assert(Tok.is(tok::kw___reflect_print) && "Not __reflect_print");
  SourceLocation Loc = ConsumeToken();

  // Parse any number of arguments in parens.
  BalancedDelimiterTracker Parens(*this, tok::l_paren);
  if (Parens.expectAndConsume())
    return ExprError();

  SmallVector<Expr *, 2> Args;
  do {
    ExprResult Expr = ParseConstantExpression();
    if (Expr.isInvalid()) {
      Parens.skipToEnd();
      return ExprError();
    }
    Args.push_back(Expr.get());
  } while (TryConsumeToken(tok::comma));

  if (Parens.consumeClose())
    return ExprError();

  SourceLocation LPLoc = Parens.getOpenLocation();
  SourceLocation RPLoc = Parens.getCloseLocation();
  return Actions.ActOnCXXReflectPrintLiteral(Loc, Args, LPLoc, RPLoc);
}

/// Parse a reflective pretty print of a reflection.
///
/// \verbatim
///   primary-expression:
///     __reflect_pretty_print '(' reflection ')'
/// \endverbatim
ExprResult Parser::ParseCXXReflectPrintReflectionExpression() {
  assert(Tok.is(tok::kw___reflect_pretty_print) && "Not __reflect_pretty_print");
  SourceLocation Loc = ConsumeToken();

  // Parse any number of arguments in parens.
  BalancedDelimiterTracker Parens(*this, tok::l_paren);
  if (Parens.expectAndConsume())
    return ExprError();

  ExprResult Reflection = ParseConstantExpression();
  if (Reflection.isInvalid()) {
    Parens.skipToEnd();
    return ExprError();
  }

  if (Parens.consumeClose())
    return ExprError();

  SourceLocation LPLoc = Parens.getOpenLocation();
  SourceLocation RPLoc = Parens.getCloseLocation();
  return Actions.ActOnCXXReflectPrintReflection(Loc, Reflection.get(),
                                                LPLoc, RPLoc);
}

/// Parse a reflective dump of a reflection.
///
/// \verbatim
///   primary-expression:
///     __reflect_dump '(' reflection ')'
/// \endverbatim
ExprResult Parser::ParseCXXReflectDumpReflectionExpression() {
  assert(Tok.is(tok::kw___reflect_dump) && "Not __reflect_dump");
  SourceLocation Loc = ConsumeToken();

  // Parse any number of arguments in parens.
  BalancedDelimiterTracker Parens(*this, tok::l_paren);
  if (Parens.expectAndConsume())
    return ExprError();

  ExprResult Reflection = ParseConstantExpression();
  if (Reflection.isInvalid()) {
    Parens.skipToEnd();
    return ExprError();
  }

  if (Parens.consumeClose())
    return ExprError();

  SourceLocation LPLoc = Parens.getOpenLocation();
  SourceLocation RPLoc = Parens.getCloseLocation();
  return Actions.ActOnCXXReflectDumpReflection(Loc, Reflection.get(),
                                               LPLoc, RPLoc);
}

ExprResult Parser::ParseCXXCompilerErrorExpression() {
  assert(Tok.is(tok::kw___compiler_error) && "Not '__compiler_error'");

  SourceLocation BuiltinLoc = ConsumeToken();
  BalancedDelimiterTracker T(*this, tok::l_paren);

  if (T.expectAndConsume(diag::err_expected_lparen_after, "__compiler_error"))
    return ExprError();

  ExprResult MessageExpr = ParseConstantExpression();

  if (MessageExpr.isInvalid()) {
    SkipUntil(tok::r_paren, StopAtSemi);
    return ExprError();
  }

  if (T.consumeClose())
    return ExprError();

  return Actions.ActOnCXXCompilerErrorExpr(MessageExpr.get(), BuiltinLoc,
                                           T.getCloseLocation());
}

bool Parser::matchCXXSpliceBegin(tok::TokenKind T, unsigned LookAhead) {
  if (getRelativeToken(LookAhead).isNot(tok::l_square))
    return false;
  if (getRelativeToken(LookAhead + 1).isNot(T))
    return false;

  return true;
}

bool Parser::matchCXXSpliceEnd(tok::TokenKind T, unsigned LookAhead) {
  if (getRelativeToken(LookAhead).isNot(T))
    return false;
  if (getRelativeToken(LookAhead + 1).isNot(tok::r_square))
    return false;

  return true;
}
bool Parser::parseCXXSpliceBegin(tok::TokenKind T, SourceLocation &SL) {
  if (!matchCXXSpliceBegin(T))
    return true;

  SL = ConsumeBracket();
  ConsumeToken();
  return false;
}

bool Parser::parseCXXSpliceEnd(tok::TokenKind T, SourceLocation &SL) {
  if (!matchCXXSpliceEnd(T)) {
    Diag(Tok, diag::err_expected_end_of_splice);
    return true;
  }

  ConsumeToken();
  SL = ConsumeBracket();
  return false;
}

/// Parse an expression splice expression.
///
/// \verbatim
///   decl-splice:
///     '[' '<' constant-expression '>' ']'
/// \endverbatim
ExprResult Parser::ParseCXXExprSpliceExpr() {
  assert(matchCXXSpliceBegin(tok::less) && "Not '[<'");

  SourceLocation SBELoc;
  if (parseCXXSpliceBegin(tok::less, SBELoc))
    return ExprError();

  ExprResult Expr = ParseConstantExpression();
  if (Expr.isInvalid())
    return ExprError();

  SourceLocation SEELoc;
  if (parseCXXSpliceEnd(tok::greater, SEELoc))
    return ExprError();

  return Actions.ActOnCXXExprSpliceExpr(SBELoc, Expr.get(), SEELoc);
}

ExprResult Parser::ParseCXXMemberExprSpliceExpr(Expr *Base) {
  assert(Tok.isOneOf(tok::arrow, tok::period));

  bool IsArrow = Tok.getKind() == tok::arrow;
  SourceLocation OperatorLoc = ConsumeToken();
  SourceLocation TemplateKWLoc;
  if (Tok.is(tok::kw_template))
    TemplateKWLoc = ConsumeToken();

  SourceLocation SBELoc;
  if (parseCXXSpliceBegin(tok::less, SBELoc))
    return ExprError();

  ExprResult Expr = ParseConstantExpression();
  if (Expr.isInvalid())
    return ExprError();

  SourceLocation SEELoc;
  if (parseCXXSpliceEnd(tok::greater, SEELoc))
    return ExprError();

  // Check for template arguments
  if (Tok.is(tok::less) && !TemplateKWLoc.isInvalid()) {
    SourceLocation LAngleLoc;
    TemplateArgList TemplateArgs;
    SourceLocation RAngleLoc;

    if (ParseTemplateIdAfterTemplateName(/*ConsumeLastToken=*/true,
                                         LAngleLoc, TemplateArgs, RAngleLoc))
      return ExprError();

    ASTTemplateArgsPtr TemplateArgsPtr(TemplateArgs);

    return Actions.ActOnCXXMemberExprSpliceExpr(
        Base, Expr.get(), IsArrow, OperatorLoc, TemplateKWLoc,
        SBELoc, SEELoc, LAngleLoc, TemplateArgsPtr, RAngleLoc);
  }

  return Actions.ActOnCXXMemberExprSpliceExpr(
      Base, Expr.get(), IsArrow, OperatorLoc,
      TemplateKWLoc, SBELoc, SEELoc, /*TemplateArgs=*/nullptr);
}

bool Parser::AnnotateIdentifierSplice() {
  assert(matchCXXSpliceBegin(tok::hash) && GetLookAheadToken(2).isNot(tok::ellipsis));

  // Attempt to reinterpret an identifier splice as a single annotated token.
  IdentifierInfo *II;
  SourceLocation IIBeginLoc;
  SourceLocation IIEndLoc;

  bool Invalid = ParseCXXIdentifierSplice(II, IIBeginLoc, IIEndLoc);
  if (Invalid) {
    // If invalid replace the identifier with a unique invalid identifier
    // for recovery purposes.
    Actions.ActOnCXXInvalidIdentifierSplice(II);
  }

  Token AnnotTok;
  AnnotTok.startToken();
  AnnotTok.setKind(Invalid ? tok::annot_invalid_identifier_splice
                           : tok::annot_identifier_splice);
  AnnotTok.setAnnotationValue(reinterpret_cast<void *>(II));
  AnnotTok.setLocation(IIBeginLoc);
  AnnotTok.setAnnotationEndLoc(IIEndLoc);
  UnconsumeToken(AnnotTok);

  return Invalid;
}

bool Parser::TryAnnotateIdentifierSplice() {
  if (!matchCXXSpliceBegin(tok::hash) || GetLookAheadToken(2).is(tok::ellipsis))
    return false;

  return AnnotateIdentifierSplice();
}

bool Parser::ParseCXXIdentifierSplice(
    IdentifierInfo *&Id, SourceLocation &IdBeginLoc) {
  SourceLocation IdEndLoc;
  return ParseCXXIdentifierSplice(Id, IdBeginLoc, IdEndLoc);
}

/// Parse an identifier splice
///
///   identifier-splice:
///     '[' '#' reflection '#' ']'
///
/// Returns true if parsing or semantic analysis fail.
bool Parser::ParseCXXIdentifierSplice(
    IdentifierInfo *&Id, SourceLocation &IdBeginLoc, SourceLocation &IdEndLoc) {
  assert(matchCXXSpliceBegin(tok::hash) && "Not '[#'");

  if (parseCXXSpliceBegin(tok::hash, IdBeginLoc))
    return true;

  SmallVector<Expr *, 4> Parts;
  while (true) {
    ExprResult Result = ParseConstantExpression();
    if (Result.isInvalid()) {
      SkipUntil(tok::r_square);
      return true;
    }

    Parts.push_back(Result.get());
    if (matchCXXSpliceEnd(tok::hash))
      break;

    if (ExpectAndConsume(tok::comma)) {
      SkipUntil(tok::r_square);
      return true;
    }
  }

  if (parseCXXSpliceEnd(tok::hash, IdEndLoc))
    return true;

  ArrayRef<Expr *> FinalParts(Parts.data(), Parts.size());
  if (Actions.ActOnCXXIdentifierSplice(FinalParts, Id))
    return true;

  return false;
}

/// Parse a type splice
///
/// \verbatim
///   type-splice:
///     'typename' '[' '<' reflection '>' ']'
/// \endverbatim
///
/// The constant expression must be a reflection of a type.
SourceLocation Parser::ParseTypeSplice(DeclSpec &DS) {
  assert(Tok.is(tok::annot_type_splice) ||
         (Tok.is(tok::kw_typename) &&
          matchCXXSpliceBegin(tok::less, /*LookAhead=*/1))
         && "Not a type splice");

  ExprResult Result;
  SourceLocation StartLoc = Tok.getLocation();
  SourceLocation EndLoc;

  if (Tok.is(tok::annot_type_splice)) {
    Result = getExprAnnotation(Tok);
    EndLoc = Tok.getAnnotationEndLoc();
    ConsumeAnnotationToken();
    if (Result.isInvalid()) {
      DS.SetTypeSpecError();
      return EndLoc;
    }
  } else {
    StartLoc = ConsumeToken();

    SourceLocation SBELoc;
    if (parseCXXSpliceBegin(tok::less, SBELoc))
      return EndLoc;

    Result = ParseConstantExpression();

    if (parseCXXSpliceEnd(tok::greater, EndLoc))
      return EndLoc;
  }

  const char *PrevSpec = nullptr;
  unsigned DiagID;
  const PrintingPolicy &Policy = Actions.getASTContext().getPrintingPolicy();

  if (DS.SetTypeSpecType(DeclSpec::TST_type_splice, StartLoc, PrevSpec,
                         DiagID, Result.get(), Policy)) {
    Diag(StartLoc, DiagID) << PrevSpec;
    DS.SetTypeSpecError();
  }
  return EndLoc;
}

void Parser::AnnotateExistingTypeSplice(const DeclSpec &DS,
                                        SourceLocation StartLoc,
                                        SourceLocation EndLoc) {
  // make sure we have a token we can turn into an annotation token
  if (PP.isBacktrackEnabled())
    PP.RevertCachedTokens(1);
  else
    PP.EnterToken(Tok, /*IsReinject*/true);

  Tok.setKind(tok::annot_type_splice);
  setExprAnnotation(Tok,
                    DS.getTypeSpecType() == TST_type_splice ?
                    DS.getRepAsExpr() : ExprError());
  Tok.setAnnotationEndLoc(EndLoc);
  Tok.setLocation(StartLoc);
  PP.AnnotateCachedTokens(Tok);
}

/// Parse a concatenation expression.
///
///   primary-expression:
///      '__concatenate' '(' constant-argument-list ')'
///
/// Each argument in the constant-argument-list must be a constant expression.
///
/// Returns true if parsing or semantic analysis fail.
ExprResult Parser::ParseCXXConcatenateExpression() {
  assert(Tok.is(tok::kw___concatenate));
  SourceLocation KeyLoc = ConsumeToken();

  BalancedDelimiterTracker Parens(*this, tok::l_paren);
  if (Parens.expectAndConsume())
    return ExprError();

  SmallVector<Expr *, 4> Parts;
  do {
    ExprResult Expr = ParseConditionalExpression();
    if (Expr.isInvalid()) {
      Parens.skipToEnd();
      return ExprError();
    }
    Parts.push_back(Expr.get());
  } while (TryConsumeToken(tok::comma));

  if (Parens.consumeClose())
    return ExprError();

  return Actions.ActOnCXXConcatenateExpr(Parts, KeyLoc,
                                         Parens.getOpenLocation(),
                                         Parens.getCloseLocation());
}
