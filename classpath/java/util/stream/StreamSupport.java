/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.stream;

import java.util.Spliterator;
import java.util.function.Supplier;

public final class StreamSupport {
  private StreamSupport() { }

  public static <T> Stream<T> stream(Spliterator<T> spliterator, boolean parallel) {
    return RefStream.fromSpliterator(spliterator);
  }

  public static <T> Stream<T> stream(Supplier<? extends Spliterator<T>> supplier, int characteristics,
      boolean parallel) {
    return RefStream.fromSpliterator(supplier.get());
  }
}
