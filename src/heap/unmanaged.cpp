#include "avian/unmanaged.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/mman.h>

#include <new>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace vm {

#if !defined(__x86_64__)
uintptr_t unmanagedRegionBase = 0;
uintptr_t unmanagedRegionSpan = 0;
#endif

namespace {

#if defined(__x86_64__)
const uintptr_t RegionBase = UnmanagedTagIndex << UnmanagedTagShift;
const uintptr_t RegionSpan = uintptr_t(1) << UnmanagedTagShift;
#else
const uintptr_t RegionSpan = 256u * 1024u * 1024u;
#endif

uintptr_t regionBase = 0;
uintptr_t regionBump = 0;
uintptr_t regionEnd = 0;
pthread_once_t once = PTHREAD_ONCE_INIT;
pthread_mutex_t bumpLock = PTHREAD_MUTEX_INITIALIZER;

#if !defined(__linux__) || !defined(__x86_64__)
void* bumpAllocate(size_t bytes)
{
  const uintptr_t align = 16;
  pthread_mutex_lock(&bumpLock);
  uintptr_t p = (regionBump + align - 1) & ~(align - 1);
  uintptr_t end = (p + bytes + 4095) & ~uintptr_t(4095);
  if (end > regionEnd) {
    pthread_mutex_unlock(&bumpLock);
    return 0;
  }
  regionBump = end;
  pthread_mutex_unlock(&bumpLock);
  if (mprotect(reinterpret_cast<void*>(p), end - p, PROT_READ | PROT_WRITE)
      != 0) {
    return 0;
  }
  memset(reinterpret_cast<void*>(p), 0, bytes);
  return reinterpret_cast<void*>(p);
}
#endif

#if defined(__linux__) && defined(__x86_64__)

struct ExtentHooks;

typedef void* (*ExtentAllocFn)(ExtentHooks*,
                               void*,
                               size_t,
                               size_t,
                               bool*,
                               bool*,
                               unsigned);
typedef bool (*ExtentDallocFn)(ExtentHooks*, void*, size_t, bool, unsigned);
typedef void (*ExtentDestroyFn)(ExtentHooks*, void*, size_t, bool, unsigned);
typedef bool (
    *ExtentCommitFn)(ExtentHooks*, void*, size_t, size_t, size_t, unsigned);
typedef bool (*ExtentSplitFn)(
    ExtentHooks*, void*, size_t, size_t, size_t, bool, unsigned);
typedef bool (
    *ExtentMergeFn)(ExtentHooks*, void*, size_t, void*, size_t, bool, unsigned);

struct ExtentHooks {
  ExtentAllocFn alloc;
  ExtentDallocFn dalloc;
  ExtentDestroyFn destroy;
  ExtentCommitFn commit;
  ExtentCommitFn decommit;
  ExtentCommitFn purgeLazy;
  ExtentCommitFn purgeForced;
  ExtentSplitFn split;
  ExtentMergeFn merge;
};

extern "C" int mallctl(const char*, void*, size_t*, void*, size_t);
extern "C" void* mallocx(size_t, int);
extern "C" void dallocx(void*, int);
extern "C" void* rallocx(void*, size_t, int);

const int MallocxZero = 0x40;
// MALLOCX_TCACHE(-1). Required whenever the requested arena is not the
// thread's current arena, or a small alloc comes back from the tcache.
const int MallocxTcacheNone = 0x100;
// Low bits of a mallocx flag word are lg(alignment). 16-byte alignment
// is 4. MALLOCX_ARENA occupies bits from 20 upward.
const int MallocxAlign16 = 4;
#define MALLOCX_ARENA(a) ((((int)(a)) + 1) << 20)
// Arena 0 is created by jemalloc itself and has no extent hook.
const int DefaultArena = 0;

unsigned arenaIndex = 0;
__thread int threadArenaReady = 0;
__thread int ensureDepth = 0;
int heapLogged = 0;

void die(const char* msg)
{
  size_t n = 0;
  while (msg[n] != 0) {
    ++n;
  }
  ssize_t wrote = ::write(2, msg, n);
  (void)wrote;
  ::abort();
}

void* extentAlloc(ExtentHooks*,
                  void* newAddr,
                  size_t size,
                  size_t alignment,
                  bool* zero,
                  bool* commit,
                  unsigned)
{
  pthread_mutex_lock(&bumpLock);
  uintptr_t p;
  if (newAddr != 0) {
    p = reinterpret_cast<uintptr_t>(newAddr);
    if (p < regionBase or p + size > regionEnd) {
      pthread_mutex_unlock(&bumpLock);
      return 0;
    }
    if (p + size > regionBump) {
      regionBump = p + size;
    }
  } else {
    uintptr_t align = alignment == 0 ? 4096 : alignment;
    p = (regionBump + align - 1) & ~(align - 1);
    if (p + size > regionEnd) {
      pthread_mutex_unlock(&bumpLock);
      return 0;
    }
    regionBump = p + size;
  }
  pthread_mutex_unlock(&bumpLock);

  if ((p & 4095) != 0 or p + size < p) {
    return 0;
  }
  size_t protect = (size + 4095) & ~size_t(4095);
  if (p + protect > regionEnd) {
    return 0;
  }
  if (mprotect(reinterpret_cast<void*>(p), protect, PROT_READ | PROT_WRITE)
      != 0) {
    return 0;
  }
  if (*zero) {
    memset(reinterpret_cast<void*>(p), 0, size);
  }
  *commit = true;
  return reinterpret_cast<void*>(p);
}

bool extentRefuse(ExtentHooks*, void*, size_t, bool, unsigned)
{
  return true;
}

void extentDestroy(ExtentHooks*, void*, size_t, bool, unsigned) {}

bool extentRefuseRange(ExtentHooks*, void*, size_t, size_t, size_t, unsigned)
{
  return true;
}

bool extentAllowSplit(
    ExtentHooks*, void*, size_t, size_t, size_t, bool, unsigned)
{
  return false;
}

bool extentAllowMerge(
    ExtentHooks*, void*, size_t, void*, size_t, bool, unsigned)
{
  return false;
}

ExtentHooks hooks = {extentAlloc,
                     extentRefuse,
                     extentDestroy,
                     extentRefuseRange,
                     extentRefuseRange,
                     extentRefuseRange,
                     extentRefuseRange,
                     extentAllowSplit,
                     extentAllowMerge};

void initOnce()
{
  void* reserved = mmap(reinterpret_cast<void*>(RegionBase),
                        RegionSpan,
                        PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
                            | MAP_FIXED_NOREPLACE,
                        -1,
                        0);
  if (reserved == MAP_FAILED) {
    die("[avian] unmanaged heap reserve failed\n");
  }
  regionBase = RegionBase;
  regionBump = RegionBase;
  regionEnd = RegionBase + RegionSpan;

  ExtentHooks* hp = &hooks;
  size_t sz = sizeof(arenaIndex);
  int rc = mallctl("arenas.create", &arenaIndex, &sz, &hp, sizeof(hp));
  if (rc != 0) {
    die("[avian] unmanaged jemalloc arena failed\n");
  }
}

// The automatic tcache belongs to the thread's current arena. mallocx
// with MALLOCX_ARENA and a different arena can hand back a cached block
// from outside the granule. Pin this thread to the tagged arena first.
void ensureArena()
{
  // fprintf below may call malloc. A nested call from mallctl or from
  // initOnce would deadlock inside pthread_once, so fail instead.
  if (ensureDepth != 0) {
    die("[avian] unmanaged allocator reentered\n");
  }
  ++ensureDepth;
  pthread_once(&once, initOnce);
  if (not threadArenaReady) {
    // mallocx(MALLOCX_ARENA) still pops the automatic tcache, and that
    // cache holds blocks from whatever arena this thread used before the
    // pin. Those addresses are outside the granule, so a later collection
    // treats the object as managed and overwrites its class word.
    mallctl("thread.tcache.flush", 0, 0, 0, 0);
    if (mallctl("thread.arena", 0, 0, &arenaIndex, sizeof(arenaIndex)) != 0) {
      die("[avian] unmanaged thread arena failed\n");
    }
    mallctl("thread.tcache.flush", 0, 0, 0, 0);
    threadArenaReady = 1;
  }
  --ensureDepth;
  if (not heapLogged) {
    heapLogged = 1;
    fprintf(stderr,
            "[avian] unmanaged heap base=0x%llx span=%llu tag_bit=45 "
            "(1 = unmanaged)\n",
            static_cast<unsigned long long>(regionBase),
            static_cast<unsigned long long>(RegionSpan));
  }
}

int taggedFlags(int lgAlign)
{
  // MALLOCX_TCACHE_NONE: an explicit arena must not use the automatic
  // cache. Jemalloc 5.2 encodes that as MALLOCX_TCACHE(-1) == 0x100.
  return (lgAlign & 63) | MALLOCX_ARENA(arenaIndex) | MallocxTcacheNone;
}

void* arenaAllocate(size_t bytes)
{
  ensureArena();
  return mallocx(bytes, taggedFlags(MallocxAlign16) | MallocxZero);
}

#elif defined(__x86_64__)

void initOnce()
{
  void* reserved = mmap(reinterpret_cast<void*>(RegionBase),
                        RegionSpan,
                        PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
                            | MAP_FIXED_NOREPLACE,
                        -1,
                        0);
  if (reserved == MAP_FAILED) {
    fprintf(stderr, "[avian] unmanaged heap reserve failed\n");
    ::abort();
  }
  regionBase = RegionBase;
  regionBump = RegionBase;
  regionEnd = RegionBase + RegionSpan;
  fprintf(stderr,
          "[avian] unmanaged heap base=0x%llx span=%llu tag_bit=45 "
          "(1 = unmanaged)\n",
          static_cast<unsigned long long>(regionBase),
          static_cast<unsigned long long>(RegionSpan));
}

void* arenaAllocate(size_t bytes)
{
  pthread_once(&once, initOnce);
  return bumpAllocate(bytes);
}

#else

void initOnce()
{
  void* reserved = mmap(0,
                        RegionSpan,
                        PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                        -1,
                        0);
  if (reserved == MAP_FAILED) {
    fprintf(stderr, "[avian] unmanaged heap reserve failed\n");
    ::abort();
  }
  regionBase = reinterpret_cast<uintptr_t>(reserved);
  regionBump = regionBase;
  regionEnd = regionBase + RegionSpan;
  unmanagedRegionBase = regionBase;
  unmanagedRegionSpan = RegionSpan;
  fprintf(stderr,
          "[avian] unmanaged heap base=0x%llx span=%llu (range check)\n",
          static_cast<unsigned long long>(regionBase),
          static_cast<unsigned long long>(RegionSpan));
}

void* arenaAllocate(size_t bytes)
{
  pthread_once(&once, initOnce);
  return bumpAllocate(bytes);
}

#endif

struct Tracked {
  Tracked* next;
  void* object;
};

Tracked* trackedHead = 0;

}  // namespace

