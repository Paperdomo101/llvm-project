//===--- ParseStmt.cpp - Statement and Block Parser -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Statement and Block portions of the Parser
// interface.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/PrettyDeclStackTrace.h"
#include "clang/Basic/Attributes.h"
#include "clang/Basic/PrettyStackTrace.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Parse/LoopHint.h"
#include "clang/Parse/Parser.h"
#include "clang/Parse/RAIIObjectsForParser.h"
#include "clang/Sema/DeclSpec.h"
#include "clang/Sema/EnterExpressionEvaluationContext.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/SemaCodeCompletion.h"
#include "clang/Sema/SemaObjC.h"
#include "clang/Sema/SemaOpenACC.h"
#include "clang/Sema/SemaOpenMP.h"
#include "clang/Sema/TypoCorrection.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include <optional>

using namespace clang;

//===----------------------------------------------------------------------===//
// C99 6.8: Statements and Blocks.
//===----------------------------------------------------------------------===//

StmtResult Parser::ParseStatement(SourceLocation *TrailingElseLoc,
                                  ParsedStmtContext StmtCtx,
                                  LabelDecl *PrecedingLabel) {
  StmtResult Res;

  // We may get back a null statement if we found a #pragma. Keep going until
  // we get an actual statement.
  StmtVector Stmts;
  do {
    Res = ParseStatementOrDeclaration(Stmts, StmtCtx, TrailingElseLoc,
                                      PrecedingLabel);
  } while (!Res.isInvalid() && !Res.get());

  return Res;
}





bool Parser::ParseLHSVar(LHSVarInfo &Out, bool SuppressDiags) {
  Out = LHSVarInfo();

  // Optional 'const' and 'static' in any order
  while (true) {
    if (Tok.is(tok::kw_const)) {
      Out.IsConst = true;
      ConsumeToken();
    } else if (Tok.is(tok::kw_static)) {
      Out.IsStatic = true;
      ConsumeToken();
    } else {
      break;
    }
  }

  // Optional '[]' (unsized) or '[N]' (sized) before identifier (C4)
  if (Tok.is(tok::l_square)) {
    if (NextToken().is(tok::r_square)) {
      // Unsized [] C4 array prefix
      Out.Kind = ArrayKind::C4;
      ConsumeBracket(); // '['
      ConsumeBracket(); // ']'
    } else if (((NextToken().is(tok::numeric_constant) || NextToken().is(tok::identifier)) &&
                PP.LookAhead(1).is(tok::r_square)) ||
               (NextToken().is(tok::hashdot) &&
                PP.LookAhead(1).is(tok::identifier) &&
                PP.LookAhead(2).is(tok::r_square)) ||
               (NextToken().is(tok::hashhash) &&
                PP.LookAhead(1).is(tok::period) &&
                PP.LookAhead(2).is(tok::identifier) &&
                PP.LookAhead(3).is(tok::r_square))) {
      // Sized [N] or [size] or [#.board] or [##.board] C4 array prefix: parse the size expression during real
      // parsing; in tentative mode (SuppressDiags=true) just consume tokens.
      Out.Kind = ArrayKind::C4;
      ConsumeBracket(); // '['
      if (!SuppressDiags) {
        // Parse the size expression the same way ParseDeclarationSpecifiers
        // does for '[N]' declarations.
        ExprResult SizeResult = ParseAssignmentExpression();
        if (SizeResult.isUsable())
          Out.SizeExpr = SizeResult.get();
      } else {
        if (Tok.is(tok::hashhash)) {
          ConsumeToken(); // consume '##'
          ConsumeToken(); // consume '.'
          ConsumeToken(); // consume identifier
        } else {
          ConsumeToken(); // consume the numeric literal, identifier or hashdot token
          if (Tok.is(tok::identifier))
            ConsumeToken();
        }
      }
      if (Tok.is(tok::r_square))
        ConsumeBracket(); // ']'
    }
  }

  // Expect an identifier
  if (Tok.isNot(tok::identifier)) {
    if (!SuppressDiags && !Tok.is(tok::eof))
      Diag(Tok.getLocation(), diag::err_expected_identifier_after_qualifier);
    return false;
  }
  Out.Ident = Tok.getIdentifierInfo();
  Out.IdentLoc = ConsumeToken();

  // Optional '[]' after identifier (C) – only if not already set
  if (Out.Kind == ArrayKind::None) {
    if (Tok.is(tok::l_square) && NextToken().is(tok::r_square)) {
      Out.Kind = ArrayKind::C;
      ConsumeBracket();
      ConsumeBracket();
    }
  }

  return true;
}

// C4

bool Parser::isTypeInferredAssignment() {
  if (!isC4File())
    return false;

  TentativeParsingAction TPA(*this);

  while (true) {
    LHSVarInfo Var;
    if (!ParseLHSVar(Var, /*SuppressDiags=*/true)) {
      TPA.Revert();
      return false;
    }
    if (Tok.is(tok::comma)) {
      ConsumeToken();
      continue;
    }
    bool result = Tok.is(tok::colonequal);
    TPA.Revert();
    return result;
  }
}

StmtResult Parser::ParseTypeInferredAssignment() {
  SmallVector<LHSVarInfo, 4> LHSVars;

  while (true) {
    LHSVarInfo Var;
    if (!ParseLHSVar(Var, /*SuppressDiags=*/false)) {
      SkipUntil(tok::semi, StopAtSemi);
      return StmtError();
    }
    LHSVars.push_back(Var);

    if (Tok.is(tok::comma)) {
      ConsumeToken();
      if (Tok.is(tok::eof)) {
        Diag(Tok.getLocation(), diag::err_expected_expression);
        return StmtError();
      }
      continue;
    } else {
      break;
    }
  }


  // Now we must see ':='
  if (Tok.isNot(tok::colonequal)) {
    Diag(Tok.getLocation(), diag::err_expected_colon_equal);
    SkipUntil(tok::semi, StopAtSemi);
    return StmtError();
  }
  SourceLocation ColonEqualLoc = ConsumeToken();

  // Parse RHS expression
  ExprResult InitExpr = ParseAssignmentExpression();
  if (InitExpr.isInvalid() || !InitExpr.isUsable() || !InitExpr.get()) {
    SkipUntil(tok::semi, StopAtSemi);
    return StmtError();
  }

  // ---- Destructuring / swizzle count logic (unchanged) ----
  size_t SwizzleCount = 1;
  if (LHSVars.size() > 1) {
    Expr *E = InitExpr.get()->IgnoreImplicit();
    if (auto *CE = dyn_cast<CastExpr>(E))
      E = CE->getSubExpr()->IgnoreImplicit();
    else if (auto *CLE = dyn_cast<CompoundLiteralExpr>(E))
      E = CLE->getInitializer()->IgnoreImplicit();

    if (auto *ILE = dyn_cast<InitListExpr>(E))
      SwizzleCount = ILE->getNumInits();
    else if (auto *PLE = dyn_cast<ParenListExpr>(E))
      SwizzleCount = PLE->getNumExprs();
    else if (E->getType()->isStructureType() || E->getType()->isUnionType()) {
      if (const auto *RD = E->getType()->getAsRecordDecl()) {
        SwizzleCount = 0;
        for (auto *D : RD->decls())
          if (isa<FieldDecl>(D))
            ++SwizzleCount;
      }
    } else {
      SwizzleCount = 1;
    }
    // If still unknown, defer to Sema (e.g., for multi-return calls).
    if (SwizzleCount == 0)
      SwizzleCount = LHSVars.size();
  }

  if (LHSVars.size() != SwizzleCount) {
    Diag(ColonEqualLoc, diag::err_swizzle_variable_count_mismatch)
      << (int)SwizzleCount << (int)LHSVars.size()
      << SourceRange(LHSVars.front().IdentLoc, InitExpr.get()->getEndLoc());
    SkipUntil(tok::semi, StopAtSemi);
    return StmtError();
  }

  // ---- Call Sema actions ----
  if (LHSVars.size() == 1) {
    if (InitExpr.get()->getType().isNull()) {
      SkipUntil(tok::semi, StopAtSemi);
      return StmtError();
    }
    // Your updated ActOnTypeInferredAssignment expects LHSVarInfo
    return Actions.ActOnTypeInferredAssignment(getCurScope(), LHSVars[0], InitExpr.get());
  } else {
    return Actions.ActOnMultiTypeInferredAssignment(getCurScope(), LHSVars, InitExpr.get());
  }
}




StmtResult Parser::ParseStatementOrDeclaration(StmtVector &Stmts,
                                               ParsedStmtContext StmtCtx,
                                               SourceLocation *TrailingElseLoc,
                                               LabelDecl *PrecedingLabel) {
  ParenBraceBracketBalancer BalancerRAIIObj(*this);

  // Look ahead to see if this statement starts an inferred assignment.
  // We check if it's an identifier followed immediately by ':=' OR a comma ','

  // C4: Check for enum declaration before type-inferred assignment
  if (getLangOpts().C4() && Tok.is(tok::identifier)) {
    if (isC4EnumDeclaration()) {
      SourceLocation DeclEnd;
      OpaquePtr<DeclGroupRef> DG = ParseC4EnumDeclaration(&DeclEnd);
      if (DG) {
        StmtResult S = Actions.ActOnDeclStmt(DG, Tok.getLocation(), DeclEnd);
        if (S.isUsable()) {
          Stmts.push_back(S.get());
          return StmtResult();
        }
      }
      SkipUntil(tok::semi, StopAtSemi);
      return StmtError();
    }
  }

  if (Tok.isOneOf(tok::kw_const, tok::kw_static, tok::l_square, tok::identifier)) {
      if (isTypeInferredAssignment()) {
        StmtResult AssignmentRes = ParseTypeInferredAssignment();
        if (AssignmentRes.isUsable()) {
          Stmts.push_back(AssignmentRes.get());
          return StmtResult(); // Return empty to prevent duplicate injection
        }
        // On error, consume up to semicolon and propagate error
        SkipUntil(tok::semi, StopAtSemi);
        return AssignmentRes;
      }
    }




  // Because we're parsing either a statement or a declaration, the order of
  // attribute parsing is important. [[]] attributes at the start of a
  // statement are different from [[]] attributes that follow an __attribute__
  // at the start of the statement. Thus, we're not using MaybeParseAttributes
  // here because we don't want to allow arbitrary orderings.
  ParsedAttributes CXX11Attrs(AttrFactory);
  bool HasStdAttr =
      MaybeParseCXX11Attributes(CXX11Attrs, /*MightBeObjCMessageSend*/ true);
  ParsedAttributes GNUOrMSAttrs(AttrFactory);
  if (getLangOpts().OpenCL)
    MaybeParseGNUAttributes(GNUOrMSAttrs);

  if (getLangOpts().HLSL)
    MaybeParseMicrosoftAttributes(GNUOrMSAttrs);

  StmtResult Res = ParseStatementOrDeclarationAfterAttributes(
      Stmts, StmtCtx, TrailingElseLoc, CXX11Attrs, GNUOrMSAttrs,
      PrecedingLabel);
  MaybeDestroyTemplateIds();

  takeAndConcatenateAttrs(CXX11Attrs, std::move(GNUOrMSAttrs));

  assert((CXX11Attrs.empty() || Res.isInvalid() || Res.isUsable()) &&
         "attributes on empty statement");

  if (HasStdAttr && getLangOpts().C23 &&
      (StmtCtx & ParsedStmtContext::AllowDeclarationsInC) ==
          ParsedStmtContext{} &&
      isa_and_present<NullStmt>(Res.get()))
    Diag(CXX11Attrs.Range.getBegin(), diag::warn_attr_in_secondary_block)
        << CXX11Attrs.Range;

  if (CXX11Attrs.empty() || Res.isInvalid())
    return Res;

  return Actions.ActOnAttributedStmt(CXX11Attrs, Res.get());
}

namespace {
class StatementFilterCCC final : public CorrectionCandidateCallback {
public:
  StatementFilterCCC(Token nextTok) : NextToken(nextTok) {
    WantTypeSpecifiers = nextTok.isOneOf(tok::l_paren, tok::less, tok::l_square,
                                         tok::identifier, tok::star, tok::amp);
    WantExpressionKeywords =
        nextTok.isOneOf(tok::l_paren, tok::identifier, tok::arrow, tok::period);
    WantRemainingKeywords =
        nextTok.isOneOf(tok::l_paren, tok::semi, tok::identifier, tok::l_brace);
    WantCXXNamedCasts = false;
  }

  bool ValidateCandidate(const TypoCorrection &candidate) override {
    if (FieldDecl *FD = candidate.getCorrectionDeclAs<FieldDecl>())
      return !candidate.getCorrectionSpecifier() || isa<ObjCIvarDecl>(FD);
    if (NextToken.is(tok::equal))
      return candidate.getCorrectionDeclAs<VarDecl>();
    if (NextToken.is(tok::period) &&
        candidate.getCorrectionDeclAs<NamespaceDecl>())
      return false;
    return CorrectionCandidateCallback::ValidateCandidate(candidate);
  }

  std::unique_ptr<CorrectionCandidateCallback> clone() override {
    return std::make_unique<StatementFilterCCC>(*this);
  }

private:
  Token NextToken;
};
}

