/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

// Private to the CodeMemory implementation (src/system/code-memory.cpp
// and the per-OS backends in src/system/<os>/code-memory.cpp).  Nothing
// else in the VM may include this.

#ifndef AVIAN_SYSTEM_CODE_MEMORY_BACKEND_H
#define AVIAN_SYSTEM_CODE_MEMORY_BACKEND_H

#include <avian/system/code-memory.h>

#include <new>

namespace avian {
namespace system {
namespace code_memory {

// Stores `size` bytes of code at `dst` (a writable view of live code).
// A naturally aligned 4-byte store, or 8-byte store on 64-bit hosts, is
// performed as a single store instruction after a store-store barrier,
// so a thread executing through the code concurrently sees either the
// old or the new bytes, never a mix, and never the new bytes before
// anything they depend on.  Other sizes are plain copies.
void storeCode(void* dst, const void* src, unsigned size);

// Constructs a backend object in memory from `allocator`; backends free
// themselves in dispose() (the VM links no operator delete).
template <class T, class... Args>
T* construct(util::Alloc* allocator, const Args&... args)
{
  return new (allocator->allocate(sizeof(T))) T(allocator, args...);
}

template <class T>
void destroy(T* object, util::Alloc* allocator)
{
  object->~T();
  allocator->free(object, sizeof(T));
}

}  // namespace code_memory
}  // namespace system
}  // namespace avian

#endif  // AVIAN_SYSTEM_CODE_MEMORY_BACKEND_H
