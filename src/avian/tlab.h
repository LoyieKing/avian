/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_TLAB_H
#define AVIAN_TLAB_H

#include "avian/common.h"

namespace vm {

// HotSpot JDK 21 ThreadLocalAllocBuffer sizing. The defaults are the
// JDK product flags: MinTLABSize, TLABWasteTargetPercent,
// TLABRefillWasteFraction, TLABWasteIncrement, TLABAllocationWeight.
// target refills is 100 / (2 * waste percent), and at least 2: a TLAB
// is assumed half full at a collection, so 50 refills waste about one
// percent of the thread's allocation.

const unsigned TlabMinBytes = 2 * 1024;
const unsigned TlabMaxBytes = 16 * 1024 * 1024;
const unsigned TlabWasteTargetPercent = 1;
const unsigned TlabRefillWasteFraction = 64;
const unsigned TlabWasteIncrementWords = 4;
const unsigned TlabAllocationWeight = 35;
const unsigned TlabAverageOldThreshold = 100;
// NewRatio 2: the young budget is one third of the heap limit.
const unsigned TlabEdenDivisor = 3;
// Objects up to the old fixed chunk stayed movable. Keep that window
// whenever the young budget can hold it.
const unsigned TlabHistoricalBytes = 64 * 1024;
const unsigned TlabEdenFloorBytes = 4 * 1024 * 1024;

// AdaptiveWeightedAverage from HotSpot gcUtil, weight fixed at
// TlabAllocationWeight. The first samples use 100/count until that
// drops to the weight, so the average settles before it starts to lag.
struct TlabAverage {
  float average;
  unsigned count;
  bool old;

  TlabAverage() : average(0), count(0), old(false)
  {
  }

  void sample(float value)
  {
    ++count;
    if (not old and count > TlabAverageOldThreshold) {
      old = true;
    }

    unsigned countWeight = 0;
    if (not old) {
      countWeight = TlabAverageOldThreshold / count;
    }

    unsigned weight = TlabAllocationWeight;
    if (countWeight > weight) {
      weight = countWeight;
    }

    average = ((100.0f - static_cast<float>(weight)) * average
               + static_cast<float>(weight) * value)
              / 100.0f;
  }
};

// Thread carries this between the backup heap and the debug fields.
// target-fields.h follows from this size plus the three unsigneds and
// the heapEnd pointer inserted after Thread::heap.
static_assert(sizeof(TlabAverage) == 12,
              "TlabAverage size changed; update Thread field offsets");

struct TlabSize {
  static unsigned targetRefills()
  {
    unsigned refills = 100 / (2 * TlabWasteTargetPercent);
    if (refills < 2) {
      refills = 2;
    }
    return refills;
  }

  // TLAB and segment sizes are 32-bit word counts. 16GiB-8 keeps
  // edenWords inside that range; a 30g heap asks for 10GiB and is
  // unaffected. Heaps above 48GiB saturate the nursery here.
  static uint64_t edenCapacity(uint64_t heapLimit)
  {
    uint64_t third = heapLimit / TlabEdenDivisor;
    uint64_t half = heapLimit / 2;
    uint64_t floorBytes = TlabEdenFloorBytes;
    if (floorBytes > half) {
      floorBytes = half;
    }
    uint64_t result = floorBytes;
    if (third > result) {
      result = third;
    }
    const uint64_t maxEden = static_cast<uint64_t>(0x7fffffffu) * 8u;
    if (result > maxEden) {
      result = maxEden;
    }
    return result;
  }

  static unsigned minWords(uint64_t edenBytes)
  {
    unsigned bytes = TlabMinBytes;
    if (static_cast<uint64_t>(bytes) > edenBytes) {
      bytes = static_cast<unsigned>(edenBytes);
    }
    unsigned words = bytes / BytesPerWord;
    uint64_t edenWords = edenBytes / BytesPerWord;
    if (words < 1) {
      words = 1;
    }
    if (edenWords > 0 and static_cast<uint64_t>(words) > edenWords) {
      words = static_cast<unsigned>(edenWords);
    }
    return words;
  }