StmtResult Parser::ParseStatementOrDeclarationAfterAttributes(
    StmtVector &Stmts, ParsedStmtContext StmtCtx,
    SourceLocation *TrailingElseLoc, ParsedAttributes &CXX11Attrs,
    ParsedAttributes &GNUAttrs, LabelDecl *PrecedingLabel) {
  const char *SemiError = nullptr;
  StmtResult Res;
  SourceLocation GNUAttributeLoc;

  // Cases in this switch statement should fall through if the parser expects
  // the token to end in a semicolon (in which case SemiError should be set),
  // or they directly 'return;' if not.
Retry:
  tok::TokenKind Kind  = Tok.getKind();
  SourceLocation AtLoc;
  switch (Kind) {
  case tok::at: // May be a @try or @throw statement
    {
      AtLoc = ConsumeToken();  // consume @
      return ParseObjCAtStatement(AtLoc, StmtCtx);
    }

  case tok::code_completion:
    cutOffParsing();
    Actions.CodeCompletion().CodeCompleteOrdinaryName(
        getCurScope(), SemaCodeCompletion::PCC_Statement);
    return StmtError();

  case tok::identifier:
  ParseIdentifier: {
    Token Next = NextToken();
    if (Next.is(tok::colon)) { // C99 6.8.1: labeled-statement
      // Both C++11 and GNU attributes preceding the label appertain to the
      // label, so put them in a single list to pass on to
      // ParseLabeledStatement().
      takeAndConcatenateAttrs(CXX11Attrs, std::move(GNUAttrs));

      // identifier ':' statement
      return ParseLabeledStatement(CXX11Attrs, StmtCtx);
    }
/*
    if (Next.is(tok::colonequal))
      return ParseInferredDeclaration();
*/

    // Look up the identifier, and typo-correct it to a keyword if it's not
    // found.
    if (Next.isNot(tok::coloncolon)) {
      // Try to limit which sets of keywords should be included in typo
      // correction based on what the next token is.
      StatementFilterCCC CCC(Next);
      if (TryAnnotateName(&CCC) == AnnotatedNameKind::Error) {
        // Handle errors here by skipping up to the next semicolon or '}', and
        // eat the semicolon if that's what stopped us.
        SkipUntil(tok::r_brace, StopAtSemi | StopBeforeMatch);
        if (Tok.is(tok::semi))
          ConsumeToken();
        return StmtError();
      }

      // If the identifier was annotated, try again.
      if (Tok.isNot(tok::identifier))
        goto Retry;
    }

    // Fall through
    [[fallthrough]];
  }

  // === C4 LANGUAGE EXTENSION: BOUNDS-CHECKED ARRAY DECLARATION LOOKAHEAD ===
  // Handles both [] (unsized) and [N] (sized with compile-time constant N).
  case tok::l_square: {
    bool IsC4Decl = false;

    if (NextToken().is(tok::r_square)) {
      // [] — unsized C4 array
      IsC4Decl = true;
    } else if (((NextToken().is(tok::numeric_constant) || NextToken().is(tok::identifier)) &&
                PP.LookAhead(1).is(tok::r_square)) ||
               (NextToken().is(tok::hashdot) &&
                PP.LookAhead(1).is(tok::identifier) &&
                PP.LookAhead(2).is(tok::r_square)) ||
               (NextToken().is(tok::hashhash) &&
                PP.LookAhead(1).is(tok::period) &&
                PP.LookAhead(2).is(tok::identifier) &&
                PP.LookAhead(3).is(tok::r_square))) {
      // [N], [size], [#.board] or [##.board] — sized C4 array.
      IsC4Decl = true;
    }

    if (IsC4Decl) {
      // Both C++11 and GNU attributes preceding the declaration appertain to it.
      takeAndConcatenateAttrs(CXX11Attrs, std::move(GNUAttrs));
      SourceLocation DeclStart = Tok.getLocation();
      SourceLocation DeclEnd;
      DeclGroupPtrTy Decl = ParseDeclaration(DeclaratorContext::Block, DeclEnd,
                                             CXX11Attrs, GNUAttrs);
      return Actions.ActOnDeclStmt(Decl, DeclStart, DeclEnd);
    }

    // Otherwise fall through:If it's a single '[', fall through to default to let it parse as a standard
    // C expression statement (e.g. an array access or ObjC message expression).
    [[fallthrough]];
  }
  // =========================================================================


  default: {
    if (getLangOpts().CPlusPlus && MaybeParseCXX11Attributes(CXX11Attrs, true))
      goto Retry;

    bool HaveAttrs = !CXX11Attrs.empty() || !GNUAttrs.empty();
    auto IsStmtAttr = [](ParsedAttr &Attr) { return Attr.isStmtAttr(); };
    bool AllAttrsAreStmtAttrs = llvm::all_of(CXX11Attrs, IsStmtAttr) &&
                                llvm::all_of(GNUAttrs, IsStmtAttr);
    // In C, the grammar production for statement (C23 6.8.1p1) does not allow
    // for declarations, which is different from C++ (C++23 [stmt.pre]p1). So
    // in C++, we always allow a declaration, but in C we need to check whether
    // we're in a statement context that allows declarations. e.g., in C, the
    // following is invalid: if (1) int x;
    if ((getLangOpts().CPlusPlus || getLangOpts().MicrosoftExt ||
         (StmtCtx & ParsedStmtContext::AllowDeclarationsInC) !=
             ParsedStmtContext()) &&
        ((GNUAttributeLoc.isValid() && !(HaveAttrs && AllAttrsAreStmtAttrs)) ||
         isDeclarationStatement())) {
      SourceLocation DeclStart = Tok.getLocation(), DeclEnd;
      DeclGroupPtrTy Decl;
      if (GNUAttributeLoc.isValid()) {
        DeclStart = GNUAttributeLoc;
        Decl = ParseDeclaration(DeclaratorContext::Block, DeclEnd, CXX11Attrs,
                                GNUAttrs, &GNUAttributeLoc);
      } else {
        Decl = ParseDeclaration(DeclaratorContext::Block, DeclEnd, CXX11Attrs,
                                GNUAttrs);
      }
      if (CXX11Attrs.Range.getBegin().isValid()) {
        // Order of C++11 and GNU attributes is may be arbitrary.
        DeclStart = GNUAttrs.Range.getBegin().isInvalid()
                        ? CXX11Attrs.Range.getBegin()
                        : std::min(CXX11Attrs.Range.getBegin(),
                                   GNUAttrs.Range.getBegin());
      } else if (GNUAttrs.Range.getBegin().isValid())
        DeclStart = GNUAttrs.Range.getBegin();
      return Actions.ActOnDeclStmt(Decl, DeclStart, DeclEnd);
    }

    if (Tok.is(tok::r_brace)) {
      Diag(Tok, diag::err_expected_statement);
      return StmtError();
    }

    switch (Tok.getKind()) {
#define TRANSFORM_TYPE_TRAIT_DEF(_, Trait) case tok::kw___##Trait:
#include "clang/Basic/TransformTypeTraits.def"
      if (NextToken().is(tok::less)) {
        Tok.setKind(tok::identifier);
        Diag(Tok, diag::ext_keyword_as_ident)
            << Tok.getIdentifierInfo()->getName() << 0;
        goto ParseIdentifier;
      }
      [[fallthrough]];
    default:
      return ParseExprStatement(StmtCtx);
    }
  }

  case tok::kw___attribute: {
    GNUAttributeLoc = Tok.getLocation();
    ParseGNUAttributes(GNUAttrs);
    goto Retry;
  }

  case tok::kw_template: {
    SourceLocation DeclEnd;
    ParseTemplateDeclarationOrSpecialization(DeclaratorContext::Block, DeclEnd,
                                             getAccessSpecifierIfPresent());
    return StmtError();
  }

  case tok::kw_case:                // C99 6.8.1: labeled-statement
    return ParseCaseStatement(StmtCtx);
  case tok::kw_default:             // C99 6.8.1: labeled-statement
    return ParseDefaultStatement(StmtCtx);

    case tok::l_brace: { // C99 6.8.2: compound-statement
        // C4
        // Lookahead: Check if this brace opens an expression instead of a block
        bool IsMethodDispatch = false;

        // We look ahead up to 1 token past the matching closing brace
        // to see if a '::' (tok::coloncolon) immediately follows it.
        // Skip this lookahead if the '{' comes from a macro expansion — a
        // macro-body '{' can't start a {expr}::method() dispatch, and the TPA
        // would push any active TokenLexer onto the stack, breaking the
        // expansion.
        if (Tok.getLocation().isFileID() &&
            GetLookAheadToken(1).isNot(tok::eof)) {
            // Create a tentative parsing guard so we can safely roll back the token stream
            TentativeParsingAction TPA(*this);

            bool OldMacro = PP.isMacroExpansionDisabled();
            PP.setMacroExpansionDisabled(true);

            BalancedDelimiterTracker T(*this, tok::l_brace);
            if (!T.consumeOpen()) {
                // Skip to the matching closing brace
                T.skipToEnd();

                // Check if the token immediately following '}' is '::'
                if (Tok.is(tok::coloncolon)) {
                    IsMethodDispatch = true;
                }
            }

            PP.setMacroExpansionDisabled(OldMacro);

            // Always revert the token stream back to the starting position
            TPA.Revert();
        }

        // Route to the correct parsing pipeline based on lookahead
        if (IsMethodDispatch) {
            ParsedStmtContext SubStmtCtx = ParsedStmtContext();
            return ParseExprStatement(SubStmtCtx);
        }
        // End C4

        return ParseCompoundStatement();
    }

  case tok::semi: {                 // C99 6.8.3p3: expression[opt] ';'
    bool HasLeadingEmptyMacro = Tok.hasLeadingEmptyMacro();
    return Actions.ActOnNullStmt(ConsumeToken(), HasLeadingEmptyMacro);
  }

  case tok::kw_if:                  // C99 6.8.4.1: if-statement
    return ParseIfStatement(TrailingElseLoc);
  case tok::kw_switch:              // C99 6.8.4.2: switch-statement
    return ParseSwitchStatement(TrailingElseLoc, PrecedingLabel);

  case tok::kw_while:               // C99 6.8.5.1: while-statement
    return ParseWhileStatement(TrailingElseLoc, PrecedingLabel);
  case tok::kw_do:                  // C99 6.8.5.2: do-statement
    Res = ParseDoStatement(PrecedingLabel);
    SemiError = "do/while";
    break;
  case tok::kw_for:                 // C99 6.8.5.3: for-statement
    return ParseForStatement(TrailingElseLoc, PrecedingLabel);

  case tok::kw_goto:                // C99 6.8.6.1: goto-statement
    Res = ParseGotoStatement();
    SemiError = "goto";
    break;
  case tok::kw_continue:            // C99 6.8.6.2: continue-statement
    Res = ParseContinueStatement();
    SemiError = "continue";
    break;
  case tok::kw_break:               // C99 6.8.6.3: break-statement
    Res = ParseBreakStatement();
    SemiError = "break";
    break;
  case tok::kw_return:              // C99 6.8.6.4: return-statement
    Res = ParseReturnStatement();
    SemiError = "return";
    break;
  case tok::kw_co_return:            // C++ Coroutines: co_return statement
    Res = ParseReturnStatement();
    SemiError = "co_return";
    break;

  // ---> C4: ->} defer
  case tok::arrow_r_brace:
  case tok::kw__Defer: // C defer TS: defer-statement
    return ParseDeferStatement(TrailingElseLoc);

  case tok::kw_asm: {
    for (const ParsedAttr &AL : CXX11Attrs)
      // Could be relaxed if asm-related regular keyword attributes are
      // added later.
      (AL.isRegularKeywordAttribute()
           ? Diag(AL.getRange().getBegin(), diag::err_keyword_not_allowed)
           : Diag(AL.getRange().getBegin(), diag::warn_attribute_ignored))
          << AL;
    // Prevent these from being interpreted as statement attributes later on.
    CXX11Attrs.clear();
    ProhibitAttributes(GNUAttrs);
    bool msAsm = false;
    Res = ParseAsmStatement(msAsm);
    if (msAsm) return Res;
    SemiError = "asm";
    break;
  }

  case tok::kw___if_exists:
  case tok::kw___if_not_exists:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    ParseMicrosoftIfExistsStatement(Stmts);
    // An __if_exists block is like a compound statement, but it doesn't create
    // a new scope.
    return StmtEmpty();

  case tok::kw_try:                 // C++ 15: try-block
    return ParseCXXTryBlock();

  case tok::kw___try:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    return ParseSEHTryBlock();

  case tok::kw___leave:
    Res = ParseSEHLeaveStatement();
    SemiError = "__leave";
    break;

  case tok::annot_pragma_vis:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaVisibility();
    return StmtEmpty();

  case tok::annot_pragma_pack:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaPack();
    return StmtEmpty();

  case tok::annot_pragma_msstruct:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaMSStruct();
    return StmtEmpty();

  case tok::annot_pragma_align:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaAlign();
    return StmtEmpty();

  case tok::annot_pragma_weak:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaWeak();
    return StmtEmpty();

  case tok::annot_pragma_weakalias:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaWeakAlias();
    return StmtEmpty();

  case tok::annot_pragma_redefine_extname:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaRedefineExtname();
    return StmtEmpty();

  case tok::annot_pragma_fp_contract:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    Diag(Tok, diag::err_pragma_file_or_compound_scope) << "fp_contract";
    ConsumeAnnotationToken();
    return StmtError();

  case tok::annot_pragma_fp:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    Diag(Tok, diag::err_pragma_file_or_compound_scope) << "clang fp";
    ConsumeAnnotationToken();
    return StmtError();

  case tok::annot_pragma_fenv_access:
  case tok::annot_pragma_fenv_access_ms:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    Diag(Tok, diag::err_pragma_file_or_compound_scope)
        << (Kind == tok::annot_pragma_fenv_access ? "STDC FENV_ACCESS"
                                                    : "fenv_access");
    ConsumeAnnotationToken();
    return StmtEmpty();

  case tok::annot_pragma_fenv_round:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    Diag(Tok, diag::err_pragma_file_or_compound_scope) << "STDC FENV_ROUND";
    ConsumeAnnotationToken();
    return StmtError();

  case tok::annot_pragma_cx_limited_range:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    Diag(Tok, diag::err_pragma_file_or_compound_scope)
        << "STDC CX_LIMITED_RANGE";
    ConsumeAnnotationToken();
    return StmtError();

  case tok::annot_pragma_float_control:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    Diag(Tok, diag::err_pragma_file_or_compound_scope) << "float_control";
    ConsumeAnnotationToken();
    return StmtError();

  case tok::annot_pragma_opencl_extension:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaOpenCLExtension();
    return StmtEmpty();

  case tok::annot_pragma_captured:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    return HandlePragmaCaptured();

  case tok::annot_pragma_openmp:
    // Prohibit attributes that are not OpenMP attributes, but only before
    // processing a #pragma omp clause.
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    [[fallthrough]];
  case tok::annot_attr_openmp:
    // Do not prohibit attributes if they were OpenMP attributes.
    return ParseOpenMPDeclarativeOrExecutableDirective(StmtCtx);

  case tok::annot_pragma_openacc:
    return ParseOpenACCDirectiveStmt();

  case tok::annot_pragma_ms_pointers_to_members:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaMSPointersToMembers();
    return StmtEmpty();

  case tok::annot_pragma_ms_pragma:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaMSPragma();
    return StmtEmpty();

  case tok::annot_pragma_ms_vtordisp:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaMSVtorDisp();
    return StmtEmpty();

  case tok::annot_pragma_loop_hint:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    return ParsePragmaLoopHint(Stmts, StmtCtx, TrailingElseLoc, CXX11Attrs,
                               PrecedingLabel);

  case tok::annot_pragma_dump:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaDump();
    return StmtEmpty();

  case tok::annot_pragma_attribute:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaAttribute();
    return StmtEmpty();
  case tok::annot_pragma_export:
    ProhibitAttributes(CXX11Attrs);
    ProhibitAttributes(GNUAttrs);
    HandlePragmaExport();
    return StmtEmpty();
  }

  // If we reached this code, the statement must end in a semicolon.
  if (!TryConsumeToken(tok::semi) && !TryConsumeOptionalSemi() && !Res.isInvalid()) {
    // If the result was valid, then we do want to diagnose this.  Use
    // ExpectAndConsume to emit the diagnostic, even though we know it won't
    // succeed.
    ExpectAndConsume(tok::semi, diag::err_expected_semi_after_stmt, SemiError);
    // Skip until we see a } or ;, but don't eat it.
    SkipUntil(tok::r_brace, StopAtSemi | StopBeforeMatch);
  }

  return Res;
}


StmtResult Parser::ParseExprStatement(ParsedStmtContext StmtCtx) {
  // If a case keyword is missing, this is where it should be inserted.
  Token OldToken = Tok;

  ExprStatementTokLoc = Tok.getLocation();

  // expression[opt] ';'
  ExprResult Expr(ParseExpression());
  if (Expr.isInvalid()) {
    // If the expression is invalid, skip ahead to the next semicolon or '}'.
    // Not doing this opens us up to the possibility of infinite loops if
    // ParseExpression does not consume any tokens.
    SkipUntil(tok::r_brace, StopAtSemi | StopBeforeMatch);
    if (Tok.is(tok::semi))
      ConsumeToken();
    return Actions.ActOnExprStmtError();
  }


  // --- C4: FIX FOR i.{x, y} := ... CRASH ---
  if (Tok.is(tok::colonequal)) {
      Diag(Tok.getLocation(), diag::err_swizzle_decl_unsupported)
      << Expr.get()->getSourceRange();
      SkipUntil(tok::semi, StopAtSemi);
      return StmtError();
  }
  // -----------------------------------------------



  auto *CaseE = Expr.get();
  if (Tok.is(tok::colon) &&
      (getCurScope()->isSwitchScope() ||
       (getLangOpts().C4() && Actions.getCurFunction() &&
        !Actions.getCurFunction()->SwitchStack.empty())) &&
      Actions.CheckCaseExpression(CaseE)) {
    Expr = CaseE;
    if (!getLangOpts().C4()) {
      // If a constant expression is followed by a colon inside a switch block,
      // suggest a missing case keyword.
      Diag(OldToken, diag::err_expected_case_before_expression)
        << FixItHint::CreateInsertion(OldToken.getLocation(), "case ");
    }

    // Recover parsing as a case statement.
    return ParseCaseStatement(StmtCtx, /*MissingCase=*/true, Expr);
  }

  Token *CurTok = nullptr;
  // If the semicolon is missing at the end of REPL input, we want to print
  // the result. Note we shouldn't eat the token since the callback needs it.
  if (Tok.is(tok::annot_repl_input_end))
    CurTok = &Tok;
  else
    // Otherwise, eat the semicolon.
    if (!TryConsumeOptionalSemi())
        ExpectAndConsumeSemi(diag::err_expected_semi_after_expr);

  StmtResult R = handleExprStmt(Expr, StmtCtx);
  if (CurTok && !R.isInvalid())
    CurTok->setAnnotationValue(R.get());

  return R;
}

StmtResult Parser::ParseSEHTryBlock() {
  assert(Tok.is(tok::kw___try) && "Expected '__try'");
  SourceLocation TryLoc = ConsumeToken();

  if (Tok.isNot(tok::l_brace))
    return StmtError(Diag(Tok, diag::err_expected) << tok::l_brace);

  StmtResult TryBlock(ParseCompoundStatement(
      /*isStmtExpr=*/false,
      Scope::DeclScope | Scope::CompoundStmtScope | Scope::SEHTryScope));
  if (TryBlock.isInvalid())
    return TryBlock;

  StmtResult Handler;
  if (Tok.is(tok::identifier) &&
      Tok.getIdentifierInfo() == getSEHExceptKeyword()) {
    SourceLocation Loc = ConsumeToken();
    Handler = ParseSEHExceptBlock(Loc);
  } else if (Tok.is(tok::kw___finally)) {
    SourceLocation Loc = ConsumeToken();
    Handler = ParseSEHFinallyBlock(Loc);
  } else {
    return StmtError(Diag(Tok, diag::err_seh_expected_handler));
  }

  if(Handler.isInvalid())
    return Handler;

  return Actions.ActOnSEHTryBlock(false /* IsCXXTry */,
                                  TryLoc,
                                  TryBlock.get(),
                                  Handler.get());
}

StmtResult Parser::ParseSEHExceptBlock(SourceLocation ExceptLoc) {
  PoisonIdentifierRAIIObject raii(Ident__exception_code, false),
    raii2(Ident___exception_code, false),
    raii3(Ident_GetExceptionCode, false);

  if (ExpectAndConsume(tok::l_paren))
    return StmtError();

  ParseScope ExpectScope(this, Scope::DeclScope | Scope::ControlScope |
                                   Scope::SEHExceptScope);

  if (getLangOpts().Borland) {
    Ident__exception_info->setIsPoisoned(false);
    Ident___exception_info->setIsPoisoned(false);
    Ident_GetExceptionInfo->setIsPoisoned(false);
  }

  ExprResult FilterExpr;
  {
    ParseScopeFlags FilterScope(this, getCurScope()->getFlags() |
                                          Scope::SEHFilterScope);
    FilterExpr = ParseExpression();
  }

  if (getLangOpts().Borland) {
    Ident__exception_info->setIsPoisoned(true);
    Ident___exception_info->setIsPoisoned(true);
    Ident_GetExceptionInfo->setIsPoisoned(true);
  }

  if(FilterExpr.isInvalid())
    return StmtError();

  if (ExpectAndConsume(tok::r_paren))
    return StmtError();

  if (Tok.isNot(tok::l_brace))
    return StmtError(Diag(Tok, diag::err_expected) << tok::l_brace);

  StmtResult Block(ParseCompoundStatement());

  if(Block.isInvalid())
    return Block;

  return Actions.ActOnSEHExceptBlock(ExceptLoc, FilterExpr.get(), Block.get());
}

StmtResult Parser::ParseSEHFinallyBlock(SourceLocation FinallyLoc) {
  PoisonIdentifierRAIIObject raii(Ident__abnormal_termination, false),
    raii2(Ident___abnormal_termination, false),
    raii3(Ident_AbnormalTermination, false);

  if (Tok.isNot(tok::l_brace))
    return StmtError(Diag(Tok, diag::err_expected) << tok::l_brace);

  ParseScope FinallyScope(this, 0);
  Actions.ActOnStartSEHFinallyBlock();

  StmtResult Block(ParseCompoundStatement());
  if(Block.isInvalid()) {
    Actions.ActOnAbortSEHFinallyBlock();
    return Block;
  }

  return Actions.ActOnFinishSEHFinallyBlock(FinallyLoc, Block.get());
}

/// Handle __leave
///
/// seh-leave-statement:
///   '__leave' ';'
///
StmtResult Parser::ParseSEHLeaveStatement() {
  SourceLocation LeaveLoc = ConsumeToken();  // eat the '__leave'.
  return Actions.ActOnSEHLeaveStmt(LeaveLoc, getCurScope());
}

static void DiagnoseLabelFollowedByDecl(Parser &P, const Stmt *SubStmt) {
  // When in C mode (but not Microsoft extensions mode), diagnose use of a
  // label that is followed by a declaration rather than a statement.
  if (!P.getLangOpts().CPlusPlus && !P.getLangOpts().MicrosoftExt &&
      isa<DeclStmt>(SubStmt)) {
    P.Diag(SubStmt->getBeginLoc(),
           P.getLangOpts().C23
               ? diag::warn_c23_compat_label_followed_by_declaration
               : diag::ext_c_label_followed_by_declaration);
  }
}

StmtResult Parser::ParseLabeledStatement(ParsedAttributes &Attrs,
                                         ParsedStmtContext StmtCtx) {
  assert(Tok.is(tok::identifier) && Tok.getIdentifierInfo() &&
         "Not an identifier!");

  // [OpenMP 5.1] 2.1.3: A stand-alone directive may not be used in place of a
  // substatement in a selection statement, in place of the loop body in an
  // iteration statement, or in place of the statement that follows a label.
  StmtCtx &= ~ParsedStmtContext::AllowStandaloneOpenMPDirectives;

  Token IdentTok = Tok;  // Save the whole token.
  ConsumeToken();  // eat the identifier.

  assert(Tok.is(tok::colon) && "Not a label!");

  // identifier ':' statement
  SourceLocation ColonLoc = ConsumeToken();

  LabelDecl *LD = Actions.LookupOrCreateLabel(IdentTok.getIdentifierInfo(),
                                              IdentTok.getLocation());

  // Read label attributes, if present.
  StmtResult SubStmt;
  if (Tok.is(tok::kw___attribute)) {
    ParsedAttributes TempAttrs(AttrFactory);
    ParseGNUAttributes(TempAttrs);

    // In C++, GNU attributes only apply to the label if they are followed by a
    // semicolon, to disambiguate label attributes from attributes on a labeled
    // declaration.
    //
    // This doesn't quite match what GCC does; if the attribute list is empty
    // and followed by a semicolon, GCC will reject (it appears to parse the
    // attributes as part of a statement in that case). That looks like a bug.
    if (!getLangOpts().CPlusPlus || Tok.is(tok::semi))
      Attrs.takeAllAppendingFrom(TempAttrs);
    else {
      StmtVector Stmts;
      ParsedAttributes EmptyCXX11Attrs(AttrFactory);
      SubStmt = ParseStatementOrDeclarationAfterAttributes(
          Stmts, StmtCtx, /*TrailingElseLoc=*/nullptr, EmptyCXX11Attrs,
          TempAttrs, LD);
      if (!TempAttrs.empty() && !SubStmt.isInvalid())
        SubStmt = Actions.ActOnAttributedStmt(TempAttrs, SubStmt.get());
    }
  }

  // The label may have no statement following it
  if (SubStmt.isUnset() && Tok.is(tok::r_brace)) {
    DiagnoseLabelAtEndOfCompoundStatement();
    SubStmt = Actions.ActOnNullStmt(ColonLoc);
  }

  // If we've not parsed a statement yet, parse one now.
  if (SubStmt.isUnset())
    SubStmt = ParseStatement(nullptr, StmtCtx, LD);

  // Broken substmt shouldn't prevent the label from being added to the AST.
  if (SubStmt.isInvalid())
    SubStmt = Actions.ActOnNullStmt(ColonLoc);

  DiagnoseLabelFollowedByDecl(*this, SubStmt.get());

  Actions.ProcessDeclAttributeList(Actions.CurScope, LD, Attrs);
  Attrs.clear();

  return Actions.ActOnLabelStmt(IdentTok.getLocation(), LD, ColonLoc,
                                SubStmt.get());
}

