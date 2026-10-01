/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.concurrent.atomic;

public class AtomicIntegerArray {
  private final int[] array;

  public AtomicIntegerArray(int length) {
    array = new int[length];
  }

  public AtomicIntegerArray(int[] values) {
    array = values.clone();
  }

  public int length() {
    return array.length;
  }

  public int get(int index) {
    return array[index];
  }

  public void set(int index, int value) {
    array[index] = value;
  }

  public int getAndAdd(int index, int delta) {
    int current = array[index];
    array[index] = current + delta;
    return current;
  }

  public int addAndGet(int index, int delta) {
    return array[index] += delta;
  }

  public boolean compareAndSet(int index, int expect, int update) {
    if (array[index] == expect) {
      array[index] = update;
      return true;
    }
    return false;
  }
}
