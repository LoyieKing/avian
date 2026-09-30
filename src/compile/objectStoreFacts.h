/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_COMPILE_OBJECT_STORE_FACTS_H
#define AVIAN_COMPILE_OBJECT_STORE_FACTS_H

#include <stdint.h>

namespace vm {

class Thread;
class Zone;
class GcMethod;

// Nullness of the receiver at a reference store, indexed by bytecode ip.
//
// Fresh: allocated in this method, and no safepoint has run since. The
// object is not a tenured fixie and not in gen2, so needsMark is false
// and a plain store is the write barrier. Fresh implies non-null.
// NonNull: not null, but may already be tenured. The store must still
// mark unless a range check shows the object is in the thread chunk.
// Zero: might be null. The null check stays.
//
// Bits are recorded at putfield (the object), aastore (the array), and
// invokespecial (the receiver, so an inlined constructor can see the
// allocation). Unreached ips are zero, never Fresh. Null means this
// method had nothing to remove. The pool is not resolved: resolution
// can initialize a class. Avian cannot deoptimize, so a bit is set only
// when every path agrees, including loop back-edges and handlers.
class ObjectStoreFacts {
 public:
  static const uint8_t NonNull = 1;
  static const uint8_t Fresh = 2;

  static uint8_t* analyze(Thread* t, Zone* zone, GcMethod* method);

 private:
  ObjectStoreFacts();
};

}  // namespace vm

#endif  // AVIAN_COMPILE_OBJECT_STORE_FACTS_H
