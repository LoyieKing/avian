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

#ifndef AVIAN_COMPILE_OBJECT_STORE_H
#define AVIAN_COMPILE_OBJECT_STORE_H

// Reference store used by putfield, aastore, and inlined constructors.
//
// Fresh (ObjectStoreFacts) is a plain store: no null check, no mark.
// Otherwise, on 64-bit, one condJump against the thread chunk when the
// instruction has two spare bytes. The fast path is the plain store.
// The slow path is store plus mark when the receiver is non-null, and
// setMaybeNull when it might be null. aastore has no spare byte, so it
// never takes that branch. 32-bit builds have no range check.
//
// object and value must still be compiler-stack homes when allowBranch
// is true. saveState's slow edge reloads them from there. This does not
// pop. Included from src/compile.cpp for the same reason as inlineNew.h.

class ObjectStore {
 public:
  static void store(MyThread* t,
                    Frame* frame,
                    unsigned ip,
                    ir::Value* object,
                    ir::Value* value,
                    int offset,
                    unsigned holeOpcode,
                    bool allowBranch);

  static void storeElement(MyThread* t,
                           Frame* frame,
                           unsigned ip,
                           ir::Value* array,
                           ir::Value* index,
                           ir::Value* value);

 private:
  ObjectStore();
};

#endif  // AVIAN_COMPILE_OBJECT_STORE_H
