/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.concurrent.atomic;

public class AtomicLongArray {
  private final long[] array;

  public AtomicLongArray(int length) {
    array = new long[length];
  }

  public AtomicLongArray(long[] values) {
    array = values.clone();
  }

  public int length() {
    return array.length;
  }

  public long get(int index) {
    return array[index];
  }

  public void set(int index, long value) {
    array[index] = value;
  }

  public long getAndAdd(int index, long delta) {
    long current = array[index];
    array[index] = current + delta;
    return current;
  }

  public long addAndGet(int index, long delta) {
    return array[index] += delta;
  }

  public boolean compareAndSet(int index, long expect, long update) {
    if (array[index] == expect) {
      array[index] = update;
      return true;
    }
    return false;
  }
}
