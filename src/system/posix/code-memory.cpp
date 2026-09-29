/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include <avian/system/code-memory.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#include <sys/mman.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

namespace avian {
namespace system {

// ---------------------------------------------------------------------------
// Allocation (shared by all backends)

namespace {

size_t padToAlignment(size_t n)
{
  return (n + CodeMemory::Alignment - 1) & ~(CodeMemory::Alignment - 1);
}

}  // namespace

uint8_t* CodeMemory::allocate(size_t size)
{
  size_t end = used_ + padToAlignment(size);

  if (end > region_.count or end < used_) {
    return 0;
  }

  uint8_t* start = region_.begin() + used_;
  used_ = end;
  return start;
}

void CodeMemory::free(uint8_t* start, size_t size)
{
  if (start + padToAlignment(size) == region_.begin() + used_) {
    used_ = start - region_.begin();
  }
}

namespace {

// ---------------------------------------------------------------------------
// Low-level helpers.  These are the only stores to executable memory in
// the VM.

// Makes [start, start + size) coherent between the data and instruction
// side for every thread.
void flushInstructionCache(void* start, size_t size)
{
#if defined(__APPLE__)
  sys_icache_invalidate(start, size);
#elif defined(__i386__) || defined(__x86_64__)
  // x86 keeps instruction fetch coherent with stores; just keep the
  // compiler from sinking them past this point.
  (void)start;
  (void)size;
  __atomic_thread_fence(__ATOMIC_RELEASE);
#else
  __builtin___clear_cache(static_cast<char*>(start),
                          static_cast<char*>(start) + size);
#endif
}

// Stores `size` bytes at `dst`, atomically when the store is naturally
// aligned and word-sized, so that concurrently executing threads see
// either the old or the new bytes (this is what makes retargeting a
// call site safe while other threads may be running through it).
void storeCode(void* dst, const void* src, unsigned size)
{
  uintptr_t address = reinterpret_cast<uintptr_t>(dst);

  if (size == 4 and address % 4 == 0) {
    uint32_t v;
    memcpy(&v, src, 4);
    __atomic_store_n(static_cast<uint32_t*>(dst), v, __ATOMIC_RELEASE);
  } else if (sizeof(void*) == 8 and size == 8 and address % 8 == 0) {
    uint64_t v;
    memcpy(&v, src, 8);
    __atomic_store_n(static_cast<uint64_t*>(dst), v, __ATOMIC_RELEASE);
  } else {
    memcpy(dst, src, size);
  }
}

void* mapAnonymous(size_t size, int prot, int extraFlags)
{
  void* p = mmap(0, size, prot, MAP_PRIVATE | MAP_ANON | extraFlags, -1, 0);
  return p == MAP_FAILED ? 0 : p;
}

// Map code near the executable and its libraries when we can, so calls
// between them fit in a rel32 displacement (see useLongJump in
// compile.cpp).  Darwin rejects MAP_32BIT for user mappings.
#if defined(MAP_32BIT) && !defined(__APPLE__)
const int LowMemoryFlag = MAP_32BIT;
#else
const int LowMemoryFlag = 0;
#endif

void reportMapFailure(const char* backend, size_t size)
{
  int error = errno;
  fprintf(stderr,
          "avian: cannot map %zu bytes of code memory (%s): %s\n",
          size,
          backend,
          strerror(error));
}

template <class T>
T* allocateInstance(util::Alloc* allocator)
{
  return static_cast<T*>(allocator->allocate(sizeof(T)));
}

// ---------------------------------------------------------------------------
// Boot image: memory that is written but never executed.

class ImageCodeMemory : public CodeMemory {
 public:
  ImageCodeMemory(util::Alloc* allocator, util::Slice<uint8_t> image)
      : CodeMemory(image), allocator(allocator)
  {
  }

  virtual const char* name()
  {
    return "image";
  }

  virtual uint8_t* stagingBuffer(uint8_t* address, size_t)
  {
    // Assemble in place: boot image generation resolves some fixups
    // after the fact (promise listeners), and those have to land in
    // the image itself.
    return address;
  }

  virtual void commit(uint8_t* address, const void* src, size_t size)
  {
    if (src != address) {
      memcpy(address, src, size);
    }
  }

  virtual void patch(void* address, const void* src, unsigned size)
  {
    memcpy(address, src, size);
  }

  virtual void dispose()
  {
    util::Alloc* a = allocator;
    this->~ImageCodeMemory();
    a->free(this, sizeof(*this));
  }

  util::Alloc* allocator;
};

// ---------------------------------------------------------------------------
// A single mapping that is readable, writable and executable.
//
// Code could be assembled in place here, but we deliberately stage it
// elsewhere and copy it on commit, exactly as on W^X platforms, so that
// every platform runs the same code path and a codegen change that
// writes where it shouldn't fails everywhere, not just on Apple
// silicon.  The copy is noise next to the cost of compiling.

class RwxCodeMemory : public CodeMemory {
 public:
  RwxCodeMemory(util::Alloc* allocator, util::Slice<uint8_t> region)
      : CodeMemory(region), allocator(allocator)
  {
  }

