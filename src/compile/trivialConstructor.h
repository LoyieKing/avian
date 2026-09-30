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

#ifndef AVIAN_COMPILE_TRIVIAL_CONSTRUCTOR_H
#define AVIAN_COMPILE_TRIVIAL_CONSTRUCTOR_H

// Inlines a constructor whose body is aload_0, invokespecial of an
// empty <init>()V, then zero or more (aload_0, one parameter load,
// putfield), then return. A computed value, a branch, or any other
// call stays a real invoke.
//
// Every object putfield goes through ObjectStore, which uses the fact
// recorded at this invokespecial. At most one of them branches: the
// single object store that is also the last store. A slow path jumps
// to the join and must not skip a later store. Primitive stores are
// direct. Included from src/compile.cpp for the same reason as
// inlineNew.h.

class TrivialConstructor {
 public:
  // True when the invoke was replaced by the constructor's stores.
  // False leaves the operand stack untouched.
  static bool tryCompile(MyThread* t,
                         Frame* frame,
                         GcMethod* target,
                         unsigned callIp);

 private:
  TrivialConstructor();
};

#endif  // AVIAN_COMPILE_TRIVIAL_CONSTRUCTOR_H
