/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include <avian/system/memory.h>

#include <avian/util/assert.h>

#include <cerrno>
#include <cstring>
#include <cstdio>

#ifdef AVIAN_DARWIN
#include <pthread.h>
#endif

#include "sys/mman.h"

namespace avian {
namespace system {

#if defined(AVIAN_DARWIN) && defined(MAP_JIT)
namespace {
thread_local unsigned JitWriteDepth = 0;
}
#endif

const size_t Memory::PageSize = 1 << 12;

util::Slice<uint8_t> Memory::allocate(size_t sizeInBytes, Permissions perms)
{
  unsigned prot = 0;
  if (perms & Read) {
    prot |= PROT_READ;
  }
  if (perms & Write) {
    prot |= PROT_WRITE;
  }
  if (perms & Execute) {
    prot |= PROT_EXEC;
  }
#if defined(MAP_32BIT) && !defined(AVIAN_DARWIN)
  // map to the lower 32 bits of memory when possible so as to avoid
  // expensive relative jumps
  // not support in macos, may cause ENOMEM
  const unsigned Extra = MAP_32BIT;
#else
  const unsigned Extra = 0;
#endif

  unsigned flags = MAP_PRIVATE | MAP_ANON | Extra;

#if defined(AVIAN_DARWIN) && defined(MAP_JIT)
  if (perms & Execute) {
    flags |= MAP_JIT;
  }
#endif

  void* p = mmap(0, sizeInBytes, prot, flags, -1, 0);

  if (p == MAP_FAILED) {
    int err = errno;
    const char* reason = std::strerror(err);
    std::fprintf(
      stderr,
        "[Memory::allocate] mmap failed: errno=%d (%s), size=%zu, perms=0x%x, prot=0x%x, flags=0x%x\n",
        err,
        reason ? reason : "unknown",
        sizeInBytes,
        static_cast<unsigned>(perms),
        prot,
        flags);
#if defined(AVIAN_DARWIN) && defined(MAP_JIT)
    if (flags & MAP_JIT) {
      std::fprintf(
        stderr,
          "[Memory::allocate] Darwin MAP_JIT path active. Ensure the executable is codesigned with entitlement com.apple.security.cs.allow-jit.\n");
    }
#endif
    std::fflush(stderr);
    // Report failure to the caller instead of exiting; e.g. compile.cpp
    // expect()s a non-null code area and aborts through the VM's normal
    // error path.
    return util::Slice<uint8_t>(0, 0);
  }
  else {
    return util::Slice<uint8_t>(static_cast<uint8_t*>(p), sizeInBytes);
  }
}

void Memory::free(util::Slice<uint8_t> pages)
{
  munmap(const_cast<uint8_t*>(pages.begin()), pages.count);
}

void Memory::beginJitWrite()
{
#if defined(AVIAN_DARWIN) && defined(MAP_JIT)
  if (pthread_jit_write_protect_supported_np()) {
    if (JitWriteDepth++ == 0) {
      pthread_jit_write_protect_np(0);
    }
  }
#endif
}

void Memory::endJitWrite()
{
#if defined(AVIAN_DARWIN) && defined(MAP_JIT)
  if (pthread_jit_write_protect_supported_np()) {
    if (JitWriteDepth == 0) {
      std::fprintf(stderr,
                   "[Memory::endJitWrite] unbalanced scope: end called without matching begin\n");
      std::fflush(stderr);
      return;
    }

    if (--JitWriteDepth == 0) {
      pthread_jit_write_protect_np(1);
    }
  }
#endif
}

}  // namespace system
}  // namespace avian
