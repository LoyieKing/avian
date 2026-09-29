/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_COMPILE_RANGE_CHECK_ELIMINATION_H
#define AVIAN_COMPILE_RANGE_CHECK_ELIMINATION_H

#include <stdint.h>

namespace vm {

class Thread;
class Zone;
class GcMethod;

// Counted-loop range-check elimination.
//
// Same shape as HotSpot C1's RangeCheckElimination: the compiler calls one
// static entry, and RangeCheckEliminator in the .cpp owns the analysis.
// C1 can cut a check and deoptimize if the predicate fails. Avian cannot
// deoptimize, so a bytecode is marked only when its index is inside the
// array on every execution:
//
//   the loop index is stored a non-negative constant immediately before
//   the header, and the only other write is iinc by +1 (a signed i < limit
//   test never lets that +1 overflow);
//   the header compares that index with array.length, either reloaded each
//   iteration or held in a local that is the single arraylength of the
//   same array local;
//   nothing that reaches the access stores the array or that limit;
//   nothing branches into the body.
//
// The returned vector is indexed by bytecode ip. A nonzero byte means the
// compiler emits that *aload or *astore without a bounds check. Null means
// this method had nothing to remove. Storage comes from the compilation
// zone and lives as long as the compilation does.
class RangeCheckElimination {
 public:
  static uint8_t* eliminate(Thread* t, Zone* zone, GcMethod* method);

 private:
  RangeCheckElimination();
};

}  // namespace vm

#endif  // AVIAN_COMPILE_RANGE_CHECK_ELIMINATION_H
