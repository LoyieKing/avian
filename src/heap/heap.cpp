/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include <avian/heap/heap.h>
#include <avian/system/system.h>
#include "avian/common.h"
#include "avian/unmanaged.h"
#include "avian/arch.h"

#include <avian/util/math.h>

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace vm;
using namespace avian::util;

namespace {

namespace local {

const unsigned Top = ~static_cast<unsigned>(0);

const unsigned InitialGen2CapacityInBytes = 4 * 1024 * 1024;
const unsigned InitialTenuredFixieCeilingInBytes = 4 * 1024 * 1024;

const bool Verbose = false;
const bool Verbose2 = false;
const bool Debug = false;
const bool DebugFixies = false;

#ifdef NDEBUG
const bool DebugAllocation = false;
#else
const bool DebugAllocation = true;
#endif

#define ACQUIRE(x) MutexLock MAKE_NAME(monitorLock_)(x)

class MutexLock {
 public:
  MutexLock(System::Mutex* m) : m(m)
  {
    m->acquire();
  }

  ~MutexLock()
  {
    m->release();
  }

 private:
  System::Mutex* m;
};

class Context;

Aborter* getAborter(Context* c);

void* tryAllocate(Context* c, size_t size);
void* allocate(Context* c, size_t size);
void* allocate(Context* c, size_t size, bool limit);
void free(Context* c, const void* p, size_t size);

#ifdef USE_ATOMIC_OPERATIONS
inline void markBitAtomic(uintptr_t* map, unsigned i)
{
  uintptr_t* p = map + wordOf(i);
  uintptr_t v = static_cast<uintptr_t>(1) << bitOf(i);
  for (uintptr_t old = *p; not atomicCompareAndSwap(p, old, old | v);
       old = *p) {
  }
}
#endif  // USE_ATOMIC_OPERATIONS

inline void* get(void* o, unsigned offsetInWords)
{
  return maskAlignedPointer(
      fieldAtOffset<void*>(o, offsetInWords * BytesPerWord));
}

inline void** getp(void* o, unsigned offsetInWords)
{
  return &fieldAtOffset<void*>(o, offsetInWords * BytesPerWord);
}

inline void set(void** o, void* value)
{
  *o = reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(value)
      | (reinterpret_cast<uintptr_t>(*o) & (~PointerMask)));
}

inline void set(void* o, unsigned offsetInWords, void* value)
{
  set(getp(o, offsetInWords), value);
}

// Low bits of an object header. Must match vm::FixedMark.
const uintptr_t HeapFixedMark = 3;

inline bool fixedHeader(void* o)
{
  return (reinterpret_cast<uintptr_t*>(o)[0] & ~PointerMask) == HeapFixedMark;
}

inline uintptr_t headerLoad(void* o)
{
  return __atomic_load_n(reinterpret_cast<uintptr_t*>(o), __ATOMIC_ACQUIRE);
}

inline void headerStoreForward(void* o, void* dst)
{
  __atomic_store_n(reinterpret_cast<uintptr_t*>(o),
                   reinterpret_cast<uintptr_t>(dst),
                   __ATOMIC_RELEASE);
}

inline void* headerPointer(void* o)
{
  return maskAlignedPointer(reinterpret_cast<void*>(headerLoad(o)));
}

inline void atomicSetRecord(uintptr_t* map,
                            unsigned bitsPerRecord,
                            unsigned index,
                            unsigned v)
{
  for (int i = static_cast<int>(index + bitsPerRecord) - 1;
       i >= static_cast<int>(index);
       --i) {
    uintptr_t bit = static_cast<uintptr_t>(1) << bitOf(static_cast<unsigned>(i));
    uintptr_t* word = map + wordOf(static_cast<unsigned>(i));
    uintptr_t old = __atomic_load_n(word, __ATOMIC_RELAXED);
    for (;;) {
      uintptr_t next = (v & 1) ? (old | bit) : (old & ~bit);
      if (__atomic_compare_exchange_n(
              word, &old, next, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        break;
      }
    }
    v >>= 1;
  }
}

class Segment {
 public:
  class Map {
   public:
    class Iterator {
     public:
      Map* map;
      unsigned index;
      unsigned limit;

      Iterator(Map* map, unsigned start, unsigned end) : map(map)
      {
        assertT(map->segment->context, map->bitsPerRecord == 1);
        assertT(map->segment->context, map->segment);
        assertT(map->segment->context, start <= map->segment->position());

        if (end > map->segment->position())
          end = map->segment->position();

        index = map->indexOf(start);
        limit = map->indexOf(end);

        if ((end - start) % map->scale)
          ++limit;
      }

      bool hasMore()
      {
        unsigned word = wordOf(index);
        unsigned bit = bitOf(index);
        unsigned wordLimit = wordOf(limit);
        unsigned bitLimit = bitOf(limit);

        for (; word <= wordLimit and (word < wordLimit or bit < bitLimit);
             ++word) {
          uintptr_t w = map->data[word];
          if (2) {
            for (; bit < BitsPerWord and (word < wordLimit or bit < bitLimit);
                 ++bit) {
              if (w & (static_cast<uintptr_t>(1) << bit)) {
                index = ::indexOf(word, bit);
                //                 printf("hit at index %d\n", index);
                return true;
              } else {
                //                 printf("miss at index %d\n", indexOf(word,
                //                 bit));
              }
            }
          }
          bit = 0;
        }

        index = limit;

        return false;
      }

      unsigned next()
      {
        assertT(map->segment->context, hasMore());
        assertT(map->segment->context, map->segment);

        return (index++) * map->scale;
      }
    };

    Segment* segment;
    Map* child;
    uintptr_t* data;
    unsigned bitsPerRecord;
    unsigned scale;
    bool clearNewData;

    Map(Segment* segment,
        uintptr_t* data,
        unsigned bitsPerRecord,
        unsigned scale,
        Map* child,
        bool clearNewData)
        : segment(segment),
          child(child),
          data(data),
          bitsPerRecord(bitsPerRecord),
          scale(scale),
          clearNewData(clearNewData)
    {
    }

    Map(Segment* segment,
        unsigned bitsPerRecord,
        unsigned scale,
        Map* child,
        bool clearNewData)
        : segment(segment),
          child(child),
          data(0),
          bitsPerRecord(bitsPerRecord),
          scale(scale),
          clearNewData(clearNewData)
    {
    }

    void init()
    {
      assertT(segment->context, bitsPerRecord);
      assertT(segment->context, scale);
      assertT(segment->context, powerOfTwo(scale));

      if (data == 0) {
        data = segment->data + segment->capacity()
               + calculateOffset(segment->capacity());
      }

      if (clearNewData) {
        memset(data, 0, size() * BytesPerWord);
      }

      if (child) {
        child->init();
      }
    }

    unsigned calculateOffset(unsigned capacity)
    {
      unsigned n = 0;
      if (child)
        n += child->calculateFootprint(capacity);
      return n;
    }

    static unsigned calculateSize(Context* c UNUSED,
                                  unsigned capacity,
                                  unsigned scale,
                                  unsigned bitsPerRecord)
    {
      unsigned result = ceilingDivide(
          ceilingDivide(capacity, scale) * bitsPerRecord, BitsPerWord);
      assertT(c, result);
      return result;
    }

    unsigned calculateSize(unsigned capacity)
    {
      return calculateSize(segment->context, capacity, scale, bitsPerRecord);
    }

    unsigned size()
    {
      return calculateSize(segment->capacity());
    }

    unsigned calculateFootprint(unsigned capacity)
    {
      unsigned n = calculateSize(capacity);
      if (child)
        n += child->calculateFootprint(capacity);
      return n;
    }

    void replaceWith(Map* m)
    {
      assertT(segment->context, bitsPerRecord == m->bitsPerRecord);
      assertT(segment->context, scale == m->scale);

      data = m->data;

      m->segment = 0;
      m->data = 0;

      if (child)
        child->replaceWith(m->child);
    }

    unsigned indexOf(unsigned segmentIndex)
    {
      return (segmentIndex / scale) * bitsPerRecord;
    }

    unsigned indexOf(void* p)
    {
      assertT(segment->context, segment->almostContains(p));
      assertT(segment->context, segment->capacity());
      return indexOf(segment->indexOf(p));
    }

    void clearBit(unsigned i)
    {
      assertT(segment->context, wordOf(i) < size());

      vm::clearBit(data, i);
    }

    void setBit(unsigned i)
    {
      assertT(segment->context, wordOf(i) < size());

      vm::markBit(data, i);
    }

    void clearOnlyIndex(unsigned index)
    {
      atomicSetRecord(data, bitsPerRecord, index, 0);
    }

    void clearOnly(unsigned segmentIndex)
    {
      clearOnlyIndex(indexOf(segmentIndex));
    }

    void clearOnly(void* p)
    {
      clearOnlyIndex(indexOf(p));
    }

    void clear(void* p)
    {
      clearOnly(p);
      if (child)
        child->clear(p);
    }

    void setOnlyIndex(unsigned index, unsigned v = 1)
    {
      atomicSetRecord(data, bitsPerRecord, index, v);
    }

    void setOnly(unsigned segmentIndex, unsigned v = 1)
    {
      setOnlyIndex(indexOf(segmentIndex), v);
    }

    void setOnly(void* p, unsigned v = 1)
    {
      setOnlyIndex(indexOf(p), v);
    }

    void set(void* p, unsigned v = 1)
    {
      setOnly(p, v);
      assertT(segment->context, get(p) == v);
      if (child)
        child->set(p, v);
    }

#ifdef USE_ATOMIC_OPERATIONS
    void markAtomic(void* p)
    {
      assertT(segment->context, bitsPerRecord == 1);
      markBitAtomic(data, indexOf(p));
      assertT(segment->context, getBit(data, indexOf(p)));
      if (child)
        child->markAtomic(p);
    }
#endif

    unsigned get(void* p)
    {
      return getBits(data, bitsPerRecord, indexOf(p));
    }
  };

  Context* context;
  uintptr_t* data;
  unsigned position_;
  unsigned capacity_;
  Map* map;

  Segment(Context* context,
          Map* map,
          unsigned desired,
          unsigned minimum,
          int64_t available = INT64_MAX)
      : context(context), data(0), position_(0), capacity_(0), map(map)
  {
    if (desired) {
      if (minimum == 0) {
        minimum = 1;
      }

      assertT(context, desired >= minimum);

      capacity_ = desired;

      if (static_cast<int64_t>(footprint(capacity_)) > available) {
        unsigned top = capacity_;
        unsigned bottom = minimum;
        unsigned target = available;
        while (true) {
          if (static_cast<int64_t>(footprint(capacity_)) > target) {
            if (bottom == capacity_) {
              break;
            } else if (static_cast<int64_t>(footprint(capacity_ - 1))
                       <= target) {
              --capacity_;
              break;
            }
            top = capacity_;
            capacity_ = avg(bottom, capacity_);
          } else if (static_cast<int64_t>(footprint(capacity_)) < target) {
            if (top == capacity_
                or static_cast<int64_t>(footprint(capacity_ + 1)) >= target) {
              break;
            }
            bottom = capacity_;
            capacity_ = avg(top, capacity_);
          } else {
            break;
          }
        }
      }

      while (data == 0) {
        data = static_cast<uintptr_t*>(local::allocate(
            context, byteFootprint(capacity_), false));

        if (data == 0) {
          if (capacity_ > minimum) {
            capacity_ = avg(minimum, capacity_);
            if (capacity_ == 0) {
              break;
            }
          } else {
            data = static_cast<uintptr_t*>(local::allocate(
                context, byteFootprint(capacity_)));
          }
        }
      }

      if (map) {
        map->init();
      }
    }
  }

  Segment(Context* context,
          Map* map,
          uintptr_t* data,
          unsigned position,
          unsigned capacity)
      : context(context),
        data(data),
        position_(position),
        capacity_(capacity),
        map(map)
  {
    if (map) {
      map->init();
    }
  }

  unsigned footprint(unsigned capacity)
  {
    return capacity
           + (map and capacity ? map->calculateFootprint(capacity) : 0);
  }

  size_t byteFootprint(unsigned capacity)
  {
    return static_cast<size_t>(footprint(capacity)) * BytesPerWord;
  }

  unsigned capacity()
  {
    return capacity_;
  }

  unsigned position()
  {
    return __atomic_load_n(&position_, __ATOMIC_ACQUIRE);
  }

  void setPosition(unsigned p)
  {
    __atomic_store_n(&position_, p, __ATOMIC_RELEASE);
  }

  unsigned remaining()
  {
    return capacity() - position();
  }

  void replaceWith(Segment* s)
  {
    if (data) {
      free(context, data, byteFootprint(capacity()));
    }
    data = s->data;
    s->data = 0;

    setPosition(s->position());
    s->setPosition(0);

    capacity_ = s->capacity_;
    s->capacity_ = 0;

    if (s->map) {
      if (map) {
        map->replaceWith(s->map);
        s->map = 0;
      } else {
        abort(context);
      }
    } else {
      assertT(context, map == 0);
    }
  }

  bool contains(void* p)
  {
    return position() and p >= data and p < data + position();
  }

  bool almostContains(void* p)
  {
    return contains(p) or p == data + position();
  }

  void* get(unsigned offset)
  {
    assertT(context, offset <= position());
    return data + offset;
  }

  unsigned indexOf(void* p)
  {
    assertT(context, almostContains(p));
    return static_cast<uintptr_t*>(p) - data;
  }

  void* allocate(unsigned size)
  {
    assertT(context, size);
    unsigned old = position();
    assertT(context, old + size <= capacity());

    setPosition(old + size);
    return data + old;
  }

  void* claim(unsigned size)
  {
    unsigned cap = capacity_;
    unsigned old = __atomic_load_n(&position_, __ATOMIC_RELAXED);
    for (;;) {
      if (old + size < old or old + size > cap) {
        return 0;
      }
      if (__atomic_compare_exchange_n(&position_,
                                      &old,
                                      old + size,
                                      true,
                                      __ATOMIC_ACQ_REL,
                                      __ATOMIC_RELAXED)) {
        return data + old;
      }
    }
  }

  void dispose()
  {
    if (data) {
      free(context, data, byteFootprint(capacity()));
    }
    data = 0;
    map = 0;
  }
};

class Fixie {
 public:
  static const unsigned HasMask = 1 << 0;
  static const unsigned Marked = 1 << 1;
  static const unsigned Dirty = 1 << 2;
  static const unsigned Dead = 1 << 3;

  Fixie(Context* c, unsigned size, bool hasMask, Fixie** handle)
      : age(0),
        flags(hasMask ? HasMask : 0),
        size(size),
        next(0),
        handle(0)
  {
    memset(mask(), 0, maskSize(size, hasMask));
    add(c, handle);
    if (DebugFixies) {
      fprintf(stderr, "make fixie %p of size %d\n", this, totalSize());
    }
  }

  // See ImmortalFixieAge: only boot image fixed objects are immortal.
  bool immortal()
  {
    return age == ImmortalFixieAge;
  }

  void add(Context* c UNUSED, Fixie** handle)
  {
    assertT(c, this->handle == 0);
    assertT(c, next == 0);

    this->handle = handle;
    if (handle) {
      next = *handle;
      if (next)
        next->handle = &next;
      *handle = this;
    } else {
      next = 0;
    }
  }

  void remove(Context* c UNUSED)
  {
    if (handle) {
      assertT(c, *handle == this);
      *handle = next;
    }
    if (next) {
      next->handle = handle;
    }
    next = 0;
    handle = 0;
  }

  void move(Context* c, Fixie** handle)
  {
    if (DebugFixies) {
      fprintf(stderr, "move fixie %p\n", this);
    }

    remove(c);
    add(c, handle);
  }

  void** body()
  {
    return static_cast<void**>(static_cast<void*>(body_));
  }

  uintptr_t* mask()
  {
    return body_ + size;
  }

  static unsigned maskSize(unsigned size, bool hasMask)
  {
    return hasMask * ceilingDivide(size, BitsPerWord) * BytesPerWord;
  }

  static unsigned totalSize(unsigned size, bool hasMask)
  {
    return sizeof(Fixie) + (size * BytesPerWord) + maskSize(size, hasMask);
  }

  unsigned totalSize()
  {
    return totalSize(size, hasMask());
  }

  bool hasMask()
  {
    return (flags & HasMask) != 0;
  }

  bool marked()
  {
    return (flags & Marked) != 0;
  }

  void marked(bool v)
  {
    if (v) {
      flags |= Marked;
    } else {
      flags &= ~Marked;
    }
  }

  bool dirty()
  {
    return (flags & Dirty) != 0;
  }

  void dirty(bool v)
  {
    if (v) {
      flags |= Dirty;
    } else {
      flags &= ~Dirty;
    }
  }

  bool dead()
  {
    return (flags & Dead) != 0;
  }

  void dead(bool v)
  {
    if (v) {
      flags |= Dead;
    } else {
      flags &= ~Dead;
    }
  }

  // be sure to update e.g. TargetFixieSizeInBytes in bootimage.cpp if
  // you add/remove/change fields in this class:

  uint16_t age;
  uint16_t flags;
  uint32_t size;
  Fixie* next;
  Fixie** handle;
  uintptr_t body_[0];
};

Fixie* fixie(void* body)
{
  return static_cast<Fixie*>(body) - 1;
}

void free(Context* c, Fixie** fixies, bool resetImmortal = false);

class Context {
 public:
  Context(System* system, uint64_t limit)
      : system(system),
        client(0),
        count(0),
        limit(limit),
        lock(0),
        immortalHeapStart(0),
        immortalHeapEnd(0),
        ageMap(&gen1, max(1, log(TenureThreshold)), 1, 0, false),
        gen1(this, &ageMap, 0, 0),
        nextAgeMap(&nextGen1, max(1, log(TenureThreshold)), 1, 0, false),
        nextGen1(this, &nextAgeMap, 0, 0),
        pointerMap(&gen2, 1, 1, 0, true),
        pageMap(&gen2,
                1,
                LikelyPageSizeInBytes / BytesPerWord,
                &pointerMap,
                true),
        heapMap(&gen2, 1, pageMap.scale * 1024, &pageMap, true),
        gen2(this, &heapMap, 0, 0),
        nextPointerMap(&nextGen2, 1, 1, 0, true),
        nextPageMap(&nextGen2,
                    1,
                    LikelyPageSizeInBytes / BytesPerWord,
                    &nextPointerMap,
                    true),
        nextHeapMap(&nextGen2, 1, nextPageMap.scale * 1024, &nextPageMap, true),
        nextGen2(this, &nextHeapMap, 0, 0),
        gen2Base(0),
        incomingFootprint(0),
        pendingAllocation(0),
        tenureFootprint(0),
        gen1Padding(0),
        tenurePadding(0),
        gen2Padding(0),
        fixieTenureFootprint(0),
        untenuredFixieFootprint(0),
        tenuredFixieFootprint(0),
        tenuredFixieCeiling(InitialTenuredFixieCeilingInBytes),
        mode(Heap::MinorCollection),
        fixies(0),
        tenuredFixies(0),
        dirtyTenuredFixies(0),
        markedFixies(0),
        visitedFixies(0),
        lastCollectionTime(system->now()),
        totalCollectionTime(0),
        totalTime(0),
        limitWasExceeded(false)
  {
    if (not system->success(system->make(&lock))) {
      system->abort();
    }
  }

  void dispose()
  {
    gen1.dispose();
    nextGen1.dispose();
    gen2.dispose();
    nextGen2.dispose();
    lock->dispose();
  }

  void disposeFixies()
  {
    free(this, &tenuredFixies, true);
    free(this, &dirtyTenuredFixies, true);
    free(this, &fixies, true);
  }

  System* system;
  Heap::Client* client;

  uint64_t count;
  uint64_t limit;

  System::Mutex* lock;

  uintptr_t* immortalHeapStart;
  uintptr_t* immortalHeapEnd;

  Segment::Map ageMap;
  Segment gen1;

  Segment::Map nextAgeMap;
  Segment nextGen1;

  Segment::Map pointerMap;
  Segment::Map pageMap;
  Segment::Map heapMap;
  Segment gen2;

  Segment::Map nextPointerMap;
  Segment::Map nextPageMap;
  Segment::Map nextHeapMap;
  Segment nextGen2;

  unsigned gen2Base;

  unsigned incomingFootprint;
  int pendingAllocation;
  unsigned tenureFootprint;
  unsigned gen1Padding;
  unsigned tenurePadding;
  unsigned gen2Padding;

  unsigned fixieTenureFootprint;
  unsigned untenuredFixieFootprint;
  unsigned tenuredFixieFootprint;
  unsigned tenuredFixieCeiling;

  Heap::CollectionType mode;

  Fixie* fixies;
  Fixie* tenuredFixies;
  Fixie* dirtyTenuredFixies;
  Fixie* markedFixies;
  Fixie* visitedFixies;

  int64_t lastCollectionTime;
  int64_t totalCollectionTime;
  int64_t totalTime;

  bool limitWasExceeded;
};

const char* segment(Context* c, void* p)
{
  if (c->gen1.contains(p)) {
    return "gen1";
  } else if (c->nextGen1.contains(p)) {
    return "nextGen1";
  } else if (c->gen2.contains(p)) {
    return "gen2";
  } else if (c->nextGen2.contains(p)) {
    return "nextGen2";
  } else {
    return "none";
  }
}

inline Aborter* getAborter(Context* c)
{
  return c->system;
}

inline unsigned minimumNextGen1Capacity(Context* c)
{
  return c->gen1.position() - c->tenureFootprint + c->incomingFootprint
         + c->gen1Padding;
}

inline unsigned minimumNextGen2Capacity(Context* c)
{
  return c->gen2.position() + c->tenureFootprint + c->tenurePadding
         + c->gen2Padding;
}

inline bool oversizedGen2(Context* c)
{
  return c->gen2.capacity() > (InitialGen2CapacityInBytes / BytesPerWord)
         and c->gen2.position() < (c->gen2.capacity() / 4);
}

inline void initNextGen1(Context* c)
{
  new (&(c->nextAgeMap))
      Segment::Map(&(c->nextGen1), max(1, log(TenureThreshold)), 1, 0, false);

  unsigned minimum = minimumNextGen1Capacity(c);
  unsigned desired = minimum;

  new (&(c->nextGen1)) Segment(c, &(c->nextAgeMap), desired, minimum);

  if (Verbose2) {
    fprintf(stderr,
            "init nextGen1 to %d bytes\n",
            c->nextGen1.capacity() * BytesPerWord);
  }
}

inline void initNextGen2(Context* c)
{
  new (&(c->nextPointerMap)) Segment::Map(&(c->nextGen2), 1, 1, 0, true);

  new (&(c->nextPageMap)) Segment::Map(&(c->nextGen2),
                                       1,
                                       LikelyPageSizeInBytes / BytesPerWord,
                                       &(c->nextPointerMap),
                                       true);

  new (&(c->nextHeapMap)) Segment::Map(
      &(c->nextGen2), 1, c->pageMap.scale * 1024, &(c->nextPageMap), true);

  unsigned minimum = minimumNextGen2Capacity(c);
  unsigned desired = minimum;

  if (not oversizedGen2(c)) {
    desired *= 2;
  }

  if (desired < InitialGen2CapacityInBytes / BytesPerWord) {
    desired = InitialGen2CapacityInBytes / BytesPerWord;
  }

  new (&(c->nextGen2)) Segment(
      c,
      &(c->nextHeapMap),
      desired,
      minimum,
      static_cast<int64_t>(c->limit / BytesPerWord)
      - (static_cast<int64_t>(c->count / BytesPerWord)
         - c->gen2.footprint(c->gen2.capacity())
         - c->gen1.footprint(c->gen1.capacity()) + c->pendingAllocation));

  if (Verbose2) {
    fprintf(stderr,
            "init nextGen2 to %d bytes\n",
            c->nextGen2.capacity() * BytesPerWord);
  }
}

inline bool fresh(Context* c, void* o)
{
  return c->nextGen1.contains(o) or c->nextGen2.contains(o)
         or (c->gen2.contains(o) and c->gen2.indexOf(o) >= c->gen2Base);
}

inline bool wasCollected(Context* c, void* o)
{
  // An unmanaged class word is not a forwarding pointer. fresh() would
  // see a moved managed class and treat the instance as already copied.
  if (o == 0 or pointerIsUnmanaged(o)) {
    return false;
  }
  return (not fresh(c, o)) and fresh(c, headerPointer(o));
}

inline void* follow(Context* c UNUSED, void* o)
{
  assertT(c, wasCollected(c, o));
  return reinterpret_cast<void*>(headerLoad(o));
}

void free(Context* c, Fixie** fixies, bool resetImmortal)
{
  for (Fixie** p = fixies; *p;) {
    Fixie* f = *p;

    if (f->immortal()) {
      if (resetImmortal) {
        if (DebugFixies) {
          fprintf(stderr, "reset immortal fixie %p\n", f);
        }
        *p = f->next;
        memset(f->mask(), 0, Fixie::maskSize(f->size, f->hasMask()));
        f->next = 0;
        f->handle = 0;
        f->marked(false);
        f->dirty(false);
      } else {
        p = &(f->next);
      }
    } else {
      *p = f->next;
      if (DebugFixies) {
        fprintf(stderr, "free fixie %p\n", f);
      }
      free(c, f, f->totalSize());
    }
  }
}

void kill(Fixie* fixies)
{
  for (Fixie* f = fixies; f; f = f->next) {
    if (!f->immortal()) {
      f->dead(true);
    }
  }
}

void killFixies(Context* c)
{
  assertT(c, c->markedFixies == 0);

  if (c->mode == Heap::MajorCollection) {
    kill(c->tenuredFixies);
    kill(c->dirtyTenuredFixies);
  }
  kill(c->fixies);
}

void sweepFixies(Context* c)
{
  assertT(c, c->markedFixies == 0);

  if (c->mode == Heap::MajorCollection) {
    free(c, &(c->tenuredFixies), true);
    free(c, &(c->dirtyTenuredFixies), true);

    c->tenuredFixieFootprint = 0;
  }
  free(c, &(c->fixies));

  c->untenuredFixieFootprint = 0;

  while (c->visitedFixies) {
    Fixie* f = c->visitedFixies;
    f->remove(c);

    if (not f->immortal()) {
      ++f->age;
      if (f->age > FixieTenureThreshold) {
        f->age = FixieTenureThreshold;
      } else if (static_cast<unsigned>(f->age + 1) == FixieTenureThreshold) {
        c->fixieTenureFootprint += f->totalSize();
      }
    }

    if (f->age >= FixieTenureThreshold) {
      if (DebugFixies) {
        fprintf(stderr, "tenure fixie %p (dirty: %d)\n", f, f->dirty());
      }

      if (not f->immortal()) {
        c->tenuredFixieFootprint += f->totalSize();
      }

      if (f->dirty()) {
        f->add(c, &(c->dirtyTenuredFixies));
      } else {
        f->add(c, &(c->tenuredFixies));
      }
    } else {
      c->untenuredFixieFootprint += f->totalSize();

      f->add(c, &(c->fixies));
    }

    f->marked(false);
  }

  c->tenuredFixieCeiling
      = max(c->tenuredFixieFootprint * 2, InitialTenuredFixieCeilingInBytes);
}

inline void* copyTo(Context* c, Segment* s, void* o, unsigned size)
{
  void* dst = s->claim(size);
  if (dst == 0) {
    fprintf(stderr,
            "[avian] gc to-space exhausted need=%u position=%u cap=%u\n",
            size,
            s->position(),
            s->capacity());
    c->system->abort();
  }
  c->client->copy(o, dst);
  return dst;
}

bool immortalHeapContains(Context* c, void* p)
{
  return p < c->immortalHeapEnd and p >= c->immortalHeapStart;
}

void* copy2(Context* c, void* o)
{
  unsigned size = c->client->copiedSizeInWords(o);

  if (c->gen2.contains(o)) {
    assertT(c, c->mode == Heap::MajorCollection);

    return copyTo(c, &(c->nextGen2), o, size);
  } else if (c->gen1.contains(o)) {
    unsigned age = c->ageMap.get(o);
    if (age == TenureThreshold) {
      if (c->mode == Heap::MinorCollection) {
        return copyTo(c, &(c->gen2), o, size);
      } else {
        return copyTo(c, &(c->nextGen2), o, size);
      }
    } else {
      o = copyTo(c, &(c->nextGen1), o, size);

      c->nextAgeMap.setOnly(o, age + 1);
      if (age + 1 == TenureThreshold) {
        __sync_fetch_and_add(&c->tenureFootprint, size);
      }

      return o;
    }
  } else {
    assertT(c, not c->nextGen1.contains(o));
    assertT(c, not c->nextGen2.contains(o));
    assertT(c, not immortalHeapContains(c, o));

    o = copyTo(c, &(c->nextGen1), o, size);

    c->nextAgeMap.clear(o);

    return o;
  }
}

pthread_mutex_t copyStripe[256];
pthread_once_t stripeOnce = PTHREAD_ONCE_INIT;

void initStripes()
{
  for (unsigned i = 0; i < 256; ++i) {
    pthread_mutex_init(&copyStripe[i], 0);
  }
}

void* copy(Context* c, void* o, bool* won)
{
  if (pointerIsUnmanaged(o)) {
    *won = false;
    return o;
  }

  pthread_once(&stripeOnce, initStripes);

  if (wasCollected(c, o)) {
    *won = false;
    return follow(c, o);
  }

  unsigned stripe
      = static_cast<unsigned>(reinterpret_cast<uintptr_t>(o) >> 4) & 255;
  pthread_mutex_lock(&copyStripe[stripe]);

  if (wasCollected(c, o)) {
    pthread_mutex_unlock(&copyStripe[stripe]);
    *won = false;
    return follow(c, o);
  }

  void* r = copy2(c, o);

  if (Debug) {
    fprintf(stderr,
            "copy %p (%s) to %p (%s)\n",
            o,
            segment(c, o),
            r,
            segment(c, r));
  }

  headerStoreForward(o, r);
  pthread_mutex_unlock(&copyStripe[stripe]);
  *won = true;
  return r;
}

void* update3(Context* c, void* o, bool* needsVisit)
{
  if (pointerIsUnmanaged(o)) {
    *needsVisit = false;
    return o;
  }

  // A slot visited again after the strong scan already holds the
  // to-space pointer. Copying it would overwrite its class word
  // with a forwarding pointer.
  if (fresh(c, o)) {
    *needsVisit = false;
    return o;
  }
  if (fixedHeader(o)) {
    Fixie* f = fixie(o);
    if ((not f->marked()) and (c->mode == Heap::MajorCollection
                               or f->age < FixieTenureThreshold)) {
      ACQUIRE(c->lock);
      if ((not f->marked()) and (c->mode == Heap::MajorCollection
                                 or f->age < FixieTenureThreshold)) {
        if (DebugFixies) {
          fprintf(stderr, "mark fixie %p\n", f);
        }
        f->marked(true);
        f->dead(false);
        f->move(c, &(c->markedFixies));
      }
    }
    *needsVisit = false;
    return o;
  } else if (immortalHeapContains(c, o)) {
    *needsVisit = false;
    return o;
  } else if (wasCollected(c, o)) {
    *needsVisit = false;
    return follow(c, o);
  } else {
    bool won = false;
    void* r = copy(c, o, &won);
    *needsVisit = won;
    return r;
  }
}

void* update2(Context* c, void* o, bool* needsVisit)
{
  if (c->mode == Heap::MinorCollection and c->gen2.contains(o)) {
    *needsVisit = false;
    return o;
  }

  return update3(c, o, needsVisit);
}

void markDirty(Context* c, Fixie* f)
{
  if (not f->dirty()) {
#ifdef USE_ATOMIC_OPERATIONS
    ACQUIRE(c->lock);
#endif

    if (not f->dirty()) {
      f->dirty(true);
      f->move(c, &(c->dirtyTenuredFixies));
    }
  }
}

void markClean(Context* c, Fixie* f)
{
  if (f->dirty()) {
    f->dirty(false);
    if (f->immortal()) {
      f->remove(c);
    } else {
      f->move(c, &(c->tenuredFixies));
    }
  }
}

void updateHeapMap(Context* c,
                   void* p,
                   void* target,
                   unsigned offset,
                   void* result)
{
  Segment* seg;
  Segment::Map* map;

  if (c->mode == Heap::MinorCollection) {
    seg = &(c->gen2);
    map = &(c->heapMap);
  } else {
    seg = &(c->nextGen2);
    map = &(c->nextHeapMap);
  }

  if (not(immortalHeapContains(c, result)
          or (fixedHeader(result)
              and fixie(result)->age >= FixieTenureThreshold)
          or seg->contains(result))) {
    if (target and fixedHeader(target)) {
      Fixie* f = fixie(target);
      assertT(c, offset == 0 or f->hasMask());

      if (static_cast<unsigned>(f->age + 1) >= FixieTenureThreshold) {
        if (DebugFixies) {
          fprintf(stderr,
                  "dirty fixie %p at %d (%p): %p\n",
                  f,
                  offset,
                  f->body() + offset,
                  result);
        }

        f->dirty(true);
        markBit(f->mask(), offset);
      }
    } else if (seg->contains(p)) {
      if (Debug) {
        fprintf(stderr,
                "mark %p (%s) at %p (%s)\n",
                result,
                segment(c, result),
                p,
                segment(c, p));
      }

      map->set(p);
    }
  }
}

void* update(Context* c,
             void** p,
             void* target,
             unsigned offset,
             bool* needsVisit)
{
  void* masked = maskAlignedPointer(*p);
  if (masked == 0 or pointerIsUnmanaged(masked)) {
    *needsVisit = false;
    return masked;
  }

  void* result = update2(c, masked, needsVisit);

  if (result) {
    updateHeapMap(c, p, target, offset, result);
  }

  return result;
}

static const unsigned GcWorkerMax = 32;

struct GcDeque {
  pthread_mutex_t mu;
  void** data;
  unsigned n;
  unsigned cap;
};

struct GcWorker {
  unsigned id;
  GcDeque deque;
  unsigned sliceBegin;
  unsigned sliceEnd;
};

struct GcPool {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  pthread_cond_t doneCv;
  bool ready;
  unsigned n;
  unsigned epoch;
  unsigned phase;
  unsigned arrived;
  unsigned queued;
  unsigned inFlight;
  Context* context;
  GcWorker workers[GcWorkerMax];
  pthread_t threads[GcWorkerMax];
};

GcPool pool;
__thread GcWorker* tlsWorker = 0;

unsigned traceWorkerCount()
{
  static unsigned n = 0;
  if (n != 0) {
    return n;
  }

  const char* env = getenv("AVIAN_GC_THREADS");
  long count = 1;
  if (env != 0 and env[0] != 0) {
    count = strtol(env, 0, 10);
  }
  // Copies share one to-space bump and a per-object stripe lock.
  // On a 2.7GiB young collection, 18 workers took about 1.6–2.0s
  // and one worker took about 0.8–1.2s. Raise AVIAN_GC_THREADS to
  // use the extra workers anyway.
  if (count < 1) {
    count = 1;
  }
  if (count > static_cast<long>(GcWorkerMax)) {
    count = GcWorkerMax;
  }
  n = static_cast<unsigned>(count);
  return n;
}

void dequeInit(GcDeque* q)
{
  pthread_mutex_init(&q->mu, 0);
  q->data = 0;
  q->n = 0;
  q->cap = 0;
}

void dequeReset(GcDeque* q)
{
  pthread_mutex_lock(&q->mu);
  q->n = 0;
  pthread_mutex_unlock(&q->mu);
}

void dequeGrow(GcDeque* q)
{
  unsigned cap = q->cap == 0 ? 1024 : q->cap * 2;
  if (cap < q->cap) {
    fprintf(stderr, "[avian] gc work queue\n");
    abort();
  }
  void** data = static_cast<void**>(malloc(sizeof(void*) * cap));
  if (data == 0) {
    fprintf(stderr, "[avian] gc work queue\n");
    abort();
  }
  if (q->n != 0) {
    memcpy(data, q->data, sizeof(void*) * q->n);
  }
  ::free(q->data);
  q->data = data;
  q->cap = cap;
}

void gcPush(void* p)
{
  if (p == 0) {
    return;
  }
  GcWorker* w = tlsWorker;
  if (w == 0) {
    fprintf(stderr, "[avian] gc push outside trace\n");
    abort();
  }
  GcDeque* q = &w->deque;
  pthread_mutex_lock(&q->mu);
  if (q->n == q->cap) {
    dequeGrow(q);
  }
  q->data[q->n] = p;
  ++q->n;
  __atomic_fetch_add(&pool.queued, 1u, __ATOMIC_ACQ_REL);
  pthread_mutex_unlock(&q->mu);
}

void* gcPop(GcWorker* w)
{
  GcDeque* q = &w->deque;
  pthread_mutex_lock(&q->mu);
  if (q->n == 0) {
    pthread_mutex_unlock(&q->mu);
    return 0;
  }
  --q->n;
  void* p = q->data[q->n];
  __atomic_fetch_add(&pool.inFlight, 1u, __ATOMIC_ACQ_REL);
  __atomic_fetch_sub(&pool.queued, 1u, __ATOMIC_ACQ_REL);
  pthread_mutex_unlock(&q->mu);
  return p;
}

void* gcSteal(GcWorker* self)
{
  for (unsigned step = 0; step < pool.n; ++step) {
    unsigned i = (self->id + 1 + step) % pool.n;
    if (i == self->id) {
      continue;
    }
    void* p = gcPop(&pool.workers[i]);
    if (p != 0) {
      return p;
    }
  }
  return 0;
}

void collect(Context* c, Segment::Map* map, unsigned start, unsigned end,
             bool* dirty, bool expectDirty);
void collect(Context* c, void* target, unsigned offset);

void scanSlice(GcWorker* w)
{
  Context* c = pool.context;
  if (w->sliceBegin >= w->sliceEnd) {
    return;
  }
  bool dirty = false;
  collect(c, &(c->heapMap), w->sliceBegin, w->sliceEnd, &dirty, false);
}

void collect(Context* c, void** p, void* target, unsigned offset)
{
  bool needsVisit = false;
  void* result = update(c, p, target, offset, &needsVisit);
  local::set(p, result);
  if (needsVisit and result != 0) {
    gcPush(result);
  }
}

void scanObject(Context* c, void* copy)
{
  class Walker : public Heap::Walker {
   public:
    Walker(Context* c, void* copy) : c(c), copy(copy)
    {
    }

    virtual bool visit(unsigned offset)
    {
      collect(c, getp(copy, offset), copy, offset);
      return true;
    }

    Context* c;
    void* copy;
  } walker(c, copy);

  c->client->walk(copy, &walker);
}

bool traceFixie(Context* c)
{
  Fixie* f;
  {
    ACQUIRE(c->lock);
    f = c->markedFixies;
    if (f == 0) {
      return false;
    }
    // Count the fixie before it leaves the list. Otherwise another
    // worker can observe an empty list and zero counters and stop
    // while this one has not started the walk.
    __atomic_fetch_add(&pool.inFlight, 1u, __ATOMIC_ACQ_REL);
    f->remove(c);
  }

  class Walker : public Heap::Walker {
   public:
    Walker(Context* c, void** p) : c(c), p(p)
    {
    }

    virtual bool visit(unsigned offset)
    {
      collect(c, p, offset);
      return true;
    }

    Context* c;
    void** p;
  } walker(c, f->body());

  c->client->walk(f->body(), &walker);

  {
    ACQUIRE(c->lock);
    f->move(c, &(c->visitedFixies));
  }

  __atomic_fetch_sub(&pool.inFlight, 1u, __ATOMIC_ACQ_REL);
  return true;
}

bool traceQuiescent(Context* c)
{
  if (__atomic_load_n(&pool.queued, __ATOMIC_ACQUIRE) != 0) {
    return false;
  }
  if (__atomic_load_n(&pool.inFlight, __ATOMIC_ACQUIRE) != 0) {
    return false;
  }
  ACQUIRE(c->lock);
  if (c->markedFixies != 0) {
    return false;
  }
  if (__atomic_load_n(&pool.queued, __ATOMIC_ACQUIRE) != 0) {
    return false;
  }
  if (__atomic_load_n(&pool.inFlight, __ATOMIC_ACQUIRE) != 0) {
    return false;
  }
  return true;
}

void drain(GcWorker* w);

void* gcWorkerMain(void* arg)
{
  GcWorker* self = static_cast<GcWorker*>(arg);
  unsigned seen = 0;
  for (;;) {
    pthread_mutex_lock(&pool.mu);
    while (pool.epoch == seen and pool.phase != 3) {
      pthread_cond_wait(&pool.cv, &pool.mu);
    }
    if (pool.phase == 3) {
      pthread_mutex_unlock(&pool.mu);
      return 0;
    }
    unsigned phase = pool.phase;
    unsigned epoch = pool.epoch;
    pthread_mutex_unlock(&pool.mu);

    tlsWorker = self;
    if (phase == 1) {
      scanSlice(self);
    } else if (phase == 2) {
      drain(self);
    }
    tlsWorker = 0;

    pthread_mutex_lock(&pool.mu);
    ++pool.arrived;
    if (pool.arrived + 1 == pool.n) {
      pthread_cond_signal(&pool.doneCv);
    }
    seen = epoch;
    pthread_mutex_unlock(&pool.mu);
  }
}

void ensurePool(unsigned n)
{
  pthread_once(&stripeOnce, initStripes);
  if (pool.ready) {
    return;
  }

  pool.n = n;
  pthread_mutex_init(&pool.mu, 0);
  pthread_cond_init(&pool.cv, 0);
  pthread_cond_init(&pool.doneCv, 0);
  pool.epoch = 0;
  pool.phase = 0;
  pool.arrived = 0;
  pool.queued = 0;
  pool.inFlight = 0;
  pool.context = 0;
  for (unsigned i = 0; i < n; ++i) {
    pool.workers[i].id = i;
    pool.workers[i].sliceBegin = 0;
    pool.workers[i].sliceEnd = 0;
    dequeInit(&pool.workers[i].deque);
  }
  pool.ready = true;
  for (unsigned i = 1; i < n; ++i) {
    if (pthread_create(
            &pool.threads[i], 0, gcWorkerMain, &pool.workers[i])
        != 0) {
      fprintf(stderr, "[avian] gc worker\n");
      abort();
    }
  }
  fprintf(stderr, "[avian] gc workers=%u\n", n);
}

void beginTrace(Context* c)
{
  ensurePool(traceWorkerCount());
  pool.context = c;
  __atomic_store_n(&pool.queued, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&pool.inFlight, 0u, __ATOMIC_RELAXED);
  for (unsigned i = 0; i < pool.n; ++i) {
    dequeReset(&pool.workers[i].deque);
  }
  tlsWorker = &pool.workers[0];
}

void endTrace()
{
  tlsWorker = 0;
  pool.context = 0;
}

void assignSlices(Context* c)
{
  unsigned end = c->gen2.position();
  unsigned scale = c->heapMap.scale;
  if (scale == 0) {
    scale = 1;
  }
  unsigned chunks = end == 0 ? 0 : (end + scale - 1) / scale;
  for (unsigned i = 0; i < pool.n; ++i) {
    uint64_t c0 = chunks == 0
                      ? 0
                      : (static_cast<uint64_t>(chunks) * i) / pool.n;
    uint64_t c1 = chunks == 0
                      ? 0
                      : (static_cast<uint64_t>(chunks) * (i + 1)) / pool.n;
    uint64_t begin = c0 * scale;
    uint64_t limit = c1 * scale;
    if (begin > end) {
      begin = end;
    }
    if (limit > end) {
      limit = end;
    }
    pool.workers[i].sliceBegin = static_cast<unsigned>(begin);
    pool.workers[i].sliceEnd = static_cast<unsigned>(limit);
  }
}

void startPhase(unsigned phase)
{
  pthread_mutex_lock(&pool.mu);
  pool.phase = phase;
  pool.arrived = 0;
  ++pool.epoch;
  pthread_cond_broadcast(&pool.cv);
  pthread_mutex_unlock(&pool.mu);
}

void waitWorkers()
{
  if (pool.n < 2) {
    return;
  }
  pthread_mutex_lock(&pool.mu);
  while (pool.arrived + 1 < pool.n) {
    pthread_cond_wait(&pool.doneCv, &pool.mu);
  }
  pthread_mutex_unlock(&pool.mu);
}

void drain(GcWorker* w)
{
  Context* c = pool.context;
  while (true) {
    void* p = gcPop(w);
    if (p == 0) {
      p = gcSteal(w);
    }
    if (p != 0) {
      scanObject(c, p);
      __atomic_fetch_sub(&pool.inFlight, 1u, __ATOMIC_ACQ_REL);
      continue;
    }
    if (traceFixie(c)) {
      continue;
    }
    if (traceQuiescent(c)) {
      return;
    }
    sched_yield();
  }
}


void collect(Context* c, void** p)
{
  collect(c, p, 0, 0);
}

void collect(Context* c, void* target, unsigned offset)
{
  collect(c, getp(target, offset), target, offset);
}

void visitDirtyFixies(Context* c, Fixie** p)
{
  while (*p) {
    Fixie* f = *p;

    bool wasDirty UNUSED = false;
    bool clean = true;
    uintptr_t* mask = f->mask();

    unsigned word = 0;
    unsigned bit = 0;
    unsigned wordLimit = wordOf(f->size);
    unsigned bitLimit = bitOf(f->size);

    if (DebugFixies) {
      fprintf(stderr, "clean fixie %p\n", f);
    }

    for (; word <= wordLimit and (word < wordLimit or bit < bitLimit); ++word) {
      if (mask[word]) {
        for (; bit < BitsPerWord and (word < wordLimit or bit < bitLimit);
             ++bit) {
          unsigned index = indexOf(word, bit);

          if (getBit(mask, index)) {
            wasDirty = true;

            clearBit(mask, index);

            if (DebugFixies) {
              fprintf(stderr,
                      "clean fixie %p at %d (%p)\n",
                      f,
                      index,
                      f->body() + index);
            }

            collect(c, f->body(), index);

            if (getBit(mask, index)) {
              clean = false;
            }
          }
        }
        bit = 0;
      }
    }

    if (DebugFixies) {
      fprintf(stderr, "done cleaning fixie %p\n", f);
    }

    assertT(c, wasDirty);

    if (clean) {
      markClean(c, f);
    } else {
      p = &(f->next);
    }
  }
}

void visitMarkedFixies(Context* c)
{
  while (traceFixie(c)) {
  }
}

void collect(Context* c,
             Segment::Map* map,
             unsigned start,
             unsigned end,
             bool* dirty,
             bool expectDirty UNUSED)
{
  bool wasDirty UNUSED = false;
  for (Segment::Map::Iterator it(map, start, end); it.hasMore();) {
    wasDirty = true;
    if (map->child) {
      assertT(c, map->scale > 1);
      unsigned s = it.next();
      unsigned e = s + map->scale;

      map->clearOnly(s);
      bool childDirty = false;
      collect(c, map->child, s, e, &childDirty, true);
      if (childDirty) {
        map->setOnly(s);
        *dirty = true;
      }
    } else {
      assertT(c, map->scale == 1);
      void** p = reinterpret_cast<void**>(map->segment->get(it.next()));

      map->clearOnly(p);
      if (c->nextGen1.contains(*p)) {
        map->setOnly(p);
        *dirty = true;
      } else {
        collect(c, p);

        if (not c->gen2.contains(*p)) {
          map->setOnly(p);
          *dirty = true;
        }
      }
    }
  }

  assertT(c, wasDirty or not expectDirty);
}

void dequeAppend(GcDeque* q, void* p)
{
  if (q->n == q->cap) {
    dequeGrow(q);
  }
  q->data[q->n] = p;
  ++q->n;
}

// The coordinator pushes every root, so the gray stack starts on
// worker 0. Stealing that one deque serializes the drain. Hand each
// worker a slice while the others are still idle. queued is unchanged.
void spreadQueue()
{
  if (pool.n < 2) {
    return;
  }
  unsigned total = 0;
  for (unsigned i = 0; i < pool.n; ++i) {
    total += pool.workers[i].deque.n;
  }
  if (total < pool.n) {
    return;
  }
  void** all = static_cast<void**>(malloc(sizeof(void*) * total));
  if (all == 0) {
    return;
  }
  unsigned n = 0;
  for (unsigned i = 0; i < pool.n; ++i) {
    GcDeque* q = &pool.workers[i].deque;
    if (q->n != 0) {
      memcpy(all + n, q->data, sizeof(void*) * q->n);
      n += q->n;
      q->n = 0;
    }
  }
  for (unsigned i = 0; i < n; ++i) {
    dequeAppend(&pool.workers[i % pool.n].deque, all[i]);
  }
  ::free(all);
}

void drainTrace()
{
  spreadQueue();
  if (pool.n > 1) {
    startPhase(2);
    drain(&pool.workers[0]);
    waitWorkers();
  } else {
    drain(&pool.workers[0]);
  }
}

void scanUnmanagedObject(void* object, void* arg)
{
  Context* c = static_cast<Context*>(arg);
  if (maskAlignedPointer(*static_cast<void**>(object)) == 0) {
    return;
  }

  class Walker : public Heap::Walker {
   public:
    Walker(Context* c, void* copy) : c(c), copy(copy)
    {
    }

    virtual bool visit(unsigned offset)
    {
      collect(c, getp(copy, offset), copy, offset);
      return true;
    }

    Context* c;
    void* copy;
  } walker(c, object);

  c->client->walk(object, &walker);
}

void collect2(Context* c)
{
  c->gen2Base = Top;
  c->tenureFootprint = 0;
  c->fixieTenureFootprint = 0;
  c->gen1Padding = 0;
  c->tenurePadding = 0;

  if (c->mode == Heap::MajorCollection) {
    c->gen2Padding = 0;
  }

  // Preset before any worker copies. Tenure publishes into gen2 at
  // this position, and wasCollected treats index >= gen2Base as
  // to-space. A lazy store from the copier races with the readers.
  if (c->mode == Heap::MinorCollection) {
    c->gen2Base = c->gen2.position();
  }

  beginTrace(c);

  if (c->mode == Heap::MinorCollection and c->gen2.position()) {
    if (pool.n > 1) {
      assignSlices(c);
      startPhase(1);
      scanSlice(&pool.workers[0]);
      waitWorkers();
    } else {
      bool dirty;
      collect(c, &(c->heapMap), 0, c->gen2.position(), &dirty, false);
    }
  }

  if (c->mode == Heap::MinorCollection) {
    visitDirtyFixies(c, &(c->dirtyTenuredFixies));
  }

  class Visitor : public Heap::Visitor {
   public:
    Visitor(Context* c) : c(c)
    {
    }

    virtual void visit(void* p)
    {
      local::collect(c, static_cast<void**>(p));
      visitMarkedFixies(c);
    }

    Context* c;
  } v(c);

  c->client->visitRoots(&v);
  // Metadata and other unmanaged objects are not in a segment, so the
  // card table never records them. Scan every one. Slots that point at
  // managed objects are updated in place; the unmanaged object stays.
  unmanagedForEach(scanUnmanagedObject, c);
  // Roots only push. Drain before weak refs: status() reports
  // Unreachable for a from-space object that has not been forwarded,
  // and that clears WeakHashMap keys which are still strongly held.
  drainTrace();
  c->client->traceWeakRoots(&v);
  drainTrace();
  endTrace();
}

bool limitExceeded(Context* c, int pendingAllocation)
{
  uint64_t count = c->count + static_cast<uint64_t>(pendingAllocation)
                   - (static_cast<uint64_t>(c->gen2.remaining()) * BytesPerWord);

  if (Verbose) {
    if (count > c->limit) {
      if (not c->limitWasExceeded) {
        c->limitWasExceeded = true;
        fprintf(stderr,
                "heap limit %llu exceeded: %llu\n",
                static_cast<unsigned long long>(c->limit),
                static_cast<unsigned long long>(count));
      }
    } else if (c->limitWasExceeded) {
      c->limitWasExceeded = false;
      fprintf(stderr,
              "heap limit %llu no longer exceeded: %llu\n",
              static_cast<unsigned long long>(c->limit),
              static_cast<unsigned long long>(count));
    }
  }

  return count > c->limit;
}

void collect(Context* c)
{
  if (limitExceeded(c, c->pendingAllocation) or oversizedGen2(c)
      or c->tenureFootprint + c->tenurePadding > c->gen2.remaining()
      or c->fixieTenureFootprint + c->tenuredFixieFootprint
         > c->tenuredFixieCeiling) {
    if (Verbose) {
      if (limitExceeded(c, c->pendingAllocation)) {
        fprintf(stderr, "low memory causes ");
      } else if (oversizedGen2(c)) {
        fprintf(stderr, "oversized gen2 causes ");
      } else if (c->tenureFootprint + c->tenurePadding > c->gen2.remaining()) {
        fprintf(stderr, "undersized gen2 causes ");
      } else {
        fprintf(stderr, "fixie ceiling causes ");
      }
    }

    c->mode = Heap::MajorCollection;
  }

  int64_t then;
  if (Verbose) {
    if (c->mode == Heap::MajorCollection) {
      fprintf(stderr, "major collection\n");
    } else {
      fprintf(stderr, "minor collection\n");
    }

    then = c->system->now();
  }

  initNextGen1(c);

  if (c->mode == Heap::MajorCollection) {
    initNextGen2(c);
  }

  collect2(c);

  c->gen1.replaceWith(&(c->nextGen1));
  if (c->mode == Heap::MajorCollection) {
    c->gen2.replaceWith(&(c->nextGen2));
  }

  sweepFixies(c);

  if (Verbose) {
    int64_t now = c->system->now();
    int64_t collection = now - then;
    int64_t run = then - c->lastCollectionTime;
    c->totalCollectionTime += collection;
    c->totalTime += collection + run;
    c->lastCollectionTime = now;

    fprintf(stderr,
            " - collect: %4dms; "
            "total: %4dms; "
            "run: %4dms; "
            "total: %4dms\n",
            static_cast<int>(collection),
            static_cast<int>(c->totalCollectionTime),
            static_cast<int>(run),
            static_cast<int>(c->totalTime - c->totalCollectionTime));

    fprintf(stderr,
            " -             gen1: %8d/%8d bytes\n",
            c->gen1.position() * BytesPerWord,
            c->gen1.capacity() * BytesPerWord);

    fprintf(stderr,
            " -             gen2: %8d/%8d bytes\n",
            c->gen2.position() * BytesPerWord,
            c->gen2.capacity() * BytesPerWord);

    fprintf(stderr,
            " - untenured fixies:          %8d bytes\n",
            c->untenuredFixieFootprint);

    fprintf(stderr,
            " -   tenured fixies:          %8d bytes\n",
            c->tenuredFixieFootprint);
  }
}

void* allocate(Context* c, size_t size, bool limit)
{
  ACQUIRE(c->lock);

  if (DebugAllocation) {
    size = pad(size) + 2 * BytesPerWord;
  }

  if ((not limit) or size + c->count < c->limit) {
    void* p = copyingHeapAllocate(size);
    if (p) {
      c->count += size;

      if (DebugAllocation) {
        static_cast<uintptr_t*>(p)[0] = 0x22377322;
        static_cast<uintptr_t*>(p)[(size / BytesPerWord) - 1] = 0x22377322;
        return static_cast<uintptr_t*>(p) + 1;
      } else {
        return p;
      }
    }
  }
  return 0;
}

void* tryAllocate(Context* c, size_t size)
{
  return allocate(c, size, true);
}

void* allocate(Context* c, size_t size)
{
  void* p = allocate(c, size, false);
  expect(c->system, p);

  return p;
}

void free(Context* c, const void* p, size_t size)
{
  ACQUIRE(c->lock);

  if (DebugAllocation) {
    size = pad(size) + 2 * BytesPerWord;

    memset(const_cast<void*>(p), 0xFE, size - (2 * BytesPerWord));

    p = static_cast<const uintptr_t*>(p) - 1;

    expect(c->system, static_cast<const uintptr_t*>(p)[0] == 0x22377322);

    expect(c->system,
           static_cast<const uintptr_t*>(p)[(size / BytesPerWord) - 1]
           == 0x22377322);
  }

  expect(c->system, c->count >= size);

  copyingHeapFree(p);
  c->count -= size;
}

void free_(Context* c, const void* p, size_t size)
{
  free(c, p, size);
}

class MyHeap : public Heap {
 public:
  MyHeap(System* system, uint64_t limit) : c(system, limit)
  {
  }

  virtual void setClient(Heap::Client* client)
  {
    assertT(&c, c.client == 0);
    c.client = client;
  }

  virtual void setImmortalHeap(uintptr_t* start, unsigned sizeInWords)
  {
    c.immortalHeapStart = start;
    c.immortalHeapEnd = start + sizeInWords;
  }

  virtual uint64_t remaining()
  {
    return c.limit - c.count;
  }

  virtual uint64_t limit()
  {
    return c.limit;
  }

  virtual bool limitExceeded(int pendingAllocation = 0)
  {
    return local::limitExceeded(&c, pendingAllocation);
  }

  virtual void* tryAllocate(size_t size)
  {
    return local::tryAllocate(&c, size);
  }

  virtual void* allocate(size_t size)
  {
    return local::allocate(&c, size);
  }

  virtual void free(const void* p, size_t size)
  {
    free_(&c, p, size);
  }

  virtual void collect(CollectionType type,
                       unsigned incomingFootprint,
                       int pendingAllocation)
  {
    c.mode = type;
    c.incomingFootprint = incomingFootprint;
    c.pendingAllocation = pendingAllocation;

    local::collect(&c);
  }

  virtual unsigned fixedFootprint(unsigned sizeInWords, bool objectMask)
  {
    return Fixie::totalSize(sizeInWords, objectMask);
  }

  virtual void* allocateFixed(unsigned sizeInWords, bool objectMask)
  {
    expect(&c, not limitExceeded());

    unsigned total = Fixie::totalSize(sizeInWords, objectMask);
    // Fixed objects are freed by the collector with free_(), so they
    // must come from this heap.
    void* p = local::allocate(&c, total);

    expect(&c, not limitExceeded());

    return (new (p) Fixie(&c, sizeInWords, objectMask, &(c.fixies)))->body();
  }

  bool needsMark(void* p)
  {
    if (pointerIsUnmanaged(p)) {
      return false;
    }

    assertT(&c, fixedHeader(p) or (not immortalHeapContains(&c, p)));

    if (fixedHeader(p)) {
      return fixie(p)->age >= FixieTenureThreshold;
    } else {
      return c.gen2.contains(p) or c.nextGen2.contains(p);
    }
  }

  bool targetNeedsMark(void* target)
  {
    if (target == 0 or pointerIsUnmanaged(target)) {
      return false;
    }
    return not c.gen2.contains(target)
           and not c.nextGen2.contains(target)
           and not immortalHeapContains(&c, target)
           and not(fixedHeader(target)
                   and fixie(target)->age >= FixieTenureThreshold);
  }

  virtual void mark(void* p, unsigned offset, unsigned count)
  {
    if (needsMark(p)) {
#ifndef USE_ATOMIC_OPERATIONS
      ACQUIRE(c.lock);
#endif

      if (fixedHeader(p)) {
        Fixie* f = fixie(p);
        assertT(&c, offset == 0 or f->hasMask());

        bool dirty = false;
        for (unsigned i = 0; i < count; ++i) {
          void** target = static_cast<void**>(p) + offset + i;
          if (targetNeedsMark(maskAlignedPointer(*target))) {
            if (DebugFixies) {
              fprintf(stderr,
                      "dirty fixie %p at %d (%p): %p\n",
                      f,
                      offset,
                      f->body() + offset,
                      maskAlignedPointer(*target));
            }

            dirty = true;
#ifdef USE_ATOMIC_OPERATIONS
            markBitAtomic(f->mask(), offset + i);
#else
            markBit(f->mask(), offset + i);
#endif
            assertT(&c, getBit(f->mask(), offset + i));
          }
        }

        if (dirty)
          markDirty(&c, f);
      } else {
        Segment::Map* map;
        if (c.gen2.contains(p)) {
          map = &(c.heapMap);
        } else {
          assertT(&c, c.nextGen2.contains(p));
          map = &(c.nextHeapMap);
        }

        for (unsigned i = 0; i < count; ++i) {
          void** target = static_cast<void**>(p) + offset + i;
          if (targetNeedsMark(maskAlignedPointer(*target))) {
#ifdef USE_ATOMIC_OPERATIONS
            map->markAtomic(target);
#else
            map->set(target);
#endif
          }
        }
      }
    }
  }

  virtual void pad(void* p)
  {
    // Identity hash of an unmanaged object is its address. The mark bit
    // is only an accounting flag, and this object is not in a generation.
    if (pointerIsUnmanaged(p)) {
      return;
    }

    // hashCode no longer holds heapLock, so several mutators can account
    // for different objects at once. GC resets these only at a safepoint.
    unsigned* counter;
    if (c.gen1.contains(p)) {
      if (c.ageMap.get(p) == TenureThreshold) {
        counter = &c.tenurePadding;
      } else {
        counter = &c.gen1Padding;
      }
    } else if (c.gen2.contains(p)) {
      counter = &c.gen2Padding;
    } else {
      counter = &c.gen1Padding;
    }
#ifdef USE_ATOMIC_OPERATIONS
    __sync_fetch_and_add(counter, 1u);
#else
    ++(*counter);
#endif
  }

  virtual void* follow(void* p)
  {
    if (p == 0 or pointerIsUnmanaged(p) or fixedHeader(p)) {
      return p;
    } else if (wasCollected(&c, p)) {
      if (Debug) {
        fprintf(stderr,
                "follow %p (%s) to %p (%s)\n",
                p,
                segment(&c, p),
                local::follow(&c, p),
                segment(&c, local::follow(&c, p)));
      }

      return local::follow(&c, p);
    } else {
      return p;
    }
  }

  virtual void postVisit()
  {
    killFixies(&c);
  }

  virtual Status status(void* p)
  {
    p = maskAlignedPointer(p);

    if (p == 0) {
      return Null;
    } else if (pointerIsUnmanaged(p)) {
      // Alive, immovable. Unreachable would drop finalizers and weak keys.
      return Tenured;
    } else if (fixedHeader(p)) {
      Fixie* f = fixie(p);
      return f->dead() ? Unreachable : (static_cast<unsigned>(f->age + 1)
                                            < FixieTenureThreshold
                                            ? Reachable
                                            : Tenured);
    } else if (c.nextGen1.contains(p)) {
      return Reachable;
    } else if (c.nextGen2.contains(p) or immortalHeapContains(&c, p)
               or (c.gen2.contains(p)
                   and (c.mode == Heap::MinorCollection
                        or c.gen2.indexOf(p) >= c.gen2Base))) {
      return Tenured;
    } else if (wasCollected(&c, p)) {
      return Reachable;
    } else {
      return Unreachable;
    }
  }

  virtual CollectionType collectionType()
  {
    return c.mode;
  }

  virtual void disposeFixies()
  {
    c.disposeFixies();
  }

  virtual void dispose()
  {
    c.dispose();
    assertT(&c, c.count == 0);
    c.system->free(this);
  }

  Context c;
};

}  // namespace local

}  // namespace

namespace vm {

Heap* makeHeap(System* system, uint64_t limit)
{
  return new (system->tryAllocate(sizeof(local::MyHeap)))
      local::MyHeap(system, limit);
}

unsigned gcTraceWorkers()
{
  return local::traceWorkerCount();
}

}  // namespace vm