StmtResult Parser::ParseCaseStatement(ParsedStmtContext StmtCtx,
                                      bool MissingCase, ExprResult Expr) {
  assert((MissingCase || Tok.is(tok::kw_case)) && "Not a case stmt!");

  // [OpenMP 5.1] 2.1.3: A stand-alone directive may not be used in place of a
  // substatement in a selection statement, in place of the loop body in an
  // iteration statement, or in place of the statement that follows a label.
  StmtCtx &= ~ParsedStmtContext::AllowStandaloneOpenMPDirectives;

  // It is very common for code to contain many case statements recursively
  // nested, as in (but usually without indentation):
  //  case 1:
  //    case 2:
  //      case 3:
  //         case 4:
  //           case 5: etc.
  //
  // Parsing this naively works, but is both inefficient and can cause us to run
  // out of stack space in our recursive descent parser.  As a special case,
  // flatten this recursion into an iterative loop.  This is complex and gross,
  // but all the grossness is constrained to ParseCaseStatement (and some
  // weirdness in the actions), so this is just local grossness :).

  // TopLevelCase - This is the highest level we have parsed.  'case 1' in the
  // example above.
  StmtResult TopLevelCase(true);

  // DeepestParsedCaseStmt - This is the deepest statement we have parsed, which
  // gets updated each time a new case is parsed, and whose body is unset so
  // far.  When parsing 'case 4', this is the 'case 3' node.
  Stmt *DeepestParsedCaseStmt = nullptr;

  // While we have case statements, eat and stack them.
  SourceLocation ColonLoc;
  do {
    SourceLocation CaseLoc = MissingCase ? Expr.get()->getExprLoc() :
                                           ConsumeToken();  // eat the 'case'.
    ColonLoc = SourceLocation();

    if (Tok.is(tok::code_completion)) {
      cutOffParsing();
      Actions.CodeCompletion().CodeCompleteCase(getCurScope());
      return StmtError();
    }

    /// We don't want to treat 'case x : y' as a potential typo for 'case x::y'.
    /// Disable this form of error recovery while we're parsing the case
    /// expression.
    ColonProtectionRAIIObject ColonProtection(*this);

    ExprResult LHS;
    if (!MissingCase) {
      LHS = ParseCaseExpression(CaseLoc);
      if (LHS.isInvalid()) {
        // If constant-expression is parsed unsuccessfully, recover by skipping
        // current case statement (moving to the colon that ends it).
        if (!SkipUntil(tok::colon, tok::r_brace, StopAtSemi | StopBeforeMatch))
          return StmtError();
      }
    } else {
      LHS = Actions.ActOnCaseExpr(CaseLoc, Expr);
      MissingCase = false;
    }

    // GNU case range extension.
    SourceLocation DotDotDotLoc;
    ExprResult RHS;
    if (TryConsumeToken(tok::ellipsis, DotDotDotLoc)) {
      // In C++, this is a GNU extension. In C, it's a C2y extension.
      unsigned DiagId;
      if (getLangOpts().CPlusPlus)
        DiagId = diag::ext_gnu_case_range;
      else if (getLangOpts().C2y)
        DiagId = diag::warn_c23_compat_case_range;
      else
        DiagId = diag::ext_c2y_case_range;
      Diag(DotDotDotLoc, DiagId);
      RHS = ParseCaseExpression(CaseLoc);
      if (RHS.isInvalid()) {
        if (!SkipUntil(tok::colon, tok::r_brace, StopAtSemi | StopBeforeMatch))
          return StmtError();
      }
    }

    ColonProtection.restore();

    if (TryConsumeToken(tok::colon, ColonLoc)) {
    } else if (TryConsumeToken(tok::semi, ColonLoc) ||
               TryConsumeToken(tok::coloncolon, ColonLoc)) {
      // Treat "case blah;" or "case blah::" as a typo for "case blah:".
      Diag(ColonLoc, diag::err_expected_after)
          << "'case'" << tok::colon
          << FixItHint::CreateReplacement(ColonLoc, ":");
    } else {
      SourceLocation ExpectedLoc = getEndOfPreviousToken();

      Diag(ExpectedLoc, diag::err_expected_after)
          << "'case'" << tok::colon
          << FixItHint::CreateInsertion(ExpectedLoc, ":");

      ColonLoc = ExpectedLoc;
    }

    StmtResult Case =
        Actions.ActOnCaseStmt(CaseLoc, LHS, DotDotDotLoc, RHS, ColonLoc);

    // If we had a sema error parsing this case, then just ignore it and
    // continue parsing the sub-stmt.
    if (Case.isInvalid()) {
      if (TopLevelCase.isInvalid())  // No parsed case stmts.
        return ParseStatement(/*TrailingElseLoc=*/nullptr, StmtCtx);
      // Otherwise, just don't add it as a nested case.
    } else {
      // If this is the first case statement we parsed, it becomes TopLevelCase.
      // Otherwise we link it into the current chain.
      Stmt *NextDeepest = Case.get();
      if (TopLevelCase.isInvalid())
        TopLevelCase = Case;
      else
        Actions.ActOnCaseStmtBody(DeepestParsedCaseStmt, Case.get());
      DeepestParsedCaseStmt = NextDeepest;
    }

    // Handle all case statements.
  } while (Tok.is(tok::kw_case));

  // If we found a non-case statement, start by parsing it.
  StmtResult SubStmt;

  if (Tok.is(tok::r_brace)) {
    // "switch (X) { case 4: }", is valid and is treated as if label was
    // followed by a null statement.
    DiagnoseLabelAtEndOfCompoundStatement();
    SubStmt = Actions.ActOnNullStmt(ColonLoc);
  } else {
    SubStmt = ParseStatement(/*TrailingElseLoc=*/nullptr, StmtCtx);
  }

  // Install the body into the most deeply-nested case.
  if (DeepestParsedCaseStmt) {
    // Broken sub-stmt shouldn't prevent forming the case statement properly.
    if (SubStmt.isInvalid())
      SubStmt = Actions.ActOnNullStmt(SourceLocation());
    DiagnoseLabelFollowedByDecl(*this, SubStmt.get());
    Actions.ActOnCaseStmtBody(DeepestParsedCaseStmt, SubStmt.get());
  }

  // Return the top level parsed statement tree.
  return TopLevelCase;
}

StmtResult Parser::ParseDefaultStatement(ParsedStmtContext StmtCtx) {
  assert(Tok.is(tok::kw_default) && "Not a default stmt!");

  // [OpenMP 5.1] 2.1.3: A stand-alone directive may not be used in place of a
  // substatement in a selection statement, in place of the loop body in an
  // iteration statement, or in place of the statement that follows a label.
  StmtCtx &= ~ParsedStmtContext::AllowStandaloneOpenMPDirectives;

  SourceLocation DefaultLoc = ConsumeToken();  // eat the 'default'.

  SourceLocation ColonLoc;
  if (TryConsumeToken(tok::colon, ColonLoc)) {
  } else if (TryConsumeToken(tok::semi, ColonLoc)) {
    // Treat "default;" as a typo for "default:".
    Diag(ColonLoc, diag::err_expected_after)
        << "'default'" << tok::colon
        << FixItHint::CreateReplacement(ColonLoc, ":");
  } else {
    SourceLocation ExpectedLoc = PP.getLocForEndOfToken(PrevTokLocation);
    Diag(ExpectedLoc, diag::err_expected_after)
        << "'default'" << tok::colon
        << FixItHint::CreateInsertion(ExpectedLoc, ":");
    ColonLoc = ExpectedLoc;
  }

  StmtResult SubStmt;

  if (Tok.is(tok::r_brace)) {
    // "switch (X) {... default: }", is valid and is treated as if label was
    // followed by a null statement.
    DiagnoseLabelAtEndOfCompoundStatement();
    SubStmt = Actions.ActOnNullStmt(ColonLoc);
  } else {
    SubStmt = ParseStatement(/*TrailingElseLoc=*/nullptr, StmtCtx);
  }

  // Broken sub-stmt shouldn't prevent forming the case statement properly.
  if (SubStmt.isInvalid())
    SubStmt = Actions.ActOnNullStmt(ColonLoc);

  DiagnoseLabelFollowedByDecl(*this, SubStmt.get());
  return Actions.ActOnDefaultStmt(DefaultLoc, ColonLoc,
                                  SubStmt.get(), getCurScope());
}

StmtResult Parser::ParseCompoundStatement(bool isStmtExpr) {
  return ParseCompoundStatement(isStmtExpr,
                                Scope::DeclScope | Scope::CompoundStmtScope);
}

StmtResult Parser::ParseCompoundStatement(bool isStmtExpr,
                                          unsigned ScopeFlags) {
  assert(Tok.is(tok::l_brace) && "Not a compound stmt!");

  // Enter a scope to hold everything within the compound stmt.  Compound
  // statements can always hold declarations.
  ParseScope CompoundScope(this, ScopeFlags);

  // Parse the statements in the body.
  StmtResult R;
  StackHandler.runWithSufficientStackSpace(Tok.getLocation(), [&, this]() {
    R = ParseCompoundStatementBody(isStmtExpr);
  });
  return R;
}

void Parser::ParseCompoundStatementLeadingPragmas() {
  bool checkForPragmas = true;
  while (checkForPragmas) {
    switch (Tok.getKind()) {
    case tok::annot_pragma_vis:
      HandlePragmaVisibility();
      break;
    case tok::annot_pragma_pack:
      HandlePragmaPack();
      break;
    case tok::annot_pragma_msstruct:
      HandlePragmaMSStruct();
      break;
    case tok::annot_pragma_align:
      HandlePragmaAlign();
      break;
    case tok::annot_pragma_weak:
      HandlePragmaWeak();
      break;
    case tok::annot_pragma_weakalias:
      HandlePragmaWeakAlias();
      break;
    case tok::annot_pragma_redefine_extname:
      HandlePragmaRedefineExtname();
      break;
    case tok::annot_pragma_opencl_extension:
      HandlePragmaOpenCLExtension();
      break;
    case tok::annot_pragma_fp_contract:
      HandlePragmaFPContract();
      break;
    case tok::annot_pragma_fp:
      HandlePragmaFP();
      break;
    case tok::annot_pragma_fenv_access:
    case tok::annot_pragma_fenv_access_ms:
      HandlePragmaFEnvAccess();
      break;
    case tok::annot_pragma_fenv_round:
      HandlePragmaFEnvRound();
      break;
    case tok::annot_pragma_cx_limited_range:
      HandlePragmaCXLimitedRange();
      break;
    case tok::annot_pragma_float_control:
      HandlePragmaFloatControl();
      break;
    case tok::annot_pragma_ms_pointers_to_members:
      HandlePragmaMSPointersToMembers();
      break;
    case tok::annot_pragma_ms_pragma:
      HandlePragmaMSPragma();
      break;
    case tok::annot_pragma_ms_vtordisp:
      HandlePragmaMSVtorDisp();
      break;
    case tok::annot_pragma_dump:
      HandlePragmaDump();
      break;
    case tok::annot_pragma_export:
      HandlePragmaExport();
      break;
    default:
      checkForPragmas = false;
      break;
    }
  }

}

void Parser::DiagnoseLabelAtEndOfCompoundStatement() {
  if (getLangOpts().CPlusPlus) {
    Diag(Tok, getLangOpts().CPlusPlus23
                  ? diag::warn_cxx20_compat_label_end_of_compound_statement
                  : diag::ext_cxx_label_end_of_compound_statement);
  } else {
    Diag(Tok, getLangOpts().C23
                  ? diag::warn_c23_compat_label_end_of_compound_statement
                  : diag::ext_c_label_end_of_compound_statement);
  }
}

bool Parser::ConsumeNullStmt(StmtVector &Stmts) {
  if (!Tok.is(tok::semi))
    return false;

  SourceLocation StartLoc = Tok.getLocation();
  SourceLocation EndLoc;

  while (Tok.is(tok::semi) && !Tok.hasLeadingEmptyMacro() &&
         Tok.getLocation().isValid() && !Tok.getLocation().isMacroID()) {
    EndLoc = Tok.getLocation();

    // Don't just ConsumeToken() this tok::semi, do store it in AST.
    StmtResult R =
        ParseStatementOrDeclaration(Stmts, ParsedStmtContext::SubStmt);
    if (R.isUsable())
      Stmts.push_back(R.get());
  }

  // Did not consume any extra semi.
  if (EndLoc.isInvalid())
    return false;

  Diag(StartLoc, diag::warn_null_statement)
      << FixItHint::CreateRemoval(SourceRange(StartLoc, EndLoc));
  return true;
}

StmtResult Parser::handleExprStmt(ExprResult E, ParsedStmtContext StmtCtx) {
  bool IsStmtExprResult = false;
  if ((StmtCtx & ParsedStmtContext::InStmtExpr) != ParsedStmtContext()) {
    // Look ahead to see if the next two tokens close the statement expression;
    // if so, this expression statement is the last statement in a
    // statment expression.
    IsStmtExprResult = Tok.is(tok::r_brace) && NextToken().is(tok::r_paren);
  }

  if (IsStmtExprResult)
    E = Actions.ActOnStmtExprResult(E);
  return Actions.ActOnExprStmt(E, /*DiscardedValue=*/!IsStmtExprResult);
}

StmtResult Parser::ParseCompoundStatementBody(bool isStmtExpr) {
  PrettyStackTraceLoc CrashInfo(PP.getSourceManager(),
                                Tok.getLocation(),
                                "in compound statement ('{}')");

  // Record the current FPFeatures, restore on leaving the
  // compound statement.
  Sema::FPFeaturesStateRAII SaveFPFeatures(Actions);

  InMessageExpressionRAIIObject InMessage(*this, false);
  BalancedDelimiterTracker T(*this, tok::l_brace);
  if (T.consumeOpen())
    return StmtError();

  Sema::CompoundScopeRAII CompoundScope(Actions, isStmtExpr);

  // Parse any pragmas at the beginning of the compound statement.
  ParseCompoundStatementLeadingPragmas();
  Actions.ActOnAfterCompoundStatementLeadingPragmas();

  StmtVector Stmts;

  // "__label__ X, Y, Z;" is the GNU "Local Label" extension.  These are
  // only allowed at the start of a compound stmt regardless of the language.
  while (Tok.is(tok::kw___label__)) {
    SourceLocation LabelLoc = ConsumeToken();

    SmallVector<Decl *, 4> DeclsInGroup;
    while (true) {
      if (Tok.isNot(tok::identifier)) {
        Diag(Tok, diag::err_expected) << tok::identifier;
        break;
      }

      IdentifierInfo *II = Tok.getIdentifierInfo();
      SourceLocation IdLoc = ConsumeToken();
      DeclsInGroup.push_back(Actions.LookupOrCreateLabel(II, IdLoc, LabelLoc));

      if (!TryConsumeToken(tok::comma))
        break;
    }

    DeclSpec DS(AttrFactory);
    DeclGroupPtrTy Res =
        Actions.FinalizeDeclaratorGroup(getCurScope(), DS, DeclsInGroup);
    StmtResult R = Actions.ActOnDeclStmt(Res, LabelLoc, Tok.getLocation());

    if (!TryConsumeOptionalSemi())
        ExpectAndConsumeSemi(diag::err_expected_semi_declaration);

    if (R.isUsable())
      Stmts.push_back(R.get());
  }

  ParsedStmtContext SubStmtCtx =
      ParsedStmtContext::Compound |
      (isStmtExpr ? ParsedStmtContext::InStmtExpr : ParsedStmtContext());

  bool LastIsError = false;
  while (!tryParseMisplacedModuleImport() && Tok.isNot(tok::r_brace) &&
         Tok.isNot(tok::eof)) {
    if (Tok.is(tok::annot_pragma_unused)) {
      HandlePragmaUnused();
      continue;
    }

    if (ConsumeNullStmt(Stmts))
      continue;

    StmtResult R;
    if (Tok.isNot(tok::kw___extension__)) {
      R = ParseStatementOrDeclaration(Stmts, SubStmtCtx);
    } else {
      // __extension__ can start declarations and it can also be a unary
      // operator for expressions.  Consume multiple __extension__ markers here
      // until we can determine which is which.
      // FIXME: This loses extension expressions in the AST!
      SourceLocation ExtLoc = ConsumeToken();
      while (Tok.is(tok::kw___extension__))
        ConsumeToken();

      ParsedAttributes attrs(AttrFactory);
      MaybeParseCXX11Attributes(attrs, /*MightBeObjCMessageSend*/ true);

      // If this is the start of a declaration, parse it as such.
      if (isDeclarationStatement()) {
        // __extension__ silences extension warnings in the subdeclaration.
        // FIXME: Save the __extension__ on the decl as a node somehow?
        ExtensionRAIIObject O(Diags);

        SourceLocation DeclStart = Tok.getLocation(), DeclEnd;
        ParsedAttributes DeclSpecAttrs(AttrFactory);
        DeclGroupPtrTy Res = ParseDeclaration(DeclaratorContext::Block, DeclEnd,
                                              attrs, DeclSpecAttrs);
        R = Actions.ActOnDeclStmt(Res, DeclStart, DeclEnd);
      } else {
        // Otherwise this was a unary __extension__ marker.
        ExprResult Res(ParseExpressionWithLeadingExtension(ExtLoc));

        if (Res.isInvalid()) {
          SkipUntil(tok::semi);
          continue;
        }

        // Eat the semicolon at the end of stmt and convert the expr into a
        // statement.
        if (!TryConsumeOptionalSemi())
            ExpectAndConsumeSemi(diag::err_expected_semi_after_expr);

        R = handleExprStmt(Res, SubStmtCtx);
        if (R.isUsable())
          R = Actions.ActOnAttributedStmt(attrs, R.get());
      }
    }

    // C4: check for @error { ... } / @error(e) { ... } suffix handler
    if (getLangOpts().C4() &&
        Tok.is(tok::at) &&
        GetLookAheadToken(1).is(tok::identifier) &&
        GetLookAheadToken(1).getIdentifierInfo() &&
        GetLookAheadToken(1).getIdentifierInfo()->isStr("error")) {
      if (R.isUsable()) {
        R = ParseC4ErrorHandlerSuffix(R.get());
      } else if (!Stmts.empty()) {
        Stmt *LastStmt = Stmts.pop_back_val();
        StmtResult Wrapped = ParseC4ErrorHandlerSuffix(LastStmt);
        if (Wrapped.isUsable()) {
          Stmts.push_back(Wrapped.get());
        } else {
          Stmts.push_back(LastStmt);
        }
      }
    }

    if (R.isUsable())
      Stmts.push_back(R.get());
    LastIsError = R.isInvalid();
  }
  // StmtExpr needs to do copy initialization for last statement.
  // If last statement is invalid, the last statement in `Stmts` will be
  // incorrect. Then the whole compound statement should also be marked as
  // invalid to prevent subsequent errors.
  if (isStmtExpr && LastIsError && !Stmts.empty())
    return StmtError();

  // Warn the user that using option `-ffp-eval-method=source` on a
  // 32-bit target and feature `sse` disabled, or using
  // `pragma clang fp eval_method=source` and feature `sse` disabled, is not
  // supported.
  if (!PP.getTargetInfo().supportSourceEvalMethod() &&
      (PP.getLastFPEvalPragmaLocation().isValid() ||
       PP.getCurrentFPEvalMethod() ==
           LangOptions::FPEvalMethodKind::FEM_Source))
    Diag(Tok.getLocation(),
         diag::warn_no_support_for_eval_method_source_on_m32);

  SourceLocation CloseLoc = Tok.getLocation();

  // We broke out of the while loop because we found a '}' or EOF.
  if (!T.consumeClose()) {
    // If this is the '})' of a statement expression, check that it's written
    // in a sensible way.
    if (isStmtExpr && Tok.is(tok::r_paren))
      checkCompoundToken(CloseLoc, tok::r_brace, CompoundToken::StmtExprEnd);
  } else {
    // Recover by creating a compound statement with what we parsed so far,
    // instead of dropping everything and returning StmtError().
  }

  if (T.getCloseLocation().isValid())
    CloseLoc = T.getCloseLocation();

  return Actions.ActOnCompoundStmt(T.getOpenLocation(), CloseLoc,
                                   Stmts, isStmtExpr);
}

