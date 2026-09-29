/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef ARCH_H
#define ARCH_H

#ifdef _MSC_VER
#include "windows.h"
#pragma push_macro("assert")
#include "intrin.h"
#pragma pop_macro("assert")
#undef interface
#endif

#include "avian/common.h"

extern "C" void NO_RETURN vmJump(void* address,
                                 void* frame,
                                 void* stack,
                                 void* thread,
                                 uintptr_t returnLow,
                                 uintptr_t returnHigh);

// Same as vmJump, but also copies returnLow into the float return
// register (xmm0 / d0) so ForceEarlyReturn of float and double works.
// On i386 and arm32 this is an alias of vmJump: those ABIs return
// floats in the x87/VFP bank, which this stub does not load.
extern "C" void NO_RETURN vmJumpFloat(void* address,
                                      void* frame,
                                      void* stack,
                                      void* thread,
                                      uintptr_t returnLow,
                                      uintptr_t returnHigh);

namespace vm {

inline void compileTimeMemoryBarrier()
{
#ifdef _MSC_VER
  _ReadWriteBarrier();
#else
  __asm__ __volatile__("" : : : "memory");
#endif
}

}  // namespace vm

#if (defined ARCH_x86_32) || (defined ARCH_x86_64)
#include "x86.h"
#elif (defined ARCH_arm) || (defined ARCH_arm64)
#include "arm.h"
#else
#error unsupported architecture
#endif

#endif  // ARCH_H
