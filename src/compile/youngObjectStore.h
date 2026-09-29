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

#ifndef AVIAN_COMPILE_YOUNG_OBJECT_STORE_H
#define AVIAN_COMPILE_YOUNG_OBJECT_STORE_H

// Plain store of one reference field when the object is inside the
// current thread chunk.
//
// The chunk is not a slice of gen2 and the object is not a fixie, so
// needsMark is false and the remembered set does not change. Null,
// tenured, and fixed objects fail the range test and take setMaybeNull.
// invokespecial has two spare bytes, so this is one condJump. The fast
// path is 64-bit only.
//
// Included from src/compile.cpp for the same reason as inlineNew.h.

class YoungObjectStore {
 public:
  // True when the plain store and its slow path were emitted. False
  // means nothing was emitted.
  static bool tryCompile(MyThread* t,
                         Frame* frame,
                         unsigned callIp,
                         ir::Value* self,
                         ir::Value* value,
                         int offset);

  // Slow paths queued by tryCompile. Called after InlineNew::flush.
  static void flush(MyThread* t, Context* context);

 private:
  YoungObjectStore();
};

#endif  // AVIAN_COMPILE_YOUNG_OBJECT_STORE_H