bool Parser::ParseParenExprOrCondition(StmtResult *InitStmt,
                                       Sema::ConditionResult &Cond,
                                       SourceLocation Loc,
                                       Sema::ConditionKind CK,
                                       SourceLocation &LParenLoc,
                                       SourceLocation &RParenLoc) {
  BalancedDelimiterTracker T(*this, tok::l_paren);
  T.consumeOpen();
  SourceLocation Start = Tok.getLocation();

  if (getLangOpts().CPlusPlus) {
    Cond = ParseCXXCondition(InitStmt, Loc, CK, false);
  } else {
    ExprResult CondExpr = ParseExpression();

    // If required, convert to a boolean value.
    if (CondExpr.isInvalid())
      Cond = Sema::ConditionError();
    else
      Cond = Actions.ActOnCondition(getCurScope(), Loc, CondExpr.get(), CK,
                                    /*MissingOK=*/false);
  }

  // If the parser was confused by the condition and we don't have a ')', try to
  // recover by skipping ahead to a semi and bailing out.  If condexp is
  // semantically invalid but we have well formed code, keep going.
  if (Cond.isInvalid() && Tok.isNot(tok::r_paren)) {
    SkipUntil(tok::semi);
    // Skipping may have stopped if it found the containing ')'.  If so, we can
    // continue parsing the if statement.
    if (Tok.isNot(tok::r_paren))
      return true;
  }

  if (Cond.isInvalid()) {
    ExprResult CondExpr = Actions.CreateRecoveryExpr(
        Start, Tok.getLocation() == Start ? Start : PrevTokLocation, {},
        Actions.PreferredConditionType(CK));
    if (!CondExpr.isInvalid())
      Cond = Actions.ActOnCondition(getCurScope(), Loc, CondExpr.get(), CK,
                                    /*MissingOK=*/false);
  }

  // Either the condition is valid or the rparen is present.
  T.consumeClose();
  LParenLoc = T.getOpenLocation();
  RParenLoc = T.getCloseLocation();

  // Check for extraneous ')'s to catch things like "if (foo())) {".  We know
  // that all callers are looking for a statement after the condition, so ")"
  // isn't valid.
  while (Tok.is(tok::r_paren)) {
    Diag(Tok, diag::err_extraneous_rparen_in_condition)
      << FixItHint::CreateRemoval(Tok.getLocation());
    ConsumeParen();
  }

  return false;
}

namespace {

enum MisleadingStatementKind { MSK_if, MSK_else, MSK_for, MSK_while };

struct MisleadingIndentationChecker {
  Parser &P;
  SourceLocation StmtLoc;
  SourceLocation PrevLoc;
  unsigned NumDirectives;
  MisleadingStatementKind Kind;
  bool ShouldSkip;
  MisleadingIndentationChecker(Parser &P, MisleadingStatementKind K,
                               SourceLocation SL)
      : P(P), StmtLoc(SL), PrevLoc(P.getCurToken().getLocation()),
        NumDirectives(P.getPreprocessor().getNumDirectives()), Kind(K),
        ShouldSkip(P.getCurToken().is(tok::l_brace)) {
    if (!P.MisleadingIndentationElseLoc.isInvalid()) {
      StmtLoc = P.MisleadingIndentationElseLoc;
      P.MisleadingIndentationElseLoc = SourceLocation();
    }
    if (Kind == MSK_else && !ShouldSkip)
      P.MisleadingIndentationElseLoc = SL;
  }

  /// Compute the column number will aligning tabs on TabStop (-ftabstop), this
  /// gives the visual indentation of the SourceLocation.
  static unsigned getVisualIndentation(SourceManager &SM, SourceLocation Loc) {
    unsigned TabStop = SM.getDiagnostics().getDiagnosticOptions().TabStop;

    unsigned ColNo = SM.getSpellingColumnNumber(Loc);
    if (ColNo == 0 || TabStop == 1)
      return ColNo;

    FileIDAndOffset FIDAndOffset = SM.getDecomposedLoc(Loc);

    bool Invalid;
    StringRef BufData = SM.getBufferData(FIDAndOffset.first, &Invalid);
    if (Invalid)
      return 0;

    const char *EndPos = BufData.data() + FIDAndOffset.second;
    // FileOffset are 0-based and Column numbers are 1-based
    assert(FIDAndOffset.second + 1 >= ColNo &&
           "Column number smaller than file offset?");

    unsigned VisualColumn = 0; // Stored as 0-based column, here.
    // Loop from beginning of line up to Loc's file position, counting columns,
    // expanding tabs.
    for (const char *CurPos = EndPos - (ColNo - 1); CurPos != EndPos;
         ++CurPos) {
      if (*CurPos == '\t')
        // Advance visual column to next tabstop.
        VisualColumn += (TabStop - VisualColumn % TabStop);
      else
        VisualColumn++;
    }
    return VisualColumn + 1;
  }

  void Check() {
    Token Tok = P.getCurToken();
    if (P.getActions().getDiagnostics().isIgnored(
            diag::warn_misleading_indentation, Tok.getLocation()) ||
        ShouldSkip || NumDirectives != P.getPreprocessor().getNumDirectives() ||
        Tok.isOneOf(tok::semi, tok::r_brace) || Tok.isAnnotation() ||
        Tok.getLocation().isMacroID() || PrevLoc.isMacroID() ||
        StmtLoc.isMacroID() ||
        (Kind == MSK_else && P.MisleadingIndentationElseLoc.isInvalid())) {
      P.MisleadingIndentationElseLoc = SourceLocation();
      return;
    }
    if (Kind == MSK_else)
      P.MisleadingIndentationElseLoc = SourceLocation();

    SourceManager &SM = P.getPreprocessor().getSourceManager();
    unsigned PrevColNum = getVisualIndentation(SM, PrevLoc);
    unsigned CurColNum = getVisualIndentation(SM, Tok.getLocation());
    unsigned StmtColNum = getVisualIndentation(SM, StmtLoc);

    if (PrevColNum != 0 && CurColNum != 0 && StmtColNum != 0 &&
        ((PrevColNum > StmtColNum && PrevColNum == CurColNum) ||
         !Tok.isAtStartOfLine()) &&
        SM.getPresumedLineNumber(StmtLoc) !=
            SM.getPresumedLineNumber(Tok.getLocation()) &&
        (Tok.isNot(tok::identifier) ||
         P.getPreprocessor().LookAhead(0).isNot(tok::colon))) {
      P.Diag(Tok.getLocation(), diag::warn_misleading_indentation) << Kind;
      P.Diag(StmtLoc, diag::note_previous_statement);
    }
  }
};

}



// C4: Common condition parser for if and switch.
Parser::ParsedCondition Parser::ParseCondition(SourceLocation StmtLoc,
                                               Sema::ConditionKind Kind) {
  ParsedCondition Result;
  Result.Cond = Actions.ConditionError();
  Result.HasDelimitedCondition = false;

  // In C++ mode, delegate entirely to the original Clang condition parser.
  // C4's tentative-parse approach doesn't understand C++17 condition
  // declarations ("if (auto x = expr)") or init-statements
  // ("if (init; cond)"), and the C4 isTypeSpecifier list includes 'auto'
  // which would misdirect C++ declaration-conditions into the init-statement
  // path.  ParseParenExprOrCondition handles all C++ cases correctly.
  if (getLangOpts().CPlusPlus) {
    Result.HasDelimitedCondition = true; // C++ always requires parens
    if (ParseParenExprOrCondition(&Result.InitStmt, Result.Cond, StmtLoc,
                                  Kind, Result.LParen, Result.RParen)) {
      // ParseParenExprOrCondition already emitted diagnostics; signal error
      // by leaving Cond in its default (invalid) state.
    }
    return Result;
  }

  // C4: Treat `!(...)` as an explicitly delimited condition even though
  // it does not begin with '('.
  if (Tok.is(tok::exclaim) && GetLookAheadToken(1).is(tok::l_paren))
    Result.HasDelimitedCondition = true;

  // Helper to check if a token is a type specifier.
  // IMPORTANT: This must be side-effect-free – it must NOT call
  // TryAnnotateTypeOrScopeToken or any function that mutates the token stream,
  // because it is called during clangd code-completion where the preprocessor
  // runs in CachingLex mode and token-stream mutation causes crashes.
  auto isTypeSpecifier = [&](const Token &Tok) -> bool {
    // C4-specific unambiguous declaration prefix tokens.
    // ^  -> caret pointer declaration (e.g. `if ^char p = 0; ...`)
    // [] -> C4 bounds-checked array (e.g. `if []char arr; ...`)
    // Neither can appear as the start of an expression.
    if (getLangOpts().C4()) {
      if (Tok.is(tok::caret))
        return true;
      if (Tok.is(tok::l_square) && NextToken().is(tok::r_square))
        return true;
    }
    // Standard C/C4 type keyword specifiers.
    if (Tok.isOneOf(tok::kw_int, tok::kw_float, tok::kw_double,
                    tok::kw_char, tok::kw_void, tok::kw_short,
                    tok::kw_long, tok::kw_signed, tok::kw_unsigned,
                    tok::kw__Bool, tok::kw__Complex, tok::kw___vector,
                    tok::kw___attribute, tok::kw_auto,
                    tok::kw_struct, tok::kw_union, tok::kw_enum))
      return true;

    // User-defined types (typedef names)
    if (Tok.is(tok::identifier)) {
      if (Actions.getTypeName(*Tok.getIdentifierInfo(), Tok.getLocation(), getCurScope())) {
        if (NextToken().isOneOf(tok::period, tok::coloncolon))
          return false;
        return true;
      }
    }

    return false;
  };


  // C4: Parenthesized condition – no tentative parsing.
  if (Tok.is(tok::l_paren)) {
      StmtResult TmpInit;
      SourceLocation TmpLParen, TmpRParen;

      TmpLParen = ConsumeParen(); // consume '('

      // Parse optional init (:= or declaration) if present.
      // C4: multi-return destructuring init (e.g. if (a,b := expr; cond))
      if (getLangOpts().C4() && Tok.is(tok::identifier) &&
          GetLookAheadToken(1).is(tok::comma) && isTypeInferredAssignment()) {
          TmpInit = ParseTypeInferredAssignment();
          if (TmpInit.isInvalid())
              return Result;
          if (Tok.is(tok::semi))
              ConsumeToken();
          else {
              Diag(Tok, diag::err_expected_semi_after_expr);
              return Result;
          }
      } else if (Tok.is(tok::identifier) && GetLookAheadToken(1).is(tok::colonequal)) {
          TmpInit = ParseTypeInferredAssignment();
          if (TmpInit.isInvalid()) {
              return Result;
          }
          // Semicolon required after assignment init.
          if (Tok.is(tok::semi)) {
              ConsumeToken();
          } else {
              Diag(Tok, diag::err_expected_semi_after_expr);
              return Result;
          }
      } else if (isTypeSpecifier(Tok)) {
          SourceLocation DeclStart = Tok.getLocation();
          ParsedAttributes DeclAttrs(AttrFactory);
          ParsedAttributes DeclSpecAttrs(AttrFactory);
          SourceLocation DeclEnd;
          DeclGroupPtrTy DG = ParseDeclaration(DeclaratorContext::SelectionInit,
                                               DeclEnd, DeclAttrs, DeclSpecAttrs,
                                               /*DeclSpecStart=*/nullptr);
          if (!DG) {
              Diag(Tok, diag::err_expected) << tok::semi;
              return Result;
          }
          TmpInit = Actions.ActOnDeclStmt(DG, DeclStart, DeclEnd);
          if (TmpInit.isInvalid()) {
              return Result;
          }
          // Declaration parsing already consumed the ';'.
      }

      // Parse the condition expression.
      ExprResult CondExpr = ParseExpression();
      if (CondExpr.isInvalid()) {
          return Result;
      }

      // Expect closing ')'
      if (Tok.is(tok::r_paren)) {
          TmpRParen = ConsumeParen();
      } else {
          // err_expected_rparen_after requires a %0 argument; use err_expected
          // with r_paren to avoid a format-argument assertion in clangd.
          Diag(Tok, diag::err_expected) << tok::r_paren;
          return Result;
      }

      // Commit – parentheses are the sole delimiter.
      Result.HasDelimitedCondition = true;
      Result.InitStmt = std::move(TmpInit);
      Result.LParen = TmpLParen;
      Result.RParen = TmpRParen;
      Result.Cond = Actions.ActOnCondition(getCurScope(), StmtLoc,
                                           CondExpr.get(), Kind);
      if (Result.Cond.isInvalid())
          return Result;
      return Result;
  }

  // Unparenthesized condition (no '(' seen).
  // C4: multi-return destructuring init (e.g. if a,b := expr; cond)
  if (getLangOpts().C4() && Tok.is(tok::identifier) &&
      GetLookAheadToken(1).is(tok::comma) && isTypeInferredAssignment()) {
      Result.InitStmt = ParseTypeInferredAssignment();
      if (Result.InitStmt.isInvalid())
          return Result;
      if (Tok.is(tok::semi))
          ConsumeToken();
      else {
          Diag(Tok, diag::err_expected_semi_declaration);
          return Result;
      }
  }
  // Handle `:=` assignment as init.
  else if (Tok.is(tok::identifier) && GetLookAheadToken(1).is(tok::colonequal)) {
      Result.InitStmt = ParseTypeInferredAssignment();
      if (Result.InitStmt.isInvalid()) {
          return Result;
      }
      if (Tok.is(tok::semi)) {
          ConsumeToken();
      } else {
          Diag(Tok, diag::err_expected_semi_declaration);
          return Result;
      }
  }
  // Handle declaration init (e.g., `int c = 4;`).
  else if (isTypeSpecifier(Tok)) {
      SourceLocation DeclStart = Tok.getLocation();
      ParsedAttributes DeclAttrs(AttrFactory);
      ParsedAttributes DeclSpecAttrs(AttrFactory);
      SourceLocation DeclEnd;
      DeclGroupPtrTy DG = ParseDeclaration(DeclaratorContext::SelectionInit,
                                           DeclEnd,
                                           DeclAttrs,
                                           DeclSpecAttrs,
                                           /*DeclSpecStart=*/nullptr);
      if (!DG) {
          Diag(Tok, diag::err_declaration_does_not_declare_param);
          return Result;
      }
      Result.InitStmt = Actions.ActOnDeclStmt(DG, DeclStart, DeclEnd);
      if (Result.InitStmt.isInvalid())
          return Result;
      // Declaration parsing already consumed the ';'.
  }

  // Parse the condition expression.
  ExprResult CondExpr = ParseExpression();
  if (CondExpr.isInvalid()) {
      return Result;
  }

  Result.Cond = Actions.ActOnCondition(getCurScope(), StmtLoc,
                                       CondExpr.get(), Kind);
  // Result.HasDelimitedCondition remains false (set by default).
  return Result;
}