  static CodeMemory* make(util::Alloc* allocator, size_t capacity)
  {
    void* p = mapAnonymous(
        capacity, PROT_READ | PROT_WRITE | PROT_EXEC, LowMemoryFlag);
    if (p == 0) {
      reportMapFailure("rwx", capacity);
      return 0;
    }

    return new (allocateInstance<RwxCodeMemory>(allocator)) RwxCodeMemory(
        allocator, util::Slice<uint8_t>(static_cast<uint8_t*>(p), capacity));
  }

  virtual const char* name()
  {
    return "rwx";
  }

  virtual uint8_t* stagingBuffer(uint8_t*, size_t)
  {
    return 0;
  }

  virtual void commit(uint8_t* address, const void* src, size_t size)
  {
    memcpy(address, src, size);
    flushInstructionCache(address, size);
  }

  virtual void patch(void* address, const void* src, unsigned size)
  {
    storeCode(address, src, size);
    flushInstructionCache(address, size);
  }

  virtual void dispose()
  {
    munmap(region().begin(), region().count);
    util::Alloc* a = allocator;
    this->~RwxCodeMemory();
    a->free(this, sizeof(*this));
  }

  util::Alloc* allocator;
};

#if defined(__APPLE__) && defined(MAP_JIT)

// ---------------------------------------------------------------------------
// Darwin: a MAP_JIT mapping (required for writable+executable memory
// under the hardened runtime; the binary needs the
// com.apple.security.cs.allow-jit entitlement).
//
// On Apple silicon the hardware never lets a thread see such a page as
// writable and executable at once: pthread_jit_write_protect_np()
// switches the *calling thread's* view between RX (the default) and
// RW, while every other thread keeps executing through its own RX view.
// commit() and patch() are the only places that switch, and they
// switch back before returning, so the window is a single memcpy or
// store during which this thread runs no JIT code.  This is Apple's
// documented scheme and what JavaScriptCore, V8 and .NET use there.
//
// On Intel Macs pthread_jit_write_protect_supported_np() is false and
// the mapping is plain RWX.
class MapJitCodeMemory : public CodeMemory {
 public:
  MapJitCodeMemory(util::Alloc* allocator,
                   util::Slice<uint8_t> region,
                   bool toggle)
      : CodeMemory(region), allocator(allocator), toggle(toggle)
  {
  }

  static CodeMemory* make(util::Alloc* allocator, size_t capacity)
  {
    void* p = mapAnonymous(
        capacity, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_JIT);
    if (p == 0) {
      reportMapFailure("map-jit", capacity);
      fprintf(stderr,
              "avian: is the executable signed with the "
              "com.apple.security.cs.allow-jit entitlement?\n");
      return 0;
    }

    return new (allocateInstance<MapJitCodeMemory>(allocator)) MapJitCodeMemory(
        allocator,
        util::Slice<uint8_t>(static_cast<uint8_t*>(p), capacity),
        pthread_jit_write_protect_supported_np());
  }

  virtual const char* name()
  {
    return "map-jit";
  }

  virtual uint8_t* stagingBuffer(uint8_t*, size_t)
  {
    return 0;
  }

  virtual void commit(uint8_t* address, const void* src, size_t size)
  {
    beginWrite();
    memcpy(address, src, size);
    endWrite();
    flushInstructionCache(address, size);
  }

  virtual void patch(void* address, const void* src, unsigned size)
  {
    beginWrite();
    storeCode(address, src, size);
    endWrite();
    flushInstructionCache(address, size);
  }

  virtual void dispose()
  {
    munmap(region().begin(), region().count);
    util::Alloc* a = allocator;
    this->~MapJitCodeMemory();
    a->free(this, sizeof(*this));
  }

 private:
  void beginWrite()
  {
    if (toggle) {
      pthread_jit_write_protect_np(0);
    }
  }

  void endWrite()
  {
    if (toggle) {
      pthread_jit_write_protect_np(1);
    }
  }

  util::Alloc* allocator;
  bool toggle;
};

#endif  // __APPLE__ && MAP_JIT

}  // namespace

// ---------------------------------------------------------------------------
// Backend selection

CodeMemory* makeExecutableCodeMemory(util::Alloc* allocator, size_t capacity)
{
  const char* requested = getenv("AVIAN_CODE_MEMORY");
  if (requested and *requested == 0) {
    requested = 0;
  }

  if (requested and strcmp(requested, "rwx") == 0) {
    return RwxCodeMemory::make(allocator, capacity);
  }

#if defined(__APPLE__) && defined(MAP_JIT)
  if (requested == 0 or strcmp(requested, "map-jit") == 0) {
    return MapJitCodeMemory::make(allocator, capacity);
  }
#else
  if (requested == 0) {
    return RwxCodeMemory::make(allocator, capacity);
  }
#endif

  fprintf(stderr,
          "avian: AVIAN_CODE_MEMORY=%s is not supported on this platform\n",
          requested);
  return 0;
}

CodeMemory* makeImageCodeMemory(util::Alloc* allocator,
                                util::Slice<uint8_t> image)
{
  return new (allocateInstance<ImageCodeMemory>(allocator))
      ImageCodeMemory(allocator, image);
}

}  // namespace system
}  // namespace avian