  static unsigned maxWords(uint64_t edenBytes, unsigned minimum)
  {
    uint64_t maxBytes = edenBytes / 2;
    if (maxBytes > TlabMaxBytes) {
      maxBytes = TlabMaxBytes;
    }
    if (maxBytes < TlabHistoricalBytes and edenBytes >= TlabHistoricalBytes) {
      maxBytes = TlabHistoricalBytes;
    }
    unsigned words = static_cast<unsigned>(maxBytes / BytesPerWord);
    uint64_t edenWords = edenBytes / BytesPerWord;
    if (words < minimum) {
      words = minimum;
    }
    if (edenWords > 0 and static_cast<uint64_t>(words) > edenWords) {
      words = static_cast<unsigned>(edenWords);
    }
    if (words < 1) {
      words = 1;
    }
    return words;
  }

  static unsigned clamp(unsigned words, unsigned minimum, unsigned maximum)
  {
    if (words < minimum) {
      return minimum;
    }
    if (words > maximum) {
      return maximum;
    }
    return words;
  }

  static unsigned initialWords(unsigned edenWords,
                               float threadAverage,
                               unsigned minimum,
                               unsigned maximum)
  {
    unsigned threads = static_cast<unsigned>(threadAverage + 0.5f);
    if (threads < 1) {
      threads = 1;
    }
    unsigned denom = threads * targetRefills();
    unsigned size = denom == 0 ? minimum : edenWords / denom;
    return clamp(size, minimum, maximum);
  }

  // desired = fraction * eden / targetRefills, then clamped.
  static unsigned resizedWords(float fraction,
                               unsigned edenWords,
                               unsigned minimum,
                               unsigned maximum)
  {
    if (fraction < 0.f) {
      fraction = 0.f;
    }
    if (fraction > 1.f) {
      fraction = 1.f;
    }
    unsigned alloc = static_cast<unsigned>(fraction * static_cast<float>(edenWords));
    unsigned refills = targetRefills();
    unsigned size = refills == 0 ? minimum : alloc / refills;
    return clamp(size, minimum, maximum);
  }

  static unsigned wasteLimit(unsigned desiredWords)
  {
    unsigned limit = desiredWords / TlabRefillWasteFraction;
    if (limit < 1) {
      limit = 1;
    }
    return limit;
  }

  // Zero means the chunk cannot be carved out of what is left: the
  // caller collects. Otherwise the chunk fits the object and is at
  // least the minimum, and at most desired+object, the max, and the
  // bytes still in the young budget.
  static unsigned computeChunk(unsigned desiredWords,
                               unsigned objectWords,
                               unsigned availableWords,
                               unsigned minimum,
                               unsigned maximum)
  {
    if (objectWords == 0 or availableWords < objectWords
        or objectWords > maximum) {
      return 0;
    }

    unsigned summed = desiredWords + objectWords;
    if (summed < desiredWords) {
      summed = maximum;
    }
    if (summed > availableWords) {
      summed = availableWords;
    }
    if (summed > maximum) {
      summed = maximum;
    }
    if (summed < objectWords or summed < minimum) {
      return 0;
    }
    return summed;
  }

  static float seedFraction(unsigned desiredWords, unsigned edenWords)
  {
    if (edenWords == 0) {
      return 1.f;
    }
    float fraction = (static_cast<float>(desiredWords)
                      * static_cast<float>(targetRefills()))
                     / static_cast<float>(edenWords);
    if (fraction < 0.f) {
      fraction = 0.f;
    }
    if (fraction > 1.f) {
      fraction = 1.f;
    }
    return fraction;
  }
};

}  // namespace vm

#endif  // AVIAN_TLAB_H
