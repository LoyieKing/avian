/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include "avian/tlab.h"

#include "test-harness.h"

using namespace vm;

static bool near(float actual, float expected)
{
  float d = actual - expected;
  if (d < 0.f) {
    d = -d;
  }
  return d < 0.001f;
}

TEST(TlabSize)
{
  assertEqual(50u, TlabSize::targetRefills());

  TlabAverage average;
  average.sample(1.f);
  average.sample(0.f);
  assertEqual(2u, average.count);
  assertTrue(near(average.average, 0.5f));
  average.sample(0.f);
  assertEqual(3u, average.count);
  assertTrue(near(average.average, 0.325f));

  unsigned heap = 128u * 1024u * 1024u;
  uint64_t eden = TlabSize::edenCapacity(heap);
  assertEqual(static_cast<uint64_t>(134217728u / 3u), eden);

  uint64_t heap30 = 30ull << 30;
  assertEqual(heap30 / 3ull, TlabSize::edenCapacity(heap30));
  uint64_t edenCap = static_cast<uint64_t>(0x7fffffffu) * 8u;
  assertEqual(edenCap, TlabSize::edenCapacity(96ull << 30));

  unsigned minimum = TlabSize::minWords(eden);
  assertEqual(2048u / BytesPerWord, minimum);

  unsigned maximum = TlabSize::maxWords(eden, minimum);
  assertEqual((16u * 1024u * 1024u) / BytesPerWord, maximum);

  unsigned edenWords = static_cast<unsigned>(eden / BytesPerWord);
  unsigned initial = TlabSize::initialWords(edenWords, 1.f, minimum, maximum);
  assertEqual(edenWords / 50u, initial);

  float seed = TlabSize::seedFraction(initial, edenWords);
  float expectSeed
      = (static_cast<float>(initial) * 50.f) / static_cast<float>(edenWords);
  assertTrue(near(seed, expectSeed));
  assertTrue(seed <= 1.f);

  assertEqual(initial, TlabSize::resizedWords(1.f, edenWords, minimum, maximum));
  assertEqual(edenWords / 2u / 50u,
              TlabSize::resizedWords(0.5f, edenWords, minimum, maximum));
  assertEqual(minimum, TlabSize::resizedWords(0.f, edenWords, minimum, maximum));

  assertEqual(initial / 64u, TlabSize::wasteLimit(initial));
  assertEqual(1u, TlabSize::wasteLimit(50u));
  assertEqual(1u, TlabSize::wasteLimit(64u));
  assertEqual(1u, TlabSize::wasteLimit(0u));

  assertEqual(initial + 4u,
              TlabSize::computeChunk(initial, 4u, 1000000u, minimum, maximum));
  assertEqual(0u, TlabSize::computeChunk(initial, 4u, 3u, minimum, maximum));
  assertEqual(
      0u, TlabSize::computeChunk(initial, 4u, minimum - 1u, minimum, maximum));
  assertEqual(
      0u, TlabSize::computeChunk(initial, maximum + 1u, 1000000u, minimum, maximum));

  unsigned historicalEden = 64u * 1024u;
  unsigned historicalMin = TlabSize::minWords(historicalEden);
  assertEqual(historicalEden / BytesPerWord,
              TlabSize::maxWords(historicalEden, historicalMin));
}
