/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.stream;

public interface IntStream {
  int sum();
}

final class IntPipeline implements IntStream {
  private final int[] data;

  IntPipeline(int[] data) {
    this.data = data;
  }

  public int sum() {
    int sum = 0;
    for (int i = 0; i < data.length; i++) sum += data[i];
    return sum;
  }
}