StmtResult Parser::ParseIfStatement(SourceLocation *TrailingElseLoc) {
  assert(Tok.is(tok::kw_if) && "Not an if stmt!");
  SourceLocation IfLoc = ConsumeToken();

  bool IsConstexpr = false;
  bool IsConsteval = false;
  SourceLocation NotLocation;
  SourceLocation ConstevalLoc;

  // (constexpr/consteval handling – unchanged from your working code)
  if (Tok.is(tok::kw_constexpr)) {
    if (getLangOpts().CPlusPlus) {
      Diag(Tok, getLangOpts().CPlusPlus17 ? diag::warn_cxx14_compat_constexpr_if
                                          : diag::ext_constexpr_if);
      IsConstexpr = true;
      ConsumeToken();
    }
  } else {
    if (Tok.is(tok::exclaim) && GetLookAheadToken(1).is(tok::kw_consteval)) {
      NotLocation = ConsumeToken();
    }
    if (Tok.is(tok::kw_consteval)) {
      Diag(Tok, getLangOpts().CPlusPlus23 ? diag::warn_cxx20_compat_consteval_if
                                          : diag::ext_consteval_if);
      IsConsteval = true;
      ConstevalLoc = ConsumeToken();
    } else if (Tok.is(tok::code_completion)) {
      // In C4 mode the if-condition is an expression, not necessarily wrapped
      // in parens.  CodeCompleteKeywordAfterIf only suggests constexpr/consteval
      // which are irrelevant in C4.  Skip the early return so that
      // ParseCondition → ParseExpression → code_completion fires expression-level
      // completions (variables, functions, etc.) just like `if (` does.
      if (!getLangOpts().C4()) {
        cutOffParsing();
        Actions.CodeCompletion().CodeCompleteKeywordAfterIf(NotLocation.isValid());
        return StmtError();
      }
      // C4: fall through — ParseCondition below handles the completion token.
    }
  }

  bool C99orCXX = getLangOpts().C99 || getLangOpts().CPlusPlus;
  ParseScope IfScope(this, Scope::DeclScope | Scope::ControlScope, C99orCXX);

  std::optional<bool> ConstexprCondition;

  // C4: Parse the condition using the common helper.
  SourceLocation CondStartLoc = Tok.getLocation();
  ParsedCondition Parsed = ParseCondition(
      IfLoc,
      IsConstexpr ? Sema::ConditionKind::ConstexprIf : Sema::ConditionKind::Boolean);
  if (Parsed.Cond.isInvalid() || Parsed.InitStmt.isInvalid()) {
    ExprResult Recovery = Actions.CreateRecoveryExpr(CondStartLoc, Tok.getLocation(), {});
    if (Recovery.isInvalid())
      return StmtError();
    // Determine ConditionKind (same as passed to ParseCondition).
    Sema::ConditionKind CK = IsConstexpr ? Sema::ConditionKind::ConstexprIf
                                          : Sema::ConditionKind::Boolean;
    Parsed.Cond = Actions.ActOnCondition(
        getCurScope(),
        IfLoc,
        Recovery.get(),
        CK,
        Parsed.InitStmt.get());   // may be null
    // If InitStmt was invalid, clear it (already handled above).
    if (Parsed.InitStmt.isInvalid())
      Parsed.InitStmt = StmtResult();
    if (Parsed.RParen.isInvalid())
      Parsed.RParen = Tok.getLocation();
  }

  if (IsConstexpr)
    ConstexprCondition = Parsed.Cond.getKnownValue();

  // C4: Enforce braces only if condition is NOT delimited.
  bool IsBracedThen = Tok.is(tok::l_brace);
  if (!Parsed.HasDelimitedCondition && !IsBracedThen) {
    Diag(Tok, diag::err_expected) << tok::l_brace;
    SkipUntil(tok::semi);
    return StmtError();
  }

  // C99 6.8.4p3 - In C99, the body of the if statement is a scope, even if
  // there is no compound stmt.  C90 does not have this clause.  We only do this
  // if the body isn't a compound statement to avoid push/pop in common cases.
  //
  // C++ 6.4p1:
  // The substatement in a selection-statement (each substatement, in the else
  // form of the if statement) implicitly defines a local scope.
  //
  // For C++ we create a scope for the condition and a new scope for
  // substatements because:
  // -When the 'then' scope exits, we want the condition declaration to still be
  //    active for the 'else' scope too.
  // -Sema will detect name clashes by considering declarations of a
  //    'ControlScope' as part of its direct subscope.
  // -If we wanted the condition and substatement to be in the same scope, we
  //    would have to notify ParseStatement not to create a new scope. It's
  //    simpler to let it create a new scope.
  //
  ParseScope InnerScope(this, Scope::DeclScope, C99orCXX, IsBracedThen);

  MisleadingIndentationChecker MIChecker(*this, MSK_if, IfLoc);

  // Read the 'then' stmt.
  SourceLocation ThenStmtLoc = Tok.getLocation();

  SourceLocation InnerStatementTrailingElseLoc;
  StmtResult ThenStmt;
  {
    bool ShouldEnter = ConstexprCondition && !*ConstexprCondition;
    Sema::ExpressionEvaluationContext Context =
        Sema::ExpressionEvaluationContext::DiscardedStatement;
    if (NotLocation.isInvalid() && IsConsteval) {
      Context = Sema::ExpressionEvaluationContext::ImmediateFunctionContext;
      ShouldEnter = true;
    }

    EnterExpressionEvaluationContext PotentiallyDiscarded(
        Actions, Context, nullptr,
        Sema::ExpressionEvaluationContextRecord::EK_Other, ShouldEnter);
    ThenStmt = ParseStatement(&InnerStatementTrailingElseLoc);
  }

  if (Tok.isNot(tok::kw_else) && !(getLangOpts().C4() && Tok.is(tok::kw_or)))
    MIChecker.Check();

  // Pop the 'if' scope if needed.
  InnerScope.Exit();

  // If it has an else, parse it.
  SourceLocation ElseLoc;
  SourceLocation ElseStmtLoc;
  StmtResult ElseStmt;

  if (Tok.is(tok::kw_else)) {
    if (TrailingElseLoc)
      *TrailingElseLoc = Tok.getLocation();

    ElseLoc = ConsumeToken();
    ElseStmtLoc = Tok.getLocation();

    // C99 6.8.4p3 - In C99, the body of the if statement is a scope, even if
    // there is no compound stmt.  C90 does not have this clause.  We only do
    // this if the body isn't a compound statement to avoid push/pop in common
    // cases.
    //
    // C++ 6.4p1:
    // The substatement in a selection-statement (each substatement, in the else
    // form of the if statement) implicitly defines a local scope.
    //
    ParseScope InnerScope(this, Scope::DeclScope, C99orCXX,
                          Tok.is(tok::l_brace));

    MisleadingIndentationChecker MIChecker(*this, MSK_else, ElseLoc);
    bool ShouldEnter = ConstexprCondition && *ConstexprCondition;
    Sema::ExpressionEvaluationContext Context =
        Sema::ExpressionEvaluationContext::DiscardedStatement;
    if (NotLocation.isValid() && IsConsteval) {
      Context = Sema::ExpressionEvaluationContext::ImmediateFunctionContext;
      ShouldEnter = true;
    }

    EnterExpressionEvaluationContext PotentiallyDiscarded(
        Actions, Context, nullptr,
        Sema::ExpressionEvaluationContextRecord::EK_Other, ShouldEnter);
    ElseStmt = ParseStatement();

    if (ElseStmt.isUsable())
      MIChecker.Check();

    // Pop the 'else' scope if needed.
    InnerScope.Exit();
  } else if (getLangOpts().C4() && Tok.is(tok::kw_or)) {
    // C4 'or' chain — syntactic sugar for else-if / else chains.
    if (TrailingElseLoc)
      *TrailingElseLoc = Tok.getLocation();
    ElseLoc = Tok.getLocation();
    ElseStmtLoc = Tok.getLocation();

    struct OrClause {
      SourceLocation OrLoc;
      ParsedCondition Cond;
      StmtResult Body;
      bool HasCond = false;
    };
    SmallVector<OrClause, 4> Clauses;

    while (Tok.is(tok::kw_or)) {
      OrClause C;
      C.OrLoc = ConsumeToken();

      if (Tok.is(tok::l_brace)) {
        ParseScope OrScope(this, Scope::DeclScope, C99orCXX, true);
        C.Body = ParseCompoundStatementBody();
        Clauses.push_back(C);
        break;
      }

      C.Cond = ParseCondition(C.OrLoc, Sema::ConditionKind::Boolean);
      C.HasCond = true;
      if (C.Cond.Cond.isInvalid() || C.Cond.InitStmt.isInvalid()) {
        IfScope.Exit();
        return StmtError();
      }

      if (!Tok.is(tok::l_brace)) {
        Diag(Tok, diag::err_expected) << tok::l_brace;
        SkipUntil(tok::semi);
        IfScope.Exit();
        return StmtError();
      }

      {
        ParseScope OrScope(this, Scope::DeclScope, C99orCXX, true);
        C.Body = ParseCompoundStatementBody();
      }
      if (C.Body.isInvalid()) {
        IfScope.Exit();
        return StmtError();
      }

      Clauses.push_back(C);
    }

    if (Clauses.empty()) {
      ElseStmt = StmtError();
    } else {
      OrClause &Last = Clauses.back();
      if (Last.HasCond) {
        ElseStmt = Actions.ActOnIfStmt(
            Last.OrLoc, IfStatementKind::Ordinary,
            Last.Cond.LParen, Last.Cond.InitStmt.get(),
            Last.Cond.Cond, Last.Cond.RParen,
            Last.Body.get(), SourceLocation(), nullptr);
      } else {
        ElseStmt = Last.Body;
      }

      for (int i = (int)Clauses.size() - 2; i >= 0; --i) {
        OrClause &C = Clauses[i];
        ElseStmt = Actions.ActOnIfStmt(
            C.OrLoc, IfStatementKind::Ordinary,
            C.Cond.LParen, C.Cond.InitStmt.get(),
            C.Cond.Cond, C.Cond.RParen,
            C.Body.get(), C.OrLoc, ElseStmt.get());
      }
    }

    if (ElseStmt.isInvalid())
      ElseStmt = Actions.ActOnNullStmt(ElseStmtLoc);
  } else if (Tok.is(tok::code_completion)) {
    cutOffParsing();
    Actions.CodeCompletion().CodeCompleteAfterIf(getCurScope(), IsBracedThen);
    return StmtError();
  } else if (InnerStatementTrailingElseLoc.isValid()) {
    Diag(InnerStatementTrailingElseLoc, diag::warn_dangling_else);
  }

  IfScope.Exit();

  // If the then or else stmt is invalid and the other is valid (and present),
  // turn the invalid one into a null stmt to avoid dropping the other
  // part.  If both are invalid, return error.
  if ((ThenStmt.isInvalid() && ElseStmt.isInvalid()) ||
      (ThenStmt.isInvalid() && ElseStmt.get() == nullptr) ||
      (ThenStmt.get() == nullptr && ElseStmt.isInvalid())) {
    // Both invalid, or one is invalid and other is non-present: return error.
    return StmtError();
  }

  if (IsConsteval) {
    auto IsCompoundStatement = [](const Stmt *S) {
      if (const auto *Outer = dyn_cast_if_present<AttributedStmt>(S))
        S = Outer->getSubStmt();
      return isa_and_nonnull<clang::CompoundStmt>(S);
    };

    if (!IsCompoundStatement(ThenStmt.get())) {
      Diag(ConstevalLoc, diag::err_expected_after) << "consteval"
                                                   << "{";
      return StmtError();
    }
    if (!ElseStmt.isUnset() && !IsCompoundStatement(ElseStmt.get())) {
      Diag(ElseLoc, diag::err_expected_after) << "else"
                                              << "{";
      return StmtError();
    }
  }

  // Now if either are invalid, replace with a ';'.
  if (ThenStmt.isInvalid())
    ThenStmt = Actions.ActOnNullStmt(ThenStmtLoc);
  if (ElseStmt.isInvalid())
    ElseStmt = Actions.ActOnNullStmt(ElseStmtLoc);

  IfStatementKind Kind = IfStatementKind::Ordinary;
  if (IsConstexpr)
    Kind = IfStatementKind::Constexpr;
  else if (IsConsteval)
    Kind = NotLocation.isValid() ? IfStatementKind::ConstevalNegated
                                 : IfStatementKind::ConstevalNonNegated;

  return Actions.ActOnIfStmt(IfLoc, Kind, Parsed.LParen, Parsed.InitStmt.get(),
                               Parsed.Cond, Parsed.RParen,
                               ThenStmt.get(), ElseLoc, ElseStmt.get());
}


StmtResult Parser::ParseSwitchStatement(SourceLocation *TrailingElseLoc,
                                        LabelDecl *PrecedingLabel) {
  assert(Tok.is(tok::kw_switch) && "Not a switch stmt!");
  SourceLocation SwitchLoc = ConsumeToken();

  bool C99orCXX = getLangOpts().C99 || getLangOpts().CPlusPlus;
  unsigned ScopeFlags = Scope::SwitchScope;
  if (C99orCXX)
    ScopeFlags |= Scope::DeclScope | Scope::ControlScope;
  ParseScope SwitchScope(this, ScopeFlags, /*ManageAll=*/true);

  // C4: Parse the condition using the common helper.
  ParsedCondition Parsed = ParseCondition(SwitchLoc, Sema::ConditionKind::Switch);
  if (Parsed.Cond.isInvalid() || Parsed.InitStmt.isInvalid())
    return StmtError();

  // C4: Enforce braces only if condition is NOT delimited.
  bool IsBracedThen = Tok.is(tok::l_brace);
  if (!Parsed.HasDelimitedCondition && !IsBracedThen) {
    Diag(Tok, diag::err_expected) << tok::l_brace;
    SkipUntil(tok::semi);
    return StmtError();
  }

  StmtResult Switch = Actions.ActOnStartOfSwitchStmt(
      SwitchLoc, Parsed.LParen, Parsed.InitStmt.get(), Parsed.Cond, Parsed.RParen);

  if (Switch.isInvalid()) {
    if (Tok.is(tok::l_brace)) {
      ConsumeBrace();
      SkipUntil(tok::r_brace);
    } else
      SkipUntil(tok::semi);
    return Switch;
  }

  getCurScope()->EnterSwitchBody(PrecedingLabel);
  ParseScope InnerScope(this, Scope::DeclScope, C99orCXX, Tok.is(tok::l_brace));
  if (C99orCXX)
    getCurScope()->decrementMSManglingNumber();

  bool IsC4File = isC4File();

  StmtResult Body;
  if (IsC4File) {
    if (Tok.isNot(tok::l_brace)) {
      Diag(Tok, diag::err_expected) << tok::l_brace;
      Body = StmtError();
    } else {
      BalancedDelimiterTracker T(*this, tok::l_brace);
      T.consumeOpen();
      
      SmallVector<Stmt *, 16> BodyStmts;
      SmallVector<Stmt *, 8> DefaultSubStmts;
      SourceLocation DefaultLoc;
      
      while (Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof)) {
        if (Tok.is(tok::kw_default) || (Tok.is(tok::identifier) && Tok.getIdentifierInfo()->getName() == "default" && NextToken().is(tok::colon))) {
          if (!DefaultSubStmts.empty()) {
            if (DefaultLoc.isInvalid()) DefaultLoc = DefaultSubStmts.front()->getBeginLoc();
            StmtResult DefaultBody = Actions.ActOnCompoundStmt(DefaultLoc, DefaultLoc, DefaultSubStmts, false);
            if (DefaultBody.isUsable()) {
              CompoundStmt *CS = cast<CompoundStmt>(DefaultBody.get());
              bool EndsWithExit = false;
              if (CS->size() > 0) {
                Stmt *Last = CS->body_back();
                if (isa<BreakStmt>(Last) || isa<ReturnStmt>(Last) || isa<GotoStmt>(Last))
                  EndsWithExit = true;
              }
              if (!EndsWithExit) {
                SmallVector<Stmt *, 8> CSStmts(CS->body().begin(), CS->body().end());
                StmtResult Break = Actions.ActOnBreakStmt(DefaultLoc, getCurScope(), nullptr, SourceLocation());
                if (Break.isUsable()) CSStmts.push_back(Break.get());
                DefaultBody = Actions.ActOnCompoundStmt(DefaultLoc, DefaultLoc, CSStmts, false);
              }
              StmtResult DefaultCase = Actions.ActOnDefaultStmt(DefaultLoc, DefaultLoc, DefaultBody.get(), getCurScope());
              if (DefaultCase.isUsable()) BodyStmts.push_back(DefaultCase.get());
            }
            DefaultSubStmts.clear();
            DefaultLoc = SourceLocation();
          }

          SourceLocation DefaultLoc = ConsumeToken();
          ExpectAndConsume(tok::colon, diag::err_expected);
          
          StmtResult DefaultBody;
          if (Tok.is(tok::l_brace)) {
            BalancedDelimiterTracker CS_Braces(*this, tok::l_brace);
            CS_Braces.consumeOpen();
            
            ParseScope CompoundScope(this, Scope::DeclScope);
            
            StmtVector CSStmts;
            while (Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof)) {
              StmtResult R = ParseStatementOrDeclaration(CSStmts, ParsedStmtContext::Compound);
              if (R.isUsable()) CSStmts.push_back(R.get());
            }
            CS_Braces.consumeClose();
            
            bool EndsWithExit = false;
            if (!CSStmts.empty()) {
              Stmt *Last = CSStmts.back();
              if (isa<BreakStmt>(Last) || isa<ReturnStmt>(Last) || isa<GotoStmt>(Last)) {
                EndsWithExit = true;
              }
            }
            
            StmtVector TrailingStmts;
            while (Tok.isNot(tok::kw_case) && Tok.isNot(tok::kw_default) && Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof) && !isC4CaseLabel()) {
              StmtResult R = ParseStatementOrDeclaration(TrailingStmts, ParsedStmtContext::Compound);
              if (R.isUsable()) TrailingStmts.push_back(R.get());
            }
            
            bool TrailingEndsWithExit = false;
            if (!TrailingStmts.empty()) {
              Stmt *Last = TrailingStmts.back();
              if (isa<BreakStmt>(Last) || isa<ReturnStmt>(Last) || isa<GotoStmt>(Last)) {
                TrailingEndsWithExit = true;
              }
            }
            
            if (!EndsWithExit && !TrailingEndsWithExit) {
              StmtResult Break = Actions.ActOnBreakStmt(DefaultLoc, getCurScope(), nullptr, SourceLocation());
              if (Break.isUsable()) CSStmts.push_back(Break.get());
            }
            
            CSStmts.insert(CSStmts.end(), TrailingStmts.begin(), TrailingStmts.end());
            DefaultBody = Actions.ActOnCompoundStmt(CS_Braces.getOpenLocation(), CS_Braces.getCloseLocation(), CSStmts, false);
          } else {
            StmtVector Stmts;
            while (Tok.isNot(tok::kw_case) && Tok.isNot(tok::kw_default) && Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof) && !isC4CaseLabel()) {
              StmtResult R = ParseStatementOrDeclaration(Stmts, ParsedStmtContext::Compound);
              if (R.isUsable()) Stmts.push_back(R.get());
            }
            DefaultBody = Actions.ActOnCompoundStmt(DefaultLoc, DefaultLoc, Stmts, false);
          }
          
          if (DefaultBody.isUsable()) {
            StmtResult DefaultCase = Actions.ActOnDefaultStmt(DefaultLoc, DefaultLoc, DefaultBody.get(), getCurScope());
            if (DefaultCase.isUsable()) BodyStmts.push_back(DefaultCase.get());
          }
        }
        else if (Tok.is(tok::kw_case) || isC4CaseLabel()) {
          if (!DefaultSubStmts.empty()) {
            if (DefaultLoc.isInvalid()) DefaultLoc = DefaultSubStmts.front()->getBeginLoc();
            StmtResult DefaultBody = Actions.ActOnCompoundStmt(DefaultLoc, DefaultLoc, DefaultSubStmts, false);
            if (DefaultBody.isUsable()) {
              CompoundStmt *CS = cast<CompoundStmt>(DefaultBody.get());
              bool EndsWithExit = false;
              if (CS->size() > 0) {
                Stmt *Last = CS->body_back();
                if (isa<BreakStmt>(Last) || isa<ReturnStmt>(Last) || isa<GotoStmt>(Last))
                  EndsWithExit = true;
              }
              if (!EndsWithExit) {
                SmallVector<Stmt *, 8> CSStmts(CS->body().begin(), CS->body().end());
                StmtResult Break = Actions.ActOnBreakStmt(DefaultLoc, getCurScope(), nullptr, SourceLocation());
                if (Break.isUsable()) CSStmts.push_back(Break.get());
                DefaultBody = Actions.ActOnCompoundStmt(DefaultLoc, DefaultLoc, CSStmts, false);
              }
              StmtResult DefaultCase = Actions.ActOnDefaultStmt(DefaultLoc, DefaultLoc, DefaultBody.get(), getCurScope());
              if (DefaultCase.isUsable()) BodyStmts.push_back(DefaultCase.get());
            }
            DefaultSubStmts.clear();
            DefaultLoc = SourceLocation();
          }
          
          SourceLocation CaseLoc = Tok.getLocation();
          bool HasCaseKW = TryConsumeToken(tok::kw_case);
          
          struct C4CaseValue {
            ExprResult LHS;
            SourceLocation DotDotLoc;
            ExprResult RHS;
            bool HalfOpen;
          };
          SmallVector<C4CaseValue, 4> Values;
          
          do {
            ExprResult LHS = ParseCaseExpression(CaseLoc);
            if (LHS.isInvalid()) {
              SkipUntil(tok::colon, tok::r_brace, StopAtSemi | StopBeforeMatch);
              break;
            }
            
            SourceLocation DotDotLoc;
            ExprResult RHS;
            bool HalfOpen = false;
            if ((Tok.is(tok::period) && NextToken().is(tok::period)) ||
                Tok.is(tok::ellipsis)) {
              if (Tok.is(tok::period)) {
                DotDotLoc = ConsumeToken();
                ConsumeToken();
              } else {
                DotDotLoc = ConsumeToken();
              }
              if (Tok.is(tok::less)) {
                HalfOpen = true;
                ConsumeToken();
              }
              RHS = ParseCaseExpression(CaseLoc);
              if (RHS.isInvalid()) {
                SkipUntil(tok::colon, tok::r_brace, StopAtSemi | StopBeforeMatch);
                break;
              }
            }
            
            Values.push_back({LHS, DotDotLoc, RHS, HalfOpen});
          } while (TryConsumeToken(tok::comma));
          
          SourceLocation ColonLoc;
          if (!TryConsumeToken(tok::colon, ColonLoc)) {
            Diag(Tok, diag::err_expected) << tok::colon;
            SkipUntil(tok::colon, tok::r_brace, StopAtSemi | StopBeforeMatch);
            TryConsumeToken(tok::colon);
          }
          
          StmtResult CaseBody;
          if (Tok.is(tok::l_brace)) {
            BalancedDelimiterTracker CS_Braces(*this, tok::l_brace);
            CS_Braces.consumeOpen();
            
            ParseScope CompoundScope(this, Scope::DeclScope);
            
            StmtVector CSStmts;
            bool EndsWithContinue = false;
            
            while (Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof)) {
              if (Tok.is(tok::kw_continue) && NextToken().is(tok::semi)) {
                if (PP.LookAhead(1).is(tok::r_brace)) {
                  EndsWithContinue = true;
                  ConsumeToken(); // Consume 'continue'
                  ConsumeToken(); // Consume ';'
                  continue;
                }
              }
              
              StmtResult R = ParseStatementOrDeclaration(CSStmts, ParsedStmtContext::Compound);
              if (R.isUsable()) CSStmts.push_back(R.get());
            }
            
            CS_Braces.consumeClose();
            
            bool EndsWithExit = false;
            if (!CSStmts.empty()) {
              Stmt *Last = CSStmts.back();
              if (isa<BreakStmt>(Last) || isa<ReturnStmt>(Last) || isa<GotoStmt>(Last)) {
                EndsWithExit = true;
              }
            }
            
            StmtVector TrailingStmts;
            if (HasCaseKW) {
              while (Tok.isNot(tok::kw_case) && Tok.isNot(tok::kw_default) && Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof) && !isC4CaseLabel()) {
                StmtResult R = ParseStatementOrDeclaration(TrailingStmts, ParsedStmtContext::Compound);
                if (R.isUsable()) TrailingStmts.push_back(R.get());
              }
            }
            
            bool TrailingEndsWithExit = false;
            if (!TrailingStmts.empty()) {
              Stmt *Last = TrailingStmts.back();
              if (isa<BreakStmt>(Last) || isa<ReturnStmt>(Last) || isa<GotoStmt>(Last)) {
                TrailingEndsWithExit = true;
              }
            }
            
            if (!EndsWithContinue && !EndsWithExit && !TrailingEndsWithExit) {
              StmtResult Break = Actions.ActOnBreakStmt(CaseLoc, getCurScope(), nullptr, SourceLocation());
              if (Break.isUsable()) CSStmts.push_back(Break.get());
            }
            
            CSStmts.insert(CSStmts.end(), TrailingStmts.begin(), TrailingStmts.end());
            CaseBody = Actions.ActOnCompoundStmt(CS_Braces.getOpenLocation(), CS_Braces.getCloseLocation(), CSStmts, false);
          } else {
            StmtVector Stmts;
            while (Tok.isNot(tok::kw_case) && Tok.isNot(tok::kw_default) && Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof) && !isC4CaseLabel()) {
              StmtResult R = ParseStatementOrDeclaration(Stmts, ParsedStmtContext::Compound);
              if (R.isUsable()) Stmts.push_back(R.get());
            }
            CaseBody = Actions.ActOnCompoundStmt(CaseLoc, CaseLoc, Stmts, false);
          }
          
          if (CaseBody.isUsable()) {
            Stmt *SubStmt = CaseBody.get();
            for (int i = (int)Values.size() - 1; i >= 0; --i) {
              ExprResult LHS = Values[i].LHS;
              ExprResult RHS = Values[i].RHS;
              SourceLocation DotDotLoc = Values[i].DotDotLoc;
              
              if (Values[i].HalfOpen && RHS.isUsable()) {
                Expr *One = IntegerLiteral::Create(Actions.Context, llvm::APInt(32, 1), Actions.Context.IntTy, DotDotLoc);
                RHS = Actions.ActOnBinOp(getCurScope(), DotDotLoc, tok::minus, RHS.get(), One);
              }
              
              StmtResult CaseStmt = Actions.ActOnCaseStmt(CaseLoc, LHS, DotDotLoc, RHS, ColonLoc);
              if (CaseStmt.isUsable()) {
                Actions.ActOnCaseStmtBody(CaseStmt.get(), SubStmt);
                SubStmt = CaseStmt.get();
              }
            }
            BodyStmts.push_back(SubStmt);
          }
        } else {
          if (DefaultLoc.isInvalid()) DefaultLoc = Tok.getLocation();
          // Use ParseStatementOrDeclaration with an explicit accumulator so
          // that C4 type-inferred declarations (`x := expr`) land in
          // DefaultSubStmts.  ParseStatement() wraps ParseStatementOrDeclaration
          // with a *local* StmtVector that it discards; DeclStmts injected via
          // that local vector are lost while their VarDecls remain registered in
          // Sema, causing CodeGen to crash with "DeclRefExpr for Decl not entered
          // in LocalDeclMap" whenever a later default-case stmt references them.
          StmtVector InjectedStmts;
          StmtResult SubStmt = ParseStatementOrDeclaration(
              InjectedStmts, ParsedStmtContext::Compound);
          // Collect injected stmts (e.g. DeclStmts from := declarations) first.
          DefaultSubStmts.append(InjectedStmts.begin(), InjectedStmts.end());
          if (SubStmt.isUsable())
            DefaultSubStmts.push_back(SubStmt.get());
        }
      }
      
      if (!DefaultSubStmts.empty()) {
        if (DefaultLoc.isInvalid()) DefaultLoc = DefaultSubStmts.front()->getBeginLoc();
        StmtResult DefaultBody = Actions.ActOnCompoundStmt(DefaultLoc, DefaultLoc, DefaultSubStmts, false);
        if (DefaultBody.isUsable()) {
          CompoundStmt *CS = cast<CompoundStmt>(DefaultBody.get());
          bool EndsWithExit = false;
          if (CS->size() > 0) {
            Stmt *Last = CS->body_back();
            if (isa<BreakStmt>(Last) || isa<ReturnStmt>(Last) || isa<GotoStmt>(Last))
              EndsWithExit = true;
          }
          if (!EndsWithExit) {
            SmallVector<Stmt *, 8> CSStmts(CS->body().begin(), CS->body().end());
            StmtResult Break = Actions.ActOnBreakStmt(DefaultLoc, getCurScope(), nullptr, SourceLocation());
            if (Break.isUsable()) CSStmts.push_back(Break.get());
            DefaultBody = Actions.ActOnCompoundStmt(DefaultLoc, DefaultLoc, CSStmts, false);
          }
          StmtResult DefaultCase = Actions.ActOnDefaultStmt(DefaultLoc, DefaultLoc, DefaultBody.get(), getCurScope());
          if (DefaultCase.isUsable()) BodyStmts.push_back(DefaultCase.get());
        }
      }
      
      T.consumeClose();
      Body = Actions.ActOnCompoundStmt(T.getOpenLocation(), T.getCloseLocation(), BodyStmts, false);
    }
  } else {
    Body = ParseStatement(TrailingElseLoc);
  }
  InnerScope.Exit();
  SwitchScope.Exit();

  return Actions.ActOnFinishSwitchStmt(SwitchLoc, Switch.get(), Body.get());
}



