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

#ifdef __linux__
#include <sys/syscall.h>
#endif

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

// As upstream: place code in the low 2GB when possible, which keeps
// more calls between it and the VM within rel32 reach (see useLongJump
// in compile.cpp).  Darwin rejects MAP_32BIT for user mappings.
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
    const int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    void* p = mapAnonymous(capacity, prot, LowMemoryFlag);
    if (p == 0 and LowMemoryFlag) {
      p = mapAnonymous(capacity, prot, 0);
    }
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

#if defined(__linux__)

// ---------------------------------------------------------------------------
// Linux: one memfd mapped twice.  The read+execute view is where code
// lives and runs (all addresses the VM hands out are in it); the
// read+write view, at a fixed distance, is where code is staged and
// patched.  No page is ever writable and executable at the same time,
// there is no permission switching (so no per-write syscalls and no
// thread-local state), and assembling into the alias makes commit() a
// pure instruction cache flush.  This is the scheme .NET uses by
// default on Linux.
//
// Caveat: a debugger that inserts a software breakpoint into JIT code
// through ptrace gets a private copy-on-write copy of that page in the
// executable view, after which patches made through the alias are no
// longer visible there.  Use AVIAN_CODE_MEMORY=rwx when debugging
// generated code that way.

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

int createMemfd(const char* name)
{
#ifdef SYS_memfd_create
  // Linux >= 6.3 (vm.memfd_noexec) wants executable memfds to say so;
  // older kernels reject the flag, so retry without it.
  int fd = syscall(SYS_memfd_create, name, MFD_CLOEXEC | MFD_EXEC);
  if (fd < 0 and errno == EINVAL) {
    fd = syscall(SYS_memfd_create, name, MFD_CLOEXEC);
  }
  return fd;
#else
  (void)name;
  errno = ENOSYS;
  return -1;
#endif
}

class DualMapCodeMemory : public CodeMemory {
 public:
  DualMapCodeMemory(util::Alloc* allocator,
                    util::Slice<uint8_t> region,
                    uint8_t* writable)
      : CodeMemory(region),
        allocator(allocator),
        delta(writable - region.begin())
  {
  }

  static CodeMemory* make(util::Alloc* allocator, size_t capacity, bool report)
  {
    int fd = createMemfd("avian-code");
    if (fd < 0) {
      if (report) {
        reportMapFailure("dual-map: memfd_create", capacity);
      }
      return 0;
    }

    void* executable = MAP_FAILED;
    void* writable = MAP_FAILED;

    if (ftruncate(fd, capacity) == 0) {
      executable = mmap(0,
                        capacity,
                        PROT_READ | PROT_EXEC,
                        MAP_SHARED | LowMemoryFlag,
                        fd,
                        0);
      if (executable == MAP_FAILED and LowMemoryFlag) {
        executable
            = mmap(0, capacity, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
      }

      if (executable != MAP_FAILED) {
        writable
            = mmap(0, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      }
    }

    int error = errno;
    close(fd);

    if (writable == MAP_FAILED) {
      if (executable != MAP_FAILED) {
        munmap(executable, capacity);
      }
      if (report) {
        errno = error;
        reportMapFailure("dual-map", capacity);
      }
      return 0;
    }

    DualMapCodeMemory* memory
        = new (allocateInstance<DualMapCodeMemory>(allocator))
            DualMapCodeMemory(
                allocator,
                util::Slice<uint8_t>(static_cast<uint8_t*>(executable),
                                     capacity),
                static_cast<uint8_t*>(writable));

    if (not memory->selfTest()) {
      if (report) {
        fprintf(stderr,
                "avian: dual-mapped code memory does not behave as "
                "expected on this system\n");
      }
      memory->dispose();
      return 0;
    }

    return memory;
  }

  virtual const char* name()
  {
    return "dual-map";
  }

  virtual uint8_t* stagingBuffer(uint8_t* address, size_t)
  {
    return writableAlias(address);
  }

  virtual void commit(uint8_t* address, const void* src, size_t size)
  {
    uint8_t* alias = writableAlias(address);
    if (src != alias) {
      memcpy(alias, src, size);
    }
    flushInstructionCache(address, size);
  }

  virtual void patch(void* address, const void* src, unsigned size)
  {
    storeCode(writableAlias(address), src, size);
    flushInstructionCache(address, size);
  }

  virtual void dispose()
  {
    munmap(region().begin(), region().count);
    munmap(region().begin() + delta, region().count);
    util::Alloc* a = allocator;
    this->~DualMapCodeMemory();
    a->free(this, sizeof(*this));
  }

 private:
  uint8_t* writableAlias(void* address)
  {
    return static_cast<uint8_t*>(address) + delta;
  }

  // Checks, before any real code goes in, that stores through the alias
  // show up in the executable view and that code committed and patched
  // there actually runs (emulators such as Rosetta may not track writes
  // through a second mapping).  Uses the start of the region and leaves
  // it zeroed.
  bool selfTest()
  {
    uint8_t* code = region().begin();
    bool ok;

#if defined(__x86_64__) || defined(__i386__)
    // mov eax, imm32; ret
    uint8_t stub[] = {0xB8, 1, 0, 0, 0, 0xC3};
    commit(code, stub, sizeof(stub));
    ok = call(code) == 1;

    uint32_t two = 2;
    patch(code + 1, &two, 4);
    ok = ok and call(code) == 2;

    memset(stub, 0, sizeof(stub));
    commit(code, stub, sizeof(stub));
#elif defined(__aarch64__)
    // movz w0, #imm; ret
    uint32_t stub[] = {0x52800000 | (1 << 5), 0xD65F03C0};
    commit(code, stub, sizeof(stub));
    ok = call(code) == 1;

    uint32_t two = 0x52800000 | (2 << 5);
    patch(code, &two, 4);
    ok = ok and call(code) == 2;

    memset(stub, 0, sizeof(stub));
    commit(code, stub, sizeof(stub));
#else
    // Can't easily run code here; at least check the views are shared.
    uint32_t marker = 0x41564941;
    commit(code, &marker, 4);
    ok = memcmp(code, &marker, 4) == 0;
    marker = 0;
    commit(code, &marker, 4);
#endif

    return ok;
  }

  static int call(uint8_t* code)
  {
    int (*function)();
    void* p = code;
    memcpy(&function, &p, sizeof(function));
    return function();
  }

  util::Alloc* allocator;
  ptrdiff_t delta;
};

#endif  // __linux__

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
#elif defined(__linux__)
  if (requested and strcmp(requested, "dual-map") == 0) {
    return DualMapCodeMemory::make(allocator, capacity, true);
  }

  if (requested == 0) {
    // Prefer W^X; quietly fall back where memfds can't be executable
    // (old kernels, vm.memfd_noexec=2, restrictive LSM policies, ...).
    CodeMemory* memory = DualMapCodeMemory::make(allocator, capacity, false);
    return memory ? memory : RwxCodeMemory::make(allocator, capacity);
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
