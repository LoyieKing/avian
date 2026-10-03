#ifndef AVIAN_UNMANAGED_H_
#define AVIAN_UNMANAGED_H_

#include <stdint.h>
#include <stddef.h>

namespace vm {

// x86-64 user space is 48-bit. The unmanaged heap reserves one 4TB
// granule at bit 45 (address >> 42 == 8). That bit is the tag: set
// means unmanaged. Process malloc and ordinary C++ new land there.
// Copying-heap segments stay on jemalloc's normal arena, outside this
// granule, so a managed object does not carry the tag. The two areas
// are different physical pages, not a ZGC multi-map. Null is granule
// 0, so it is not unmanaged. A clear bit is not by itself a managed
// object; the copying heap still checks its segments. 32-bit builds
// have no spare bit and compare against the reserved span.
static const unsigned UnmanagedTagShift = 42;
static const uintptr_t UnmanagedTagIndex = 8;

inline bool pointerIsUnmanaged(const void* p)
{
  uintptr_t u = reinterpret_cast<uintptr_t>(p);
#if defined(__x86_64__)
  return (u >> UnmanagedTagShift) == UnmanagedTagIndex;
#else
  extern uintptr_t unmanagedRegionBase;
  extern uintptr_t unmanagedRegionSpan;
  return unmanagedRegionSpan != 0
         and u - unmanagedRegionBase < unmanagedRegionSpan;
#endif
}

// Zero-filled memory from the unmanaged arena. Null on exhaustion.
// The address stays inside the reserved granule so a later pin of that
// granule does not change callers.
void* unmanagedAllocate(size_t bytes);

// Backing store for copying-heap segments and fixed objects. This is
// not the tagged granule: managed objects must fail pointerIsUnmanaged.
void* copyingHeapAllocate(size_t bytes);
void copyingHeapFree(const void* p);

// The object is never freed. Collections walk it and update managed
// slots in place; the object itself does not move.
void unmanagedTrack(void* object);

// allocateUnmanaged places two words immediately before the Java object.
// The first is the tracker node. The second is the scan flag. Zero, the
// allocator's fill, means the object is on the dirty list and the next
// collection must scan it. A scan that sees only null and unmanaged
// slots stores 1 and unlinks the node. The node is never freed. A later
// store of a managed referent stores 0 and links that same node again.
// Neither word is a Java field.
inline void** unmanagedNodeWord(void* object)
{
  return reinterpret_cast<void**>(object) - 2;
}

inline uintptr_t* unmanagedScanWord(void* object)
{
  return reinterpret_cast<uintptr_t*>(object) - 1;
}

inline bool unmanagedScanClean(void* object)
{
  return __atomic_load_n(unmanagedScanWord(object), __ATOMIC_RELAXED) != 0;
}

inline void unmanagedMarkScanClean(void* object)
{
  __atomic_store_n(unmanagedScanWord(object), static_cast<uintptr_t>(1), __ATOMIC_RELAXED);
}

// Already-dirty is one relaxed load. A clean object is linked again
// under the stop-the-world collection that walks the list.
void unmanagedMarkScanDirty(void* object);

void unmanagedForEach(void (*fn)(void* object, void* arg), void* arg);

}  // namespace vm

#endif  // AVIAN_UNMANAGED_H_