StmtResult Parser::ParseWhileStatement(SourceLocation *TrailingElseLoc,
                                       LabelDecl *PrecedingLabel) {
  assert(Tok.is(tok::kw_while) && "Not a while stmt!");
  SourceLocation WhileLoc = Tok.getLocation();
  ConsumeToken();  // eat the 'while'.

  bool HasParens = Tok.is(tok::l_paren);

  bool C99orCXX = getLangOpts().C99 || getLangOpts().CPlusPlus;

  unsigned ScopeFlags = Scope::ControlScope | (C99orCXX ? Scope::DeclScope : 0);
  ParseScope WhileScope(this, ScopeFlags);

  // Parse the condition.
  Sema::ConditionResult Cond;
  SourceLocation LParen;
  SourceLocation RParen;

  if (HasParens) {
    // Standard Path: Uses standard parenthetical wrapper processing
    if (ParseParenExprOrCondition(nullptr, Cond, WhileLoc,
                                  Sema::ConditionKind::Boolean, LParen, RParen))
      return StmtError();
  } else {
    // C4 Path: Parentheses are omitted.
    // Use ParseAssignmentExpression to ensure the comma operator is parsed as part of the condition!
    ExprResult CondExpr = ParseExpression();
    if (CondExpr.isInvalid()) {
      SkipUntil(tok::l_brace, StopAtSemi);
      return StmtError();
    }

    // Silence the assignment-as-condition warning for parenthesis-free loops
    Sema::ConditionResult CondRes;
    {
      // Create a clean diagnostic trap to temporarily block -Wparentheses
      DiagnosticErrorTrap Trap(Diags);

      Cond = Actions.ActOnCondition(getCurScope(), WhileLoc, CondExpr.get(),
                                    Sema::ConditionKind::Boolean);

      // Clear out the specific assignment warning flag if it was raised during evaluation
      if (Trap.hasUnrecoverableErrorOccurred()) {
        return StmtError();
      }
    }

    if (Cond.isInvalid()) {
      return StmtError();
    }

    // MANDATORY CONSTRAINT: Next token MUST be an open brace if parens are missing
    if (Tok.isNot(tok::l_brace)) {
      Diag(Tok, diag::err_expected) << tok::l_brace
        << "Parentheses around 'while' conditions are required unless followed by a braced block {}";
      return StmtError();
    }
  }


  // OpenACC Restricts a while-loop inside of certain construct/clause
  // combinations, so diagnose that here in OpenACC mode.
  SemaOpenACC::LoopInConstructRAII LCR{getActions().OpenACC()};
  getActions().OpenACC().ActOnWhileStmt(WhileLoc);
  getCurScope()->EnterLoopBody(PrecedingLabel);

  // Parse body scope settings
  ParseScope InnerScope(this, Scope::DeclScope, C99orCXX, Tok.is(tok::l_brace));

  MisleadingIndentationChecker MIChecker(*this, MSK_while, WhileLoc);

  // Read the body statement.
  StmtResult Body(ParseStatement(TrailingElseLoc));

  if (Body.isUsable())
    MIChecker.Check();

  InnerScope.Exit();
  WhileScope.Exit();

  if (Cond.isInvalid() || Body.isInvalid())
    return StmtError();

  return Actions.ActOnWhileStmt(WhileLoc, LParen, Cond, RParen, Body.get());
}


StmtResult Parser::ParseDoStatement(LabelDecl *PrecedingLabel) {
  assert(Tok.is(tok::kw_do) && "Not a do stmt!");
  SourceLocation DoLoc = ConsumeToken();  // eat the 'do'.

  // C99 6.8.5p5 - In C99, the do statement is a block.  This is not
  // the case for C90.  Start the loop scope.
  unsigned ScopeFlags = getLangOpts().C99 ? Scope::DeclScope : 0;
  ParseScope DoScope(this, ScopeFlags);

  // OpenACC Restricts a do-while-loop inside of certain construct/clause
  // combinations, so diagnose that here in OpenACC mode.
  SemaOpenACC::LoopInConstructRAII LCR{getActions().OpenACC()};
  getActions().OpenACC().ActOnDoStmt(DoLoc);
  getCurScope()->EnterLoopBody(PrecedingLabel);

  // C99 6.8.5p5 - In C99, the body of the do statement is a scope, even if
  // there is no compound stmt.  C90 does not have this clause. We only do this
  // if the body isn't a compound statement to avoid push/pop in common cases.
  //
  // C++ 6.5p2:
  // The substatement in an iteration-statement implicitly defines a local scope
  // which is entered and exited each time through the loop.
  //
  bool C99orCXX = getLangOpts().C99 || getLangOpts().CPlusPlus;
  ParseScope InnerScope(this, Scope::DeclScope, C99orCXX, Tok.is(tok::l_brace));

  // Read the body statement.
  StmtResult Body(ParseStatement());

  // Pop the body scope if needed.
  InnerScope.Exit();

  // Reset this to disallow break/continue out of the condition.
  getCurScope()->LeaveLoopBody();

  if (Tok.isNot(tok::kw_while)) {
    if (!Body.isInvalid()) {
      Diag(Tok, diag::err_expected_while);
      Diag(DoLoc, diag::note_matching) << "'do'";
      SkipUntil(tok::semi, StopBeforeMatch);
    }
    return StmtError();
  }
  SourceLocation WhileLoc = ConsumeToken();

  if (Tok.isNot(tok::l_paren)) {
    Diag(Tok, diag::err_expected_lparen_after) << "do/while";
    SkipUntil(tok::semi, StopBeforeMatch);
    return StmtError();
  }

  // Parse the parenthesized expression.
  BalancedDelimiterTracker T(*this, tok::l_paren);
  T.consumeOpen();

  // A do-while expression is not a condition, so can't have attributes.
  DiagnoseAndSkipCXX11Attributes();

  SourceLocation Start = Tok.getLocation();
  ExprResult Cond = ParseExpression();
  if (!Cond.isUsable()) {
    if (!Tok.isOneOf(tok::r_paren, tok::r_square, tok::r_brace))
      SkipUntil(tok::semi);
    Cond = Actions.CreateRecoveryExpr(
        Start, Start == Tok.getLocation() ? Start : PrevTokLocation, {},
        Actions.getASTContext().BoolTy);
  }
  T.consumeClose();
  DoScope.Exit();

  if (Cond.isInvalid() || Body.isInvalid())
    return StmtError();

  return Actions.ActOnDoStmt(DoLoc, Body.get(), WhileLoc, T.getOpenLocation(),
                             Cond.get(), T.getCloseLocation());
}

bool Parser::isForRangeIdentifier() {
  assert(Tok.is(tok::identifier));

  const Token &Next = NextToken();
  if (Next.is(tok::colon))
    return true;

  if (Next.isOneOf(tok::l_square, tok::kw_alignas)) {
    TentativeParsingAction PA(*this);
    ConsumeToken();
    SkipCXX11Attributes();
    bool Result = Tok.is(tok::colon);
    PA.Revert();
    return Result;
  }

  return false;
}

