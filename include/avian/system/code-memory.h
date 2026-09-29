/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_SYSTEM_CODE_MEMORY_H
#define AVIAN_SYSTEM_CODE_MEMORY_H

#include <avian/util/allocator.h>
#include <avian/util/slice.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace avian {
namespace system {

// CodeMemory owns the region the VM places machine code in, and is the
// only component allowed to write to memory that may be executed.
//
// A piece of code goes through these steps:
//
//   1. allocate() reserves space and yields the address the code will
//      run at.  The caller may not write there.
//   2. The code is assembled into a staging buffer (ordinary memory,
//      see Writer), linked to run at that address.
//   3. commit() moves it into place and makes it visible to instruction
//      fetch.  Only after that may anything that other threads can see
//      point at the code.
//   4. patch() later retargets small pieces of live code (call sites),
//      possibly while other threads are executing it.
//
//   free() gives back space whose code was never published (compile
//   failure rollback).
//
// How executable memory is made writable is a property of the backend
// alone; nothing outside the implementation toggles permissions or
// flushes instruction caches.  Backends (see code-memory.cpp):
//
//   * Darwin: one MAP_JIT mapping; on Apple silicon, commit() and
//     patch() flip the calling thread's pthread_jit_write_protect_np
//     state around their store and back before returning.
//   * Elsewhere: a single read/write/execute mapping.
//   * Boot image generation: a plain buffer that is never executed.
//
// Threading: allocate() and free() must be serialized by the caller
// (the VM holds classLock).  commit() on distinct ranges and patch()
// may run concurrently with each other and with threads executing
// code.  None of these operations call back into the VM or execute JIT
// code.
class CodeMemory {
 public:
  class Writer;

  // Short backend name, for diagnostics ("map-jit", "dual-map", ...).
  virtual const char* name() = 0;

  // The region code is allocated from, and how much of it is in use.
  // Allocation is bump-pointer style from region().begin().
  util::Slice<uint8_t> region()
  {
    return region_;
  }

  size_t used()
  {
    return used_;
  }

  bool contains(const void* p)
  {
    return p >= region_.begin() and p < region_.begin() + region_.count;
  }

  // Granularity of allocation: every allocation starts at a multiple
  // of this from region().begin(), which is itself page aligned (or,
  // for a boot image, allocated by the heap).  16 bytes is a typical
  // function alignment and more than any patchable site needs.
  static const size_t Alignment = 16;

  // Reserves `size` bytes and returns the address the code will run
  // at, or null if the region is exhausted.
  uint8_t* allocate(size_t size);

  // Gives back an allocation whose code was never published.  Space is
  // reclaimed only if it is the most recent allocation; otherwise it is
  // simply not reused.
  void free(uint8_t* start, size_t size);

  // Where code destined for [address, address + size) may be assembled
  // in place: `address` itself for memory that is never executed, a
  // writable alias for dual-mapped memory, or null when the caller has
  // to stage the code in memory of its own.  Writer wraps this choice.
  virtual uint8_t* stagingBuffer(uint8_t* address, size_t size) = 0;

  // Publishes `size` bytes of finished code, linked to run at
  // `address`, from `src` (either the staging buffer returned for that
  // range, or ordinary memory): stores them at `address` and makes the
  // range coherent with instruction fetch on all threads.
  virtual void commit(uint8_t* address, const void* src, size_t size) = 0;

  // Replaces `size` (at most 8) bytes of live code at `address` with
  // `src` and makes the change visible to instruction fetch.  If
  // `address` is naturally aligned and `size` is 4 or 8 (on 64-bit
  // hosts), the store is single-copy atomic, so a thread executing the
  // code concurrently sees either the old or the new bytes.
  virtual void patch(void* address, const void* src, unsigned size) = 0;

  // Unmaps the region and frees this object.
  virtual void dispose() = 0;

 protected:
  CodeMemory(util::Slice<uint8_t> region) : region_(region), used_(0)
  {
  }

  // Not virtual: instances are destroyed through dispose() (the VM
  // doesn't link the C++ runtime, so there is no operator delete).
  ~CodeMemory()
  {
  }

 private:
  util::Slice<uint8_t> region_;
  size_t used_;
};

// Stages one piece of code for [address, address + size) and commits
// it:
//
//   CodeMemory::Writer writer(memory, start, size, zone);
//   assembler->write(writer.buffer(), start);
//   writer.commit();
//
// The buffer is either provided by the backend (in-place or alias) or
// taken from `scratch`, which must outlive the writer (a compilation's
// Zone, typically).
class CodeMemory::Writer {
 public:
  Writer(CodeMemory* memory,
         uint8_t* address,
         size_t size,
         util::AllocOnly* scratch)
      : memory(memory),
        address_(address),
        size(size),
        buffer_(memory->stagingBuffer(address, size))
  {
    if (buffer_ == 0) {
      buffer_ = static_cast<uint8_t*>(scratch->allocate(size));
      // Keep padding bytes the assembler doesn't write deterministic.
      memset(buffer_, 0, size);
    }
  }

  // Where to write the code.
  uint8_t* buffer() const
  {
    return buffer_;
  }

  // Where the code will run; what it must be linked against.
  uint8_t* address() const
  {
    return address_;
  }

  void commit()
  {
    memory->commit(address_, buffer_, size);
  }

 private:
  Writer(const Writer&);
  Writer& operator=(const Writer&);

  CodeMemory* memory;
  uint8_t* address_;
  size_t size;
  uint8_t* buffer_;
};

// Maps `capacity` bytes of executable memory using the best backend
// available on this platform; returns null on failure.  The environment
// variable AVIAN_CODE_MEMORY may name a backend to use instead (see
// code-memory.cpp), for diagnosis and testing.
CodeMemory* makeExecutableCodeMemory(util::Alloc* allocator, size_t capacity);

// Wraps `image`, memory that is written but never executed (the code
// section of a boot image under construction).  Code is assembled in
// place.  The caller keeps ownership of `image`.
CodeMemory* makeImageCodeMemory(util::Alloc* allocator,
                                util::Slice<uint8_t> image);

}  // namespace system
}  // namespace avian

#endif  // AVIAN_SYSTEM_CODE_MEMORY_H
