/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef HEAP_H
#define HEAP_H

#include <avian/system/system.h>
#include <avian/util/allocator.h>

#include <stdint.h>

namespace vm {

// an object must survive TenureThreshold + 2 garbage collections
// before being copied to gen2 (must be at least 1):
const unsigned TenureThreshold = 3;

const unsigned FixieTenureThreshold = TenureThreshold + 2;

// The age stored in the header of an immortal fixed object: one that
// is never freed and never moves between generations, and is linked
// into the collector's lists only while dirty.  The only such objects
// are the fixed objects of a boot image (static tables, the system
// class loader, addendums), which the boot image generator lays out
// with this age and the heap adopts via setImmortalHeap; objects
// allocated at run time are always mortal.
const unsigned ImmortalFixieAge = FixieTenureThreshold + 1;

class Heap : public avian::util::Allocator {
 public:
  enum CollectionType { MinorCollection, MajorCollection };

  enum Status { Null, Reachable, Unreachable, Tenured };

  class Visitor {
   public:
    virtual void visit(void*) = 0;
  };

  class Walker {
   public:
    virtual bool visit(unsigned) = 0;
  };

  class Client {
   public:
    virtual void collect(void* context, CollectionType type) = 0;
    virtual void visitRoots(Visitor*) = 0;
    // After the strong closure has been copied. status() is wrong for
    // an object that is still only on the gray stack, and Reference
    // targets are nogc fields, so this pass must not run earlier.
    virtual void traceWeakRoots(Visitor*)
    {
    }
    virtual bool isFixed(void*) = 0;
    virtual unsigned sizeInWords(void*) = 0;
    virtual unsigned copiedSizeInWords(void*) = 0;
    virtual void copy(void*, void*) = 0;
    virtual void walk(void*, Walker*) = 0;
    // A managed slot the walk deliberately did not trace. The object
    // must stay on the dirty list so a later pass can update it.
    virtual bool retainsManagedSlot(void*)
    {
      return false;
    }
  };

  virtual void setClient(Client* client) = 0;
  virtual void setImmortalHeap(uintptr_t* start, unsigned sizeInWords) = 0;
  virtual uint64_t remaining() = 0;
  virtual uint64_t limit() = 0;
  virtual bool limitExceeded(int pendingAllocation = 0) = 0;
  virtual void collect(CollectionType type,
                       unsigned footprint,
                       int pendingAllocation) = 0;
  virtual unsigned fixedFootprint(unsigned sizeInWords, bool objectMask) = 0;
  // Allocates an object that never moves, from this heap.
  virtual void* allocateFixed(unsigned sizeInWords, bool objectMask) = 0;
  virtual void mark(void* p, unsigned offset, unsigned count) = 0;
  virtual void pad(void* p) = 0;
  virtual void* follow(void* p) = 0;

  template <class T>
  T* follow(T* p)
  {
    return static_cast<T*>(follow(static_cast<void*>(p)));
  }

  virtual void postVisit() = 0;
  virtual Status status(void* p) = 0;
  virtual CollectionType collectionType() = 0;
  virtual void disposeFixies() = 0;
  virtual void dispose() = 0;
};

Heap* makeHeap(System* system, uint64_t limit);

// Threads that trace during a stop-the-world collection, including the
// mutator that requested it. 1 before the first collection.
unsigned gcTraceWorkers();

}  // namespace vm

#endif  // HEAP_H