#if defined(__linux__) && defined(__x86_64__)

void* heapMalloc(size_t bytes)
{
  if (bytes == 0) {
    bytes = 1;
  }
  ensureArena();
  return mallocx(bytes, taggedFlags(MallocxAlign16));
}

void heapFree(void* p)
{
  if (p == 0) {
    return;
  }
  ensureArena();
  dallocx(p, MALLOCX_ARENA(arenaIndex));
}

void* heapRealloc(void* p, size_t bytes)
{
  if (p == 0) {
    return heapMalloc(bytes);
  }
  if (bytes == 0) {
    heapFree(p);
    return 0;
  }
  ensureArena();
  return rallocx(p, bytes, taggedFlags(MallocxAlign16));
}

void* heapCalloc(size_t count, size_t size)
{
  if (count != 0 and size > (static_cast<size_t>(-1) / count)) {
    return 0;
  }
  size_t bytes = count * size;
  if (bytes == 0) {
    bytes = 1;
  }
  ensureArena();
  return mallocx(bytes, taggedFlags(MallocxAlign16) | MallocxZero);
}

void* heapMemalign(size_t alignment, size_t bytes)
{
  if (alignment < sizeof(void*) or (alignment & (alignment - 1)) != 0) {
    return 0;
  }
  unsigned lg = 0;
  while ((static_cast<size_t>(1) << lg) < alignment) {
    ++lg;
  }
  if (bytes == 0) {
    bytes = 1;
  }
  ensureArena();
  return mallocx(bytes, taggedFlags(static_cast<int>(lg)));
}

