/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

// The platform-independent part of CodeMemory: allocation, the atomic
// code store shared by all backends, and the boot image backend.  The
// executable backends live in src/system/<os>/code-memory.cpp.

#include "code-memory-backend.h"

#include <string.h>

#include "avian/arch.h"

namespace avian {
namespace system {

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

namespace code_memory {

void storeCode(void* dst, const void* src, unsigned size)
{
  uintptr_t address = reinterpret_cast<uintptr_t>(dst);

  if (size == 4 and address % 4 == 0) {
    uint32_t v;
    memcpy(&v, src, 4);
    vm::storeStoreMemoryBarrier();
    *static_cast<volatile uint32_t*>(dst) = v;
  } else if (sizeof(void*) == 8 and size == 8 and address % 8 == 0) {
    uint64_t v;
    memcpy(&v, src, 8);
    vm::storeStoreMemoryBarrier();
    *static_cast<volatile uint64_t*>(dst) = v;
  } else {
    memcpy(dst, src, size);
  }
}

}  // namespace code_memory

namespace {

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
    code_memory::destroy(this, allocator);
  }

 private:
  util::Alloc* allocator;
};

}  // namespace

CodeMemory* makeImageCodeMemory(util::Alloc* allocator,
                                util::Slice<uint8_t> image)
{
  return code_memory::construct<ImageCodeMemory>(allocator, image);
}

}  // namespace system
}  // namespace avian