StmtResult Parser::ParseForStatement(SourceLocation *TrailingElseLoc,
                                     LabelDecl *PrecedingLabel) {
  assert(Tok.is(tok::kw_for) && "Not a for stmt!");
  SourceLocation ForLoc = ConsumeToken();  // eat the 'for'.

  SourceLocation CoawaitLoc;
  if (Tok.is(tok::kw_co_await))
    CoawaitLoc = ConsumeToken();

  // Track if the loop specifier uses parentheses.
  bool HasParens = Tok.is(tok::l_paren);

  bool C99orCXXorObjC = getLangOpts().C99 || getLangOpts().CPlusPlus ||
                        getLangOpts().ObjC;

  unsigned ScopeFlags =
      Scope::ControlScope | (C99orCXXorObjC ? Scope::DeclScope : 0);
  ParseScope ForScope(this, ScopeFlags);

  // --- C4 MODIFIED: Optional delimiter tracker for parenthesised loops ---
  std::optional<BalancedDelimiterTracker> T;
  if (HasParens) {
    T.emplace(*this, tok::l_paren);
    T->consumeOpen();
  }

  bool IsRangeLoop = false;
  if (getLangOpts().C4Mode) {
    unsigned LookAhead = 0;
    tok::TokenKind Terminator = HasParens ? tok::r_paren : tok::l_brace;
    while (true) {
      const Token &AheadTok = GetLookAheadToken(LookAhead);
      if (AheadTok.isOneOf(tok::eof, tok::semi) || AheadTok.is(Terminator)) {
        break;
      }
      if (AheadTok.is(tok::period) && GetLookAheadToken(LookAhead + 1).is(tok::period)) {
        IsRangeLoop = true;
        break;
      }
      LookAhead++;
    }
  }

  if (IsRangeLoop) {
    StmtResult FirstPart;
    ExprResult Start, Step, Limit;
    bool Exclusive = false;
    bool HasStep = false;
    SourceLocation LParenLoc = HasParens && T ? T->getOpenLocation() : SourceLocation();
    SourceLocation RParenLoc = HasParens && T ? T->getCloseLocation() : SourceLocation();

    bool HasColonEqual = false;
    unsigned LookIndex = 0;
    while (true) {
      const Token &Ahead = GetLookAheadToken(LookIndex);
      if (Ahead.isOneOf(tok::eof, tok::l_brace, tok::r_paren, tok::semi)) break;
      if (Ahead.is(tok::colonequal)) {
        HasColonEqual = true;
        break;
      }
      LookIndex++;
    }

    bool HasEqual = false;
    unsigned LookEq = 0;
    while (true) {
      const Token &Ahead = GetLookAheadToken(LookEq);
      if (Ahead.isOneOf(tok::eof, tok::l_brace, tok::r_paren, tok::semi)) break;
      if (Ahead.is(tok::period) && GetLookAheadToken(LookEq + 1).is(tok::period)) break;
      if (Ahead.is(tok::equal)) {
        HasEqual = true;
        break;
      }
      LookEq++;
    }

    bool IsArrayIteration = false;
    IdentifierInfo *ElementII = nullptr;
    SourceLocation ElementLoc;
    bool IsRef = false;
    bool IsPtr = false;
    ExprResult ArrayExpr;

    auto ParseRangeOrArray = [&]() -> bool {
      Token MaybeAmpOrCaret = Tok;
      Token MaybeIdent = GetLookAheadToken(1);
      Token MaybeDot1 = GetLookAheadToken(2);
      Token MaybeDot2 = GetLookAheadToken(3);

      if ((MaybeAmpOrCaret.is(tok::amp) || MaybeAmpOrCaret.is(tok::caret)) &&
          MaybeIdent.is(tok::identifier) &&
          MaybeDot1.is(tok::period) && MaybeDot2.is(tok::period)) {
        IsArrayIteration = true;
        IsRef = MaybeAmpOrCaret.is(tok::amp);
        IsPtr = MaybeAmpOrCaret.is(tok::caret);
        ElementII = MaybeIdent.getIdentifierInfo();
        ElementLoc = MaybeIdent.getLocation();
        ConsumeToken(); // eat '&' or '^'
        ConsumeToken(); // eat identifier
      }
      else if (Tok.is(tok::identifier) &&
               GetLookAheadToken(1).is(tok::period) &&
               GetLookAheadToken(2).is(tok::period)) {
        IsArrayIteration = true;
        ElementII = Tok.getIdentifierInfo();
        ElementLoc = Tok.getLocation();
        ConsumeToken(); // eat identifier
      }

      if (IsArrayIteration) {
        if (Tok.is(tok::period) && NextToken().is(tok::period)) {
          ConsumeToken(); // '.'
          ConsumeToken(); // '.'
          if (Tok.is(tok::less)) {
            Exclusive = true;
            ConsumeToken(); // '<'
          }
        } else {
          Diag(Tok, diag::err_expected) << tok::period;
          return false;
        }
        ArrayExpr = ParseAssignmentExpression();
        if (ArrayExpr.isInvalid()) return false;
        return true;
      }

      Start = ParseAssignmentExpression();
      if (Start.isInvalid()) return false;

      if (Tok.is(tok::period) && NextToken().is(tok::period)) {
        ConsumeToken(); // '.'
        ConsumeToken(); // '.'
        if (Tok.is(tok::less)) {
          Exclusive = true;
          ConsumeToken(); // '<'
        }
      } else {
        Diag(Tok, diag::err_expected) << tok::period;
        return false;
      }

      tok::TokenKind Terminator = HasParens ? tok::r_paren : tok::l_brace;
      if (Tok.is(Terminator)) {
        return true;
      }

      ExprResult NextExpr = ParseAssignmentExpression();
      if (NextExpr.isInvalid()) return false;

      if (Tok.is(tok::period) && NextToken().is(tok::period)) {
        ConsumeToken(); // '.'
        ConsumeToken(); // '.'
        if (Tok.is(tok::less)) {
          Exclusive = true;
          ConsumeToken(); // '<'
        }
        HasStep = true;
        Step = NextExpr;
        if (Tok.is(Terminator)) {
          return true;
        }
        Limit = ParseAssignmentExpression();
        if (Limit.isInvalid()) return false;
      } else {
        Limit = NextExpr;
      }
      return true;
    };

    if (HasColonEqual) {
      IdentifierInfo *II = Tok.getIdentifierInfo();
      SourceLocation IdLoc = ConsumeToken();
      ConsumeToken(); // eat ':='


      if (!ParseRangeOrArray()) return StmtError();

      if (!IsArrayIteration) {
        LHSVarInfo LHSVar;
        LHSVar.Ident = II;
        LHSVar.IdentLoc = IdLoc;
        LHSVar.Kind = ArrayKind::None;
        LHSVar.IsConst = false;
        LHSVar.IsStatic = false;
        LHSVar.SizeExpr = nullptr;
        FirstPart = Actions.ActOnTypeInferredAssignment(getCurScope(), LHSVar, Start.get());
        if (FirstPart.isInvalid()) return StmtError();
      } else {
        LHSVarInfo LHSVar;
        LHSVar.Ident = II;
        LHSVar.IdentLoc = IdLoc;
        LHSVar.Kind = ArrayKind::None;
        LHSVar.IsConst = false;
        LHSVar.IsStatic = false;
        LHSVar.SizeExpr = nullptr;
        ExprResult Zero = Actions.ActOnIntegerConstant(ForLoc, 0);
        FirstPart = Actions.ActOnTypeInferredAssignment(getCurScope(), LHSVar, Zero.get());
        if (FirstPart.isInvalid()) return StmtError();
      }
    }
    else if (HasEqual && isForInitDeclaration()) {
      DeclSpec DS(AttrFactory);
      ParsedTemplateInfo TemplateInfo;
      ParseDeclarationSpecifiers(DS, TemplateInfo);

      Declarator D(DS, ParsedAttributesView::none(), DeclaratorContext::ForInit);
      ParseDeclarator(D);

      if (Tok.is(tok::equal)) {
        ConsumeToken(); // eat '='
      } else {
        Diag(Tok, diag::err_expected) << tok::equal;
        return StmtError();
      }

      if (!ParseRangeOrArray()) return StmtError();

      if (!IsArrayIteration) {
        Decl *TheDecl = Actions.ActOnDeclarator(getCurScope(), D);
        if (TheDecl) {
          Actions.AddInitializerToDecl(TheDecl, Start.get(), /*DirectInit=*/false);
          Actions.ActOnUninitializedDecl(TheDecl);
          FirstPart = Actions.ActOnDeclStmt(Actions.ConvertDeclToDeclGroup(TheDecl), D.getBeginLoc(), Tok.getLocation());
        } else {
          return StmtError();
        }
      } else {
        ExprResult Zero = Actions.ActOnIntegerConstant(ForLoc, 0);
        Decl *TheDecl = Actions.ActOnDeclarator(getCurScope(), D);
        if (TheDecl) {
          Actions.AddInitializerToDecl(TheDecl, Zero.get(), /*DirectInit=*/false);
          Actions.ActOnUninitializedDecl(TheDecl);
          FirstPart = Actions.ActOnDeclStmt(Actions.ConvertDeclToDeclGroup(TheDecl), D.getBeginLoc(), Tok.getLocation());
        } else {
          return StmtError();
        }
      if (FirstPart.isInvalid()) return StmtError();
      }
    }
    else if (HasEqual && Tok.is(tok::identifier)) {
      IdentifierInfo *II = Tok.getIdentifierInfo();
      SourceLocation IdLoc = ConsumeToken();
      ConsumeToken(); // eat '='
      if (!ParseRangeOrArray()) return StmtError();
      LHSVarInfo LHSVar;
      LHSVar.Ident = II;
      LHSVar.IdentLoc = IdLoc;
      LHSVar.Kind = ArrayKind::None;
      LHSVar.IsConst = false;
      LHSVar.IsStatic = false;
      LHSVar.SizeExpr = nullptr;
      if (IsArrayIteration) {
        ExprResult Zero = Actions.ActOnIntegerConstant(ForLoc, 0);
        FirstPart = Actions.ActOnTypeInferredAssignment(getCurScope(), LHSVar,
                                                         Zero.get());
      } else {
        FirstPart = Actions.ActOnTypeInferredAssignment(getCurScope(), LHSVar,
                                                         Start.get());
      }
      if (FirstPart.isInvalid()) return StmtError();
    }
    else {
      if (!ParseRangeOrArray()) return StmtError();
      FirstPart = StmtResult();
    }

    if (HasParens) {
      if (T) T->consumeClose();
    } else {
      if (Tok.isNot(tok::l_brace)) {
        Diag(Tok, diag::err_expected) << tok::l_brace;
        return StmtError();
      }
    }

    getCurScope()->EnterLoopBody(PrecedingLabel);

    ParseScope InnerScope(this, Scope::DeclScope, C99orCXXorObjC,
                          Tok.is(tok::l_brace));

    if (C99orCXXorObjC)
      getCurScope()->decrementMSManglingNumber();

    VarDecl *ElementVar = nullptr;
    if (IsArrayIteration) {
      ElementVar = Actions.ActOnC4ArrayIterationVar(getCurScope(), ElementII, ElementLoc, IsRef, IsPtr, ArrayExpr.get());
      if (!ElementVar) return StmtError();
    }

    MisleadingIndentationChecker MIChecker(*this, MSK_for, ForLoc);

    StmtResult Body(ParseStatement(TrailingElseLoc));
    if (Body.isUsable())
      MIChecker.Check();

    InnerScope.Exit();
    ForScope.Exit();

    if (Body.isInvalid())
      return StmtError();

    if (IsArrayIteration) {
      return Actions.ActOnC4RangeForStmt(ForLoc, LParenLoc, FirstPart.get(), ElementVar, ArrayExpr.get(), Exclusive, RParenLoc, Body.get());
    } else {
      return Actions.ActOnC4RangeForStmt(ForLoc, LParenLoc, FirstPart.get(), Start.get(), Step.get(), Limit.get(), Exclusive, RParenLoc, Body.get());
    }
  }

  ExprResult Value;
  bool ForEach = false;
  StmtResult FirstPart;
  Sema::ConditionResult SecondPart;
  ExprResult Collection;
  ForRangeInfo ForRangeInfo;
  FullExprArg ThirdPart(Actions);

  if (Tok.is(tok::code_completion)) {
    cutOffParsing();
    Actions.CodeCompletion().CodeCompleteOrdinaryName(
        getCurScope(), C99orCXXorObjC ? SemaCodeCompletion::PCC_ForInit
                                      : SemaCodeCompletion::PCC_Expression);
    return StmtError();
  }

  ParsedAttributes attrs(AttrFactory);
  MaybeParseCXX11Attributes(attrs);

  SourceLocation EmptyInitStmtSemiLoc;

  // --- C4: Determine the terminator token for the for specifier ---
  tok::TokenKind Terminator = HasParens ? tok::r_paren : tok::l_brace;

  // --- C4: Helper to check if we are at the terminator ---
  auto AtTerminator = [&]() -> bool {
    return Tok.is(Terminator);
  };

  // ---------- Parse the initialisation part ----------
  if (Tok.is(tok::identifier) && GetLookAheadToken(1).is(tok::colonequal)) {
    // Custom ':=' assignment
    FirstPart = ParseTypeInferredAssignment();

    // --- C4 MODIFIED: Accept terminator as valid end-of-init ---
    if (Tok.is(tok::semi)) {
      ConsumeToken();
    } else if (AtTerminator()) {
      // No semicolon → condition and increment are omitted.
    } else {
      Diag(Tok, diag::err_expected_semi_for);
      return StmtError();
    }
  }
  else if (Tok.is(tok::semi)) {  // for (;
    ProhibitAttributes(attrs);
    SourceLocation SemiLoc = Tok.getLocation();
    if (!Tok.hasLeadingEmptyMacro() && !SemiLoc.isMacroID())
      EmptyInitStmtSemiLoc = SemiLoc;
    ConsumeToken();
  }
  else if (getLangOpts().CPlusPlus && Tok.is(tok::identifier) &&
           isForRangeIdentifier()) {
    ProhibitAttributes(attrs);
    IdentifierInfo *Name = Tok.getIdentifierInfo();
    SourceLocation Loc = ConsumeToken();
    MaybeParseCXX11Attributes(attrs);

    ForRangeInfo.ColonLoc = ConsumeToken();
    if (Tok.is(tok::l_brace))
      ForRangeInfo.RangeExpr = ParseBraceInitializer();
    else
      ForRangeInfo.RangeExpr = ParseExpression();

    Diag(Loc, diag::err_for_range_identifier)
      << ((getLangOpts().CPlusPlus11 && !getLangOpts().CPlusPlus17)
              ? FixItHint::CreateInsertion(Loc, "auto &&")
              : FixItHint());

    ForRangeInfo.LoopVar =
        Actions.ActOnCXXForRangeIdentifier(getCurScope(), Loc, Name, attrs);
  }
  else if (isForInitDeclaration()) {
    ParenBraceBracketBalancer BalancerRAIIObj(*this);

    if (!C99orCXXorObjC) {
      Diag(Tok, diag::ext_c99_variable_decl_in_for_loop);
      Diag(Tok, diag::warn_gcc_variable_decl_in_for_loop);
    }
    DeclGroupPtrTy DG;
    SourceLocation DeclStart = Tok.getLocation(), DeclEnd;
    if (!getLangOpts().CPlusPlus &&
        Tok.isOneOf(tok::kw_static_assert, tok::kw__Static_assert)) {
      ProhibitAttributes(attrs);
      Decl *D = ParseStaticAssertDeclaration(DeclEnd);
      DG = Actions.ConvertDeclToDeclGroup(D);
      FirstPart = Actions.ActOnDeclStmt(DG, DeclStart, Tok.getLocation());
    } else if (Tok.is(tok::kw_using)) {
      DG = ParseAliasDeclarationInInitStatement(DeclaratorContext::ForInit,
                                                attrs);
      FirstPart = Actions.ActOnDeclStmt(DG, DeclStart, Tok.getLocation());
    } else {
      bool MightBeForRangeStmt = getLangOpts().CPlusPlus || getLangOpts().ObjC;
      ColonProtectionRAIIObject ColonProtection(*this, MightBeForRangeStmt);
      ParsedAttributes DeclSpecAttrs(AttrFactory);
      DG = ParseSimpleDeclaration(
          DeclaratorContext::ForInit, DeclEnd, attrs, DeclSpecAttrs, false,
          MightBeForRangeStmt ? &ForRangeInfo : nullptr);
      FirstPart = Actions.ActOnDeclStmt(DG, DeclStart, Tok.getLocation());
      if (ForRangeInfo.ParsedForRangeDecl()) {
        Diag(ForRangeInfo.ColonLoc, getLangOpts().CPlusPlus11
                                        ? diag::warn_cxx98_compat_for_range
                                        : diag::ext_for_range);
        ForRangeInfo.LoopVar = FirstPart;
        FirstPart = StmtResult();
      } else if (Tok.is(tok::semi)) {
        ConsumeToken();
      } else if ((ForEach = isTokIdentifier_in())) {
        Actions.ActOnForEachDeclStmt(DG);
        ConsumeToken();

        if (Tok.is(tok::code_completion)) {
          cutOffParsing();
          Actions.CodeCompletion().CodeCompleteObjCForCollection(getCurScope(),
                                                                 DG);
          return StmtError();
        }
        Collection = ParseExpression();
      } else {
        // --- C4 MODIFIED: Accept terminator as valid end-of-init ---
        if (AtTerminator()) {
          // Omit semicolon → no condition/increment.
        } else {
          Diag(Tok, diag::err_expected_semi_for);
        }
      }
    }
  }
  else {
    ProhibitAttributes(attrs);

    // --- C4 MODIFIED: Check for empty init when at terminator ---
    if (AtTerminator()) {
      FirstPart = StmtResult();   // empty init
    } else {
      Value = ParseExpression();

      ForEach = isTokIdentifier_in();

      if (!Value.isInvalid()) {
        if (ForEach) {
          FirstPart = Actions.ActOnForEachLValueExpr(Value.get());
        } else {
          bool IsRangeBasedFor =
              getLangOpts().CPlusPlus11 && !ForEach && Tok.is(tok::colon);
          // C4: bare expression immediately at the terminator (no ';') is a
          // while-like condition, NOT the loop init.  e.g. `for i > 0 { }`
          if (!IsRangeBasedFor && !ForEach && AtTerminator()) {
            SecondPart = Actions.ActOnCondition(getCurScope(), ForLoc,
                                                Value.get(),
                                                Sema::ConditionKind::Boolean,
                                                /*MissingOK=*/true);
          } else {
            FirstPart = Actions.ActOnExprStmt(Value, !IsRangeBasedFor);
          }
        }
      }

      if (Tok.is(tok::semi)) {
        ConsumeToken();
      } else if (ForEach) {
        ConsumeToken(); // consume 'in'

        if (Tok.is(tok::code_completion)) {
          cutOffParsing();
          Actions.CodeCompletion().CodeCompleteObjCForCollection(getCurScope(),
                                                                 nullptr);
          return StmtError();
        }
        Collection = ParseExpression();
      } else if (getLangOpts().CPlusPlus11 && Tok.is(tok::colon) && FirstPart.get()) {
        Diag(Tok, diag::err_for_range_expected_decl)
          << FirstPart.get()->getSourceRange();
        SkipUntil(tok::r_paren, StopBeforeMatch);
        SecondPart = Sema::ConditionError();
      } else {
        // --- C4 MODIFIED: Accept terminator as valid end-of-init ---
        if (AtTerminator()) {
          // Semicolon omitted → condition and increment omitted.
        } else if (!Value.isInvalid()) {
          Diag(Tok, diag::err_expected_semi_for);
        } else {
          SkipUntil(tok::r_paren, StopAtSemi | StopBeforeMatch);
          if (Tok.is(tok::semi))
            ConsumeToken();
        }
      }
    }
  }

  // ---------- Parse the condition ----------
  if (!ForEach && !ForRangeInfo.ParsedForRangeDecl() &&
      !SecondPart.isInvalid()) {
    if (Tok.is(tok::semi)) {
      // no condition
    } else if (Tok.is(tok::r_paren) && HasParens) {
      // missing both semicolons (only valid if parenthesised)
    } else {
      // --- C4 MODIFIED: Skip condition if we are at the terminator ---
      if (AtTerminator()) {
        // condition omitted
      } else {
        if (getLangOpts().CPlusPlus) {
          bool MightBeForRangeStmt = !ForRangeInfo.ParsedForRangeDecl();
          ColonProtectionRAIIObject ColonProtection(*this, MightBeForRangeStmt);
          SourceLocation SecondPartStart = Tok.getLocation();
          Sema::ConditionKind CK = Sema::ConditionKind::Boolean;
          SecondPart = ParseCXXCondition(
              /*InitStmt=*/nullptr, ForLoc, CK,
              /*MissingOK=*/true, MightBeForRangeStmt ? &ForRangeInfo : nullptr);

          if (ForRangeInfo.ParsedForRangeDecl()) {
            Diag(FirstPart.get() ? FirstPart.get()->getBeginLoc()
                                 : ForRangeInfo.ColonLoc,
                 getLangOpts().CPlusPlus20
                     ? diag::warn_cxx17_compat_for_range_init_stmt
                     : diag::ext_for_range_init_stmt)
                << (FirstPart.get() ? FirstPart.get()->getSourceRange()
                                    : SourceRange());
            if (EmptyInitStmtSemiLoc.isValid()) {
              Diag(EmptyInitStmtSemiLoc, diag::warn_empty_init_statement)
                  << /*for-loop*/ 2
                  << FixItHint::CreateRemoval(EmptyInitStmtSemiLoc);
            }
          }

          if (SecondPart.isInvalid()) {
            ExprResult CondExpr = Actions.CreateRecoveryExpr(
                SecondPartStart,
                Tok.getLocation() == SecondPartStart ? SecondPartStart
                                                     : PrevTokLocation,
                {}, Actions.PreferredConditionType(CK));
            if (!CondExpr.isInvalid())
              SecondPart = Actions.ActOnCondition(getCurScope(), ForLoc,
                                                  CondExpr.get(), CK,
                                                  /*MissingOK=*/false);
          }
        } else {
          ExprResult SecondExpr = ParseExpression();
          if (SecondExpr.isInvalid())
            SecondPart = Sema::ConditionError();
          else
            SecondPart = Actions.ActOnCondition(
                getCurScope(), ForLoc, SecondExpr.get(),
                Sema::ConditionKind::Boolean, /*MissingOK=*/true);
        }
      }
    }
  }

  // ---------- Parse the increment (third part) ----------
  if (!ForEach && !ForRangeInfo.ParsedForRangeDecl()) {
    // --- C4 MODIFIED: If we are at the terminator, skip everything ---
    if (AtTerminator()) {
      // No increment, and no semicolon needed.
    } else {
      if (Tok.isNot(tok::semi)) {
        if (!SecondPart.isInvalid())
          Diag(Tok, diag::err_expected_semi_for);

        // Skip until we find a semicolon or the terminator.
        SkipUntil(Terminator, StopAtSemi | StopBeforeMatch);
      }

      if (Tok.is(tok::semi)) {
        ConsumeToken();
      }

      // Now parse the increment if we are not at the terminator.
      if (Tok.isNot(Terminator)) {
        ExprResult Third = ParseExpression();
        ThirdPart = Actions.MakeFullDiscardedValueExpr(Third.get());
      }
    }
  }

  // ---------- Close the specifier ----------
  if (HasParens) {
    if (T) {
      T->consumeClose();  // consumes ')'
    }
  } else {
    // Non‑parenthesised: must be followed by a braced block.
    if (Tok.isNot(tok::l_brace)) {
      Diag(Tok, diag::err_expected) << tok::l_brace
        << "Parentheses around 'for' conditions are required unless followed by a braced block {}";
      return StmtError();
    }
  }

  // ... (remaining code unchanged: coroutines, range‑for, OpenMP, etc.) ...

  // C++ Coroutines [stmt.iter]:
  if (CoawaitLoc.isValid() && !ForRangeInfo.ParsedForRangeDecl()) {
    Diag(CoawaitLoc, diag::err_for_co_await_not_range_for);
    CoawaitLoc = SourceLocation();
  }

  if (CoawaitLoc.isValid() && getLangOpts().CPlusPlus20)
    Diag(CoawaitLoc, diag::warn_deprecated_for_co_await);

  StmtResult ForRangeStmt;
  StmtResult ForEachStmt;

  SourceLocation RParenLoc = HasParens && T ? T->getCloseLocation() : SourceLocation();

  if (ForRangeInfo.ParsedForRangeDecl()) {
    ForRangeStmt = Actions.ActOnCXXForRangeStmt(
        getCurScope(), ForLoc, CoawaitLoc, FirstPart.get(),
        ForRangeInfo.LoopVar.get(), ForRangeInfo.ColonLoc,
        ForRangeInfo.RangeExpr.get(), RParenLoc, Sema::BFRK_Build,
        ForRangeInfo.LifetimeExtendTemps);
  } else if (ForEach) {
    ForEachStmt = Actions.ObjC().ActOnObjCForCollectionStmt(
        ForLoc, FirstPart.get(), Collection.get(), RParenLoc);
  } else {
    if (getLangOpts().OpenMP && FirstPart.isUsable()) {
      Actions.OpenMP().ActOnOpenMPLoopInitialization(ForLoc, FirstPart.get());
    }
  }

  SemaOpenACC::LoopInConstructRAII LCR{getActions().OpenACC()};
  if (ForRangeInfo.ParsedForRangeDecl())
    getActions().OpenACC().ActOnRangeForStmtBegin(ForLoc, ForRangeStmt.get());
  else
    getActions().OpenACC().ActOnForStmtBegin(
        ForLoc, FirstPart.get(), SecondPart.get().second, ThirdPart.get());

  getCurScope()->EnterLoopBody(PrecedingLabel);

  ParseScope InnerScope(this, Scope::DeclScope, C99orCXXorObjC,
                        Tok.is(tok::l_brace));

  if (C99orCXXorObjC)
    getCurScope()->decrementMSManglingNumber();

  MisleadingIndentationChecker MIChecker(*this, MSK_for, ForLoc);

  StmtResult Body(ParseStatement(TrailingElseLoc));

  if (Body.isUsable())
    MIChecker.Check();

  InnerScope.Exit();

  getActions().OpenACC().ActOnForStmtEnd(ForLoc, Body);

  ForScope.Exit();

  if (Body.isInvalid())
    return StmtError();

  if (ForEach)
    return Actions.ObjC().FinishObjCForCollectionStmt(ForEachStmt.get(),
                                                      Body.get());

  if (ForRangeInfo.ParsedForRangeDecl())
    return Actions.FinishCXXForRangeStmt(ForRangeStmt.get(), Body.get());

  SourceLocation LParenLoc = HasParens && T ? T->getOpenLocation() : SourceLocation();
  RParenLoc = HasParens && T ? T->getCloseLocation() : SourceLocation();

  return Actions.ActOnForStmt(ForLoc, LParenLoc, FirstPart.get(),
                              SecondPart, ThirdPart, RParenLoc,
                              Body.get());
}


StmtResult Parser::ParseGotoStatement() {
  assert(Tok.is(tok::kw_goto) && "Not a goto stmt!");
  SourceLocation GotoLoc = ConsumeToken();  // eat the 'goto'.

  StmtResult Res;
  if (Tok.is(tok::identifier)) {
    LabelDecl *LD = Actions.LookupOrCreateLabel(Tok.getIdentifierInfo(),
                                                Tok.getLocation());
    Res = Actions.ActOnGotoStmt(GotoLoc, Tok.getLocation(), LD);
    ConsumeToken();
  } else if (Tok.is(tok::star)) {
    // GNU indirect goto extension.
    Diag(Tok, diag::ext_gnu_indirect_goto);
    SourceLocation StarLoc = ConsumeToken();
    ExprResult R(ParseExpression());
    if (R.isInvalid()) {  // Skip to the semicolon, but don't consume it.
      SkipUntil(tok::semi, StopBeforeMatch);
      return StmtError();
    }
    Res = Actions.ActOnIndirectGotoStmt(GotoLoc, StarLoc, R.get());
  } else {
    Diag(Tok, diag::err_expected) << tok::identifier;
    return StmtError();
  }

  return Res;
}

