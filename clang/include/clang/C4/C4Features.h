#ifndef LLVM_C4_FEATURES_H
#define LLVM_C4_FEATURES_H
#include <clang/Basic/IdentifierTable.h>

enum class ArrayKind { None, C4, C };

  struct LHSVarInfo {
      bool IsConst = false;
      ArrayKind Kind = ArrayKind::None;
      clang::IdentifierInfo *Ident = nullptr;
      clang::SourceLocation IdentLoc;
  };

#endif