void* copyingHeapAllocate(size_t bytes)
{
  if (bytes == 0) {
    bytes = 1;
  }
  // Reserve the granule before arena 0 maps, so a normal chunk cannot
  // land on the tag. TCACHE_NONE keeps a pinned thread from reusing a
  // tagged cached block.
  ensureArena();
  return mallocx(bytes, MALLOCX_ARENA(DefaultArena) | MallocxTcacheNone);
}

void copyingHeapFree(const void* p)
{
  if (p == 0) {
    return;
  }
  ensureArena();
  dallocx(const_cast<void*>(p), MALLOCX_ARENA(DefaultArena) | MallocxTcacheNone);
}

#else

void* copyingHeapAllocate(size_t bytes)
{
  return malloc(bytes);
}

void copyingHeapFree(const void* p)
{
  free(const_cast<void*>(p));
}

#endif

void* unmanagedAllocate(size_t bytes)
{
  if (bytes == 0) {
    return 0;
  }
  void* p = arenaAllocate(bytes);
  if (p != 0 and not pointerIsUnmanaged(p)) {
    fprintf(stderr, "[avian] unmanaged alloc outside granule %p\n", p);
    fflush(stderr);
    ::abort();
  }
  if (p != 0) {
    memset(p, 0, bytes);
  }
  return p;
}