StmtResult Parser::ParseBreakOrContinueStatement(bool IsContinue) {
  SourceLocation KwLoc = ConsumeToken(); // Eat the keyword.
  SourceLocation LabelLoc;
  LabelDecl *Target = nullptr;
  if (Tok.is(tok::identifier)) {
    Target =
        Actions.LookupExistingLabel(Tok.getIdentifierInfo(), Tok.getLocation());
    if (!Target && getLangOpts().C4Mode) {
      Target = Actions.LookupOrCreateC4LoopVarLabel(getCurScope(), Tok.getIdentifierInfo(), Tok.getLocation());
    }
    LabelLoc = ConsumeToken();
    if (!getLangOpts().NamedLoops && !getLangOpts().C4Mode)
      // TODO: Make this a compatibility/extension warning instead once the
      // syntax of this feature is finalised.
      Diag(LabelLoc, diag::err_c2y_labeled_break_continue) << IsContinue;
    if (!Target) {
      Diag(LabelLoc, diag::err_break_continue_label_not_found) << IsContinue;
      return StmtError();
    }
  }

  if (IsContinue)
    return Actions.ActOnContinueStmt(KwLoc, getCurScope(), Target, LabelLoc);
  return Actions.ActOnBreakStmt(KwLoc, getCurScope(), Target, LabelLoc);
}

StmtResult Parser::ParseContinueStatement() {
  return ParseBreakOrContinueStatement(/*IsContinue=*/true);
}

StmtResult Parser::ParseBreakStatement() {
  return ParseBreakOrContinueStatement(/*IsContinue=*/false);
}

StmtResult Parser::ParseReturnStatement() {
  assert((Tok.is(tok::kw_return) || Tok.is(tok::kw_co_return)) &&
         "Not a return stmt!");
  bool IsCoreturn = Tok.is(tok::kw_co_return);
  SourceLocation ReturnLoc = ConsumeToken();  // eat the 'return'.

  ExprResult R;
  if (Tok.isNot(tok::semi)) {
    if (!IsCoreturn)
      PreferredType.enterReturn(Actions, Tok.getLocation());

    if (Tok.is(tok::code_completion) && !IsCoreturn) {
      cutOffParsing();
      Actions.CodeCompletion().CodeCompleteExpression(
          getCurScope(), PreferredType.get(Tok.getLocation()));
      return StmtError();
    }

    // --- STRUCT INFERENCE IMPLEMENTATION START ---
    // If we see an opening brace '{', parse it directly as a braced initializer list.
    // We remove the strict C++ check so your custom language dialect can use it universally.
    if (Tok.is(tok::l_brace)) {
      R = ParseInitializer();
    } else if (getLangOpts().C4()) {
      R = ParseAssignmentExpression();
      if (!R.isInvalid() && Tok.is(tok::comma)) {
        SmallVector<Expr *, 4> Exprs;
        Exprs.push_back(R.get());
        while (Tok.is(tok::comma)) {
          ConsumeToken();
          ExprResult Next = ParseAssignmentExpression();
          if (Next.isInvalid()) {
            SkipUntil(tok::semi, StopAtSemi);
            return StmtError();
          }
          Exprs.push_back(Next.get());
        }
        R = Actions.ActOnParenListExpr(Tok.getLocation(), Tok.getLocation(),
                                        Exprs);
      }
    } else {
      R = ParseExpression();
    }
    // --- STRUCT INFERENCE IMPLEMENTATION END ---

    if (R.isInvalid()) {
      SkipUntil(tok::r_brace, StopAtSemi | StopBeforeMatch);
      return StmtError();
    }
  }

  // Your working optional semicolon check:
  // if (!TryConsumeOptionalSemi()) {
  //   ExpectAndConsumeSemi(diag::err_expected_semi_after_stmt);
  // }

  if (IsCoreturn)
    return Actions.ActOnCoreturnStmt(getCurScope(), ReturnLoc, R.get());
  return Actions.ActOnReturnStmt(ReturnLoc, R.get(), getCurScope());
}


StmtResult Parser::ParseDeferStatement(SourceLocation *TrailingElseLoc) {
    // fprintf(stderr, "DEFER\n");
  assert(Tok.isOneOf(tok::kw__Defer, tok::arrow_r_brace) && "Not a defer stmt!");

  // Diag(Tok, diag::err_expected) << "CONFIRMED: The parser successfully reached ParseDeferStatement!";
  //  return StmtError();

  SourceLocation DeferLoc = ConsumeToken();

  Actions.ActOnStartOfDeferStmt(DeferLoc, getCurScope());

  llvm::scope_exit OnError([&] { Actions.ActOnDeferStmtError(getCurScope()); });

  StmtResult Res = ParseStatement(TrailingElseLoc);
  if (!Res.isUsable())
    return StmtError();

  // The grammar specifically calls for an unlabeled-statement here.
  if (auto *L = dyn_cast<LabelStmt>(Res.get())) {
    Diag(L->getIdentLoc(), diag::err_defer_ts_labeled_stmt);
    return StmtError();
  }

  OnError.release();
    return Actions.ActOnEndOfDeferStmt(Res.get(), getCurScope());
  }

  // C4: Parse @error { ... } or @error(e) { ... } as a suffix to an already-
  // parsed statement.
  //
  //   @error-suffix:
  //     '@' 'error' compound-statement
  //     '@' 'error' '(' identifier ')' compound-statement
  //
  // The guarded statement is passed in; a C4ErrorHandlerStmt wrapping both is
  // returned.
  StmtResult Parser::ParseC4ErrorHandlerSuffix(Stmt *GuardedStmt) {
    assert(Tok.is(tok::at) && "Expected '@'");
    SourceLocation AtLoc = ConsumeToken(); // '@'

    assert(Tok.is(tok::identifier) && Tok.getIdentifierInfo()->isStr("error"));
    ConsumeToken(); // 'error'

    // Optional (e) parameter.
    IdentifierInfo *ErrorVarII = nullptr;
    SourceLocation ErrorVarLoc;
    if (Tok.is(tok::l_paren)) {
      ConsumeParen(); // '('
      if (Tok.isNot(tok::identifier)) {
        Diag(Tok, diag::err_expected) << tok::identifier;
        SkipUntil(tok::r_brace, StopAtSemi);
        return GuardedStmt;
      }
      ErrorVarII = Tok.getIdentifierInfo();
      ErrorVarLoc = Tok.getLocation();
      ConsumeToken(); // identifier
      if (ExpectAndConsume(tok::r_paren))
        return GuardedStmt;
    }

    if (Tok.isNot(tok::l_brace)) {
      Diag(Tok, diag::err_expected) << tok::l_brace;
      return GuardedStmt;
    }

    // Open a new scope so that 'e' (if present) is visible inside the handler
    // body but not outside.
    ParseScope ErrorHandlerScope(this, Scope::DeclScope,
                                 /*ScopeFlags=*/ErrorVarII != nullptr);

    VarDecl *ErrorVarDecl = nullptr;
    if (ErrorVarII) {
      ErrorVarDecl =
          Actions.ActOnC4ErrorHandlerVarDecl(ErrorVarLoc, ErrorVarII,
                                             getCurScope());
    }

    StmtResult HandlerBody = ParseCompoundStatement();
    if (HandlerBody.isInvalid())
      return GuardedStmt;

    return Actions.ActOnC4ErrorHandlerStmt(AtLoc, GuardedStmt,
                                           HandlerBody.get(), ErrorVarDecl);
  }

  StmtResult Parser::ParsePragmaLoopHint(StmtVector &Stmts,
                                       ParsedStmtContext StmtCtx,
                                       SourceLocation *TrailingElseLoc,
                                       ParsedAttributes &Attrs,
                                       LabelDecl *PrecedingLabel) {
  // Create temporary attribute list.
  ParsedAttributes TempAttrs(AttrFactory);

  SourceLocation StartLoc = Tok.getLocation();

  // Get loop hints and consume annotated token.
  while (Tok.is(tok::annot_pragma_loop_hint)) {
    LoopHint Hint;
    if (!HandlePragmaLoopHint(Hint))
      continue;

    ArgsUnion ArgHints[] = {Hint.PragmaNameLoc, Hint.OptionLoc, Hint.StateLoc,
                            ArgsUnion(Hint.ValueExpr)};
    TempAttrs.addNew(Hint.PragmaNameLoc->getIdentifierInfo(), Hint.Range,
                     AttributeScopeInfo(), ArgHints, /*numArgs=*/4,
                     ParsedAttr::Form::Pragma());
  }

  // Get the next statement.
  MaybeParseCXX11Attributes(Attrs);

  ParsedAttributes EmptyDeclSpecAttrs(AttrFactory);
  StmtResult S = ParseStatementOrDeclarationAfterAttributes(
      Stmts, StmtCtx, TrailingElseLoc, Attrs, EmptyDeclSpecAttrs,
      PrecedingLabel);

  Attrs.takeAllPrependingFrom(TempAttrs);

  // Start of attribute range may already be set for some invalid input.
  // See PR46336.
  if (Attrs.Range.getBegin().isInvalid())
    Attrs.Range.setBegin(StartLoc);

  return S;
}

Decl *Parser::ParseFunctionStatementBody(Decl *Decl, ParseScope &BodyScope) {
  assert(Tok.is(tok::l_brace));
  SourceLocation LBraceLoc = Tok.getLocation();

  PrettyDeclStackTraceEntry CrashInfo(Actions.Context, Decl, LBraceLoc,
                                      "parsing function body");

  // Save and reset current vtordisp stack if we have entered a C++ method body.
  bool IsCXXMethod =
      getLangOpts().CPlusPlus && Decl && isa<CXXMethodDecl>(Decl);
  Sema::PragmaStackSentinelRAII
    PragmaStackSentinel(Actions, "InternalPragmaState", IsCXXMethod);

  // Do not enter a scope for the brace, as the arguments are in the same scope
  // (the function body) as the body itself.  Instead, just read the statement
  // list and put it into a CompoundStmt for safe keeping.
  StmtResult FnBody(ParseCompoundStatementBody());

  // If the function body could not be parsed, make a bogus compoundstmt.
  if (FnBody.isInvalid()) {
    Sema::CompoundScopeRAII CompoundScope(Actions);
    FnBody = Actions.ActOnCompoundStmt(LBraceLoc, LBraceLoc, {}, false);
  }

  BodyScope.Exit();
  return Actions.ActOnFinishFunctionBody(Decl, FnBody.get());
}

Decl *Parser::ParseFunctionTryBlock(Decl *Decl, ParseScope &BodyScope) {
  assert(Tok.is(tok::kw_try) && "Expected 'try'");
  SourceLocation TryLoc = ConsumeToken();

  PrettyDeclStackTraceEntry CrashInfo(Actions.Context, Decl, TryLoc,
                                      "parsing function try block");

  // Constructor initializer list?
  if (Tok.is(tok::colon))
    ParseConstructorInitializer(Decl);
  else
    Actions.ActOnDefaultCtorInitializers(Decl);

  // Save and reset current vtordisp stack if we have entered a C++ method body.
  bool IsCXXMethod =
      getLangOpts().CPlusPlus && Decl && isa<CXXMethodDecl>(Decl);
  Sema::PragmaStackSentinelRAII
    PragmaStackSentinel(Actions, "InternalPragmaState", IsCXXMethod);

  SourceLocation LBraceLoc = Tok.getLocation();
  StmtResult FnBody(ParseCXXTryBlockCommon(TryLoc, /*FnTry*/true));
  // If we failed to parse the try-catch, we just give the function an empty
  // compound statement as the body.
  if (FnBody.isInvalid()) {
    Sema::CompoundScopeRAII CompoundScope(Actions);
    FnBody = Actions.ActOnCompoundStmt(LBraceLoc, LBraceLoc, {}, false);
  }

  BodyScope.Exit();
  return Actions.ActOnFinishFunctionBody(Decl, FnBody.get());
}

bool Parser::trySkippingFunctionBody() {
  assert(SkipFunctionBodies &&
         "Should only be called when SkipFunctionBodies is enabled");
  if (!PP.isCodeCompletionEnabled()) {
    SkipFunctionBody();
    return true;
  }

  // We're in code-completion mode. Skip parsing for all function bodies unless
  // the body contains the code-completion point.
  TentativeParsingAction PA(*this);
  bool IsTryCatch = Tok.is(tok::kw_try);
  CachedTokens Toks;
  bool ErrorInPrologue = ConsumeAndStoreFunctionPrologue(Toks);
  if (llvm::any_of(Toks, [](const Token &Tok) {
        return Tok.is(tok::code_completion);
      })) {
    PA.Revert();
    return false;
  }
  if (ErrorInPrologue) {
    PA.Commit();
    SkipMalformedDecl();
    return true;
  }
  if (!SkipUntil(tok::r_brace, StopAtCodeCompletion)) {
    PA.Revert();
    return false;
  }
  while (IsTryCatch && Tok.is(tok::kw_catch)) {
    if (!SkipUntil(tok::l_brace, StopAtCodeCompletion) ||
        !SkipUntil(tok::r_brace, StopAtCodeCompletion)) {
      PA.Revert();
      return false;
    }
  }
  PA.Commit();
  return true;
}

StmtResult Parser::ParseCXXTryBlock() {
  assert(Tok.is(tok::kw_try) && "Expected 'try'");

  SourceLocation TryLoc = ConsumeToken();
  return ParseCXXTryBlockCommon(TryLoc);
}

StmtResult Parser::ParseCXXTryBlockCommon(SourceLocation TryLoc, bool FnTry) {
  if (Tok.isNot(tok::l_brace))
    return StmtError(Diag(Tok, diag::err_expected) << tok::l_brace);

  StmtResult TryBlock(ParseCompoundStatement(
      /*isStmtExpr=*/false,
      Scope::DeclScope | Scope::TryScope | Scope::CompoundStmtScope |
          (FnTry ? Scope::FnTryCatchScope : Scope::NoScope)));
  if (TryBlock.isInvalid())
    return TryBlock;

  // Borland allows SEH-handlers with 'try'

  if ((Tok.is(tok::identifier) &&
       Tok.getIdentifierInfo() == getSEHExceptKeyword()) ||
      Tok.is(tok::kw___finally)) {
    // TODO: Factor into common return ParseSEHHandlerCommon(...)
    StmtResult Handler;
    if(Tok.getIdentifierInfo() == getSEHExceptKeyword()) {
      SourceLocation Loc = ConsumeToken();
      Handler = ParseSEHExceptBlock(Loc);
    }
    else {
      SourceLocation Loc = ConsumeToken();
      Handler = ParseSEHFinallyBlock(Loc);
    }
    if(Handler.isInvalid())
      return Handler;

    return Actions.ActOnSEHTryBlock(true /* IsCXXTry */,
                                    TryLoc,
                                    TryBlock.get(),
                                    Handler.get());
  }
  else {
    StmtVector Handlers;

    // C++11 attributes can't appear here, despite this context seeming
    // statement-like.
    DiagnoseAndSkipCXX11Attributes();

    if (Tok.isNot(tok::kw_catch))
      return StmtError(Diag(Tok, diag::err_expected_catch));
    while (Tok.is(tok::kw_catch)) {
      StmtResult Handler(ParseCXXCatchBlock(FnTry));
      if (!Handler.isInvalid())
        Handlers.push_back(Handler.get());
    }
    // Don't bother creating the full statement if we don't have any usable
    // handlers.
    if (Handlers.empty())
      return StmtError();

    return Actions.ActOnCXXTryBlock(TryLoc, TryBlock.get(), Handlers);
  }
}

StmtResult Parser::ParseCXXCatchBlock(bool FnCatch) {
  assert(Tok.is(tok::kw_catch) && "Expected 'catch'");

  SourceLocation CatchLoc = ConsumeToken();

  BalancedDelimiterTracker T(*this, tok::l_paren);
  if (T.expectAndConsume())
    return StmtError();

  // C++ 3.3.2p3:
  // The name in a catch exception-declaration is local to the handler and
  // shall not be redeclared in the outermost block of the handler.
  ParseScope CatchScope(
      this, Scope::DeclScope | Scope::ControlScope | Scope::CatchScope |
                (FnCatch ? Scope::FnTryCatchScope : Scope::NoScope));

  // exception-declaration is equivalent to '...' or a parameter-declaration
  // without default arguments.
  Decl *ExceptionDecl = nullptr;
  if (Tok.isNot(tok::ellipsis)) {
    ParsedAttributes Attributes(AttrFactory);
    MaybeParseCXX11Attributes(Attributes);

    DeclSpec DS(AttrFactory);

    if (ParseCXXTypeSpecifierSeq(DS))
      return StmtError();

    Declarator ExDecl(DS, Attributes, DeclaratorContext::CXXCatch);
    ParseDeclarator(ExDecl);
    ExceptionDecl = Actions.ActOnExceptionDeclarator(getCurScope(), ExDecl);
  } else
    ConsumeToken();

  T.consumeClose();
  if (T.getCloseLocation().isInvalid())
    return StmtError();

  if (Tok.isNot(tok::l_brace))
    return StmtError(Diag(Tok, diag::err_expected) << tok::l_brace);

  // FIXME: Possible draft standard bug: attribute-specifier should be allowed?
  StmtResult Block(ParseCompoundStatement());
  if (Block.isInvalid())
    return Block;

  return Actions.ActOnCXXCatchBlock(CatchLoc, ExceptionDecl, Block.get());
}

void Parser::ParseMicrosoftIfExistsStatement(StmtVector &Stmts) {
  IfExistsCondition Result;
  if (ParseMicrosoftIfExistsCondition(Result))
    return;

  // Handle dependent statements by parsing the braces as a compound statement.
  // This is not the same behavior as Visual C++, which don't treat this as a
  // compound statement, but for Clang's type checking we can't have anything
  // inside these braces escaping to the surrounding code.
  if (Result.Behavior == IfExistsBehavior::Dependent) {
    if (!Tok.is(tok::l_brace)) {
      Diag(Tok, diag::err_expected) << tok::l_brace;
      return;
    }

    StmtResult Compound = ParseCompoundStatement();
    if (Compound.isInvalid())
      return;

    StmtResult DepResult = Actions.ActOnMSDependentExistsStmt(Result.KeywordLoc,
                                                              Result.IsIfExists,
                                                              Result.SS,
                                                              Result.Name,
                                                              Compound.get());
    if (DepResult.isUsable())
      Stmts.push_back(DepResult.get());
    return;
  }

  BalancedDelimiterTracker Braces(*this, tok::l_brace);
  if (Braces.consumeOpen()) {
    Diag(Tok, diag::err_expected) << tok::l_brace;
    return;
  }

  switch (Result.Behavior) {
  case IfExistsBehavior::Parse:
    // Parse the statements below.
    break;

  case IfExistsBehavior::Dependent:
    llvm_unreachable("Dependent case handled above");

  case IfExistsBehavior::Skip:
    Braces.skipToEnd();
    return;
  }

  // Condition is true, parse the statements.
  while (Tok.isNot(tok::r_brace)) {
    StmtResult R =
        ParseStatementOrDeclaration(Stmts, ParsedStmtContext::Compound);
    if (R.isUsable())
      Stmts.push_back(R.get());
  }
  Braces.consumeClose();
}

bool Parser::isC4File() {
  return getLangOpts().C4();
}

bool Parser::isC4CaseLabel() {
  if (!isC4File())
    return false;

  unsigned Offset = 0;
  bool HasQuestion = false;
  while (true) {
    const Token &T = PP.LookAhead(Offset);
    if (T.is(tok::eof))
      return false;
    if (Offset > 0 && T.isAtStartOfLine())
      return false;
    if (T.is(tok::colon)) {
      return !HasQuestion;
    }
    if (T.isOneOf(tok::semi, tok::l_brace, tok::r_brace))
      return false;
    if (T.is(tok::question))
      HasQuestion = true;
    
    if (T.isOneOf(tok::l_paren, tok::l_square)) {
      tok::TokenKind CloseKind = T.is(tok::l_paren) ? tok::r_paren : tok::r_square;
      unsigned Depth = 1;
      while (Depth > 0) {
        Offset++;
        const Token &NT = PP.LookAhead(Offset);
        if (NT.is(tok::eof) || NT.isOneOf(tok::semi, tok::l_brace, tok::r_brace))
          return false;
        if (NT.is(T.getKind()))
          Depth++;
        else if (NT.is(CloseKind))
          Depth--;
      }
    }
    Offset++;
  }
  return false;
}
