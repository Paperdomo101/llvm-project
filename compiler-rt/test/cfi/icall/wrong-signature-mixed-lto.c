// Test that the checking is done with the actual type of f() even when the
// calling module has an incorrect declaration. Test a mix of lto types.
//
// -flto below overrides -flto=thin in %clang_cfi
// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %clang_cfi %t/a.c -c -o %t1_a.o
// RUN: %clang_cfi %t/b.c -c -o %t1_b.o -flto
// RUN: %clang_cfi %t1_a.o %t1_b.o -o %t1
// RUN: %expect_crash %t1 2>&1 | FileCheck --check-prefix=CFI %s
//
// RUN: %clang_cfi %t/a.c -c -o %t2_a.o -flto
// RUN: %clang_cfi %t/b.c -c -o %t2_b.o
// RUN: %clang_cfi %t2_a.o %t2_b.o -o %t2
// RUN: %expect_crash %t2 2>&1 | FileCheck --check-prefix=CFI %s
//
// RUN: %clang_cfi %t/a.c -c -o %t3_a.o
// RUN: %clang_cfi %t/b.c -c -o %t3_b.o
// RUN: %clang_cfi %t3_a.o %t3_b.o -o %t3
// RUN: %expect_crash %t3 2>&1 | FileCheck --check-prefix=CFI %s
//
// REQUIRES: thinlto

//--- a.c
#include <stdio.h>

void f();

int main() {
  // CFI: 1
  fprintf(stderr, "1\n");

  void (*volatile p)() = &f;
  p();

  // CFI-NOT: 2
  fprintf(stderr, "2\n");
}

//--- b.c
int f() { return 42; }