void unmanagedTrack(void* object)
{
  Tracked* node = static_cast<Tracked*>(malloc(sizeof(Tracked)));
  if (node == 0) {
    fprintf(stderr, "[avian] unmanaged track\n");
    ::abort();
  }
  node->object = object;
  Tracked* old = __atomic_load_n(&trackedHead, __ATOMIC_ACQUIRE);
  do {
    node->next = old;
  } while (not __atomic_compare_exchange_n(&trackedHead,
                                           &old,
                                           node,
                                           true,
                                           __ATOMIC_RELEASE,
                                           __ATOMIC_ACQUIRE));
}

void unmanagedForEach(void (*fn)(void* object, void* arg), void* arg)
{
  for (Tracked* node = __atomic_load_n(&trackedHead, __ATOMIC_ACQUIRE);
       node != 0;
       node = node->next) {
    fn(node->object, arg);
  }
}

}  // namespace vm

#if defined(__linux__) && defined(__x86_64__)

// Default visibility so a program that links libjvm, and this executable,
// resolve malloc here instead of glibc. -Bsymbolic-functions on libjvm
// keeps the VM's own calls on this path when a host dlopens libjvm after
// glibc has already claimed the process malloc.
#define AVIAN_ALLOC_EXPORT __attribute__((visibility("default")))

extern "C" AVIAN_ALLOC_EXPORT void* malloc(size_t bytes)
{
  return vm::heapMalloc(bytes);
}

extern "C" AVIAN_ALLOC_EXPORT void free(void* p)
{
  vm::heapFree(p);
}

extern "C" AVIAN_ALLOC_EXPORT void* realloc(void* p, size_t bytes)
{
  return vm::heapRealloc(p, bytes);
}

extern "C" AVIAN_ALLOC_EXPORT void* calloc(size_t count, size_t size)
{
  return vm::heapCalloc(count, size);
}

extern "C" AVIAN_ALLOC_EXPORT int posix_memalign(void** p,
                                                size_t alignment,
                                                size_t bytes)
{
  if (p == 0 or alignment < sizeof(void*)
      or (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  void* block = vm::heapMemalign(alignment, bytes);
  if (block == 0) {
    return ENOMEM;
  }
  *p = block;
  return 0;
}

extern "C" AVIAN_ALLOC_EXPORT void* memalign(size_t alignment, size_t bytes)
{
  return vm::heapMemalign(alignment, bytes);
}

extern "C" AVIAN_ALLOC_EXPORT void* aligned_alloc(size_t alignment,
                                                 size_t bytes)
{
  if (alignment == 0 or (alignment & (alignment - 1)) != 0
      or (bytes % alignment) != 0) {
    return 0;
  }
  return vm::heapMemalign(alignment, bytes);
}

AVIAN_ALLOC_EXPORT void* operator new(size_t bytes)
{
  void* p = vm::heapMalloc(bytes);
  if (p == 0) {
    ::abort();
  }
  return p;
}

AVIAN_ALLOC_EXPORT void* operator new[](size_t bytes)
{
  return operator new(bytes);
}

AVIAN_ALLOC_EXPORT void* operator new(size_t bytes,
                                      const std::nothrow_t&) noexcept
{
  return vm::heapMalloc(bytes);
}

AVIAN_ALLOC_EXPORT void* operator new[](size_t bytes,
                                       const std::nothrow_t&) noexcept
{
  return vm::heapMalloc(bytes);
}

AVIAN_ALLOC_EXPORT void operator delete(void* p) noexcept
{
  vm::heapFree(p);
}

AVIAN_ALLOC_EXPORT void operator delete[](void* p) noexcept
{
  vm::heapFree(p);
}

AVIAN_ALLOC_EXPORT void operator delete(void* p, size_t) noexcept
{
  vm::heapFree(p);
}

AVIAN_ALLOC_EXPORT void operator delete[](void* p, size_t) noexcept
{
  vm::heapFree(p);
}

AVIAN_ALLOC_EXPORT void operator delete(void* p,
                                       const std::nothrow_t&) noexcept
{
  vm::heapFree(p);
}

AVIAN_ALLOC_EXPORT void operator delete[](void* p,
                                        const std::nothrow_t&) noexcept
{
  vm::heapFree(p);
}

#endif
