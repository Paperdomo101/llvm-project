#ifndef LLVM_C4_FEATURES_H
#define LLVM_C4_FEATURES_H
#include <clang/Basic/IdentifierTable.h>

namespace clang { class Expr; }

enum class ArrayKind { None, C4, C };

  struct LHSVarInfo {
      bool IsConst = false;
      bool IsStatic = false;
      ArrayKind Kind = ArrayKind::None;
      clang::IdentifierInfo *Ident = nullptr;
      clang::SourceLocation IdentLoc;
      /// Non-null for the sized [N] form of a C4 array prefix in a type-inferred
      /// assignment (`[N] id := init`).  Null for the unsized [] form and for
      /// non-array variables.
      clang::Expr *SizeExpr = nullptr;
  };

#endif
