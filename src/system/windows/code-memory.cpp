/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

// Executable CodeMemory backend for Windows.  With the shared code in
// ../code-memory.cpp this is the only code in the VM that stores to
// executable memory.
//
// Windows gets the plain read/write/execute backend, as before this
// subsystem existed.  A W^X backend here would be a pagefile-backed
// section (CreateFileMapping) mapped twice with MapViewOfFile, the
// Windows analogue of the Linux dual-map backend; it slots in behind
// the same interface if Arbitrary Code Guard or similar policies ever
// need supporting.

#include "../code-memory-backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

namespace avian {
namespace system {

namespace {

void flushInstructionCache(void* start, size_t size)
{
  FlushInstructionCache(GetCurrentProcess(), start, size);
}

class RwxCodeMemory : public CodeMemory {
 public:
  RwxCodeMemory(util::Alloc* allocator, util::Slice<uint8_t> region)
      : CodeMemory(region), allocator(allocator)
  {
  }

  static CodeMemory* make(util::Alloc* allocator, size_t capacity)
  {
    void* p = VirtualAlloc(
        0, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (p == 0) {
      fprintf(stderr,
              "avian: cannot allocate %lu bytes of code memory (rwx): "
              "error %lu\n",
              static_cast<unsigned long>(capacity),
              static_cast<unsigned long>(GetLastError()));
      return 0;
    }

    return code_memory::construct<RwxCodeMemory>(
        allocator, util::Slice<uint8_t>(static_cast<uint8_t*>(p), capacity));
  }

  virtual const char* name()
  {
    return "rwx";
  }

  virtual uint8_t* stagingBuffer(uint8_t*, size_t)
  {
    // Stage elsewhere and copy on commit, as on W^X platforms, so every
    // platform exercises the same path (see the posix rwx backend).
    return 0;
  }

  virtual void commit(uint8_t* address, const void* src, size_t size)
  {
    memcpy(address, src, size);
    flushInstructionCache(address, size);
  }

  virtual void patch(void* address, const void* src, unsigned size)
  {
    code_memory::storeCode(address, src, size);
    flushInstructionCache(address, size);
  }

  virtual void dispose()
  {
    VirtualFree(region().begin(), 0, MEM_RELEASE);
    code_memory::destroy(this, allocator);
  }

 private:
  util::Alloc* allocator;
};

}  // namespace

CodeMemory* makeExecutableCodeMemory(util::Alloc* allocator, size_t capacity)
{
  const char* requested = getenv("AVIAN_CODE_MEMORY");
  if (requested == 0 or *requested == 0 or strcmp(requested, "rwx") == 0) {
    return RwxCodeMemory::make(allocator, capacity);
  }

  fprintf(stderr,
          "avian: AVIAN_CODE_MEMORY=%s is not supported on this platform\n",
          requested);
  return 0;
}

}  // namespace system
}  // namespace avian
