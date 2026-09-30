/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_COMPILE_CPP_INCLUDE
#error "include this file from src/compile.cpp"
#endif

#ifndef AVIAN_COMPILE_INLINE_NEW_H
#define AVIAN_COMPILE_INLINE_NEW_H

// Inlined `new` of an ordinary object into the calling thread's TLAB.
//
// Same entry shape as HotSpot C1's GraphBuilder allocation and as
// RangeCheckElimination: the compiler calls one static method, and the
// .cpp owns the bump and the slow path. C1 can deoptimize out of a
// speculative fast path. Avian cannot, so the bump is taken only when
// the class is already prepared, has no finalizer and is not a weak
// reference, and the current TLAB has room. The slow path is the existing
// makeNew64 call.
//
// Frame and Context are local to src/compile.cpp, so this header is
// included from inside that translation unit. A separate object could
// not name those types. The fast path is 64-bit only. A 32-bit build
// keeps the makeNew call.

class InlineNew {
 public:
  // True when this new_ was replaced by the bump. False means nothing
  // was emitted and the walker uses the existing makeNew call.
  static bool tryCompile(MyThread* t,
                         Context* context,
                         Frame* frame,
                         GcClass* class_);

  // Slow paths queued by tryCompile. Called after the bytecode walk.
  static void flush(MyThread* t, Context* context);

  // Aborts when GcClass::vmFlags is not at the offset the bump loads.
  static void checkClassVmFlagsOffset(MyThread* t, Context* context);

 private:
  InlineNew();
};

#endif  // AVIAN_COMPILE_INLINE_NEW_H
