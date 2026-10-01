/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

import java.util.function.Consumer;

public final class Spliterators {
  private Spliterators() { }

  public static <T> Spliterator<T> spliteratorUnknownSize(Iterator<? extends T> iterator,
      int characteristics) {
    return new IteratorSpliterator<T>(iterator, characteristics);
  }

  private static final class IteratorSpliterator<T> implements Spliterator<T> {
    private final Iterator<? extends T> iterator;
    private final int characteristics;

    IteratorSpliterator(Iterator<? extends T> iterator, int characteristics) {
      this.iterator = iterator;
      this.characteristics = characteristics;
    }

    public boolean tryAdvance(Consumer<? super T> action) {
      if (!iterator.hasNext()) return false;
      action.accept(iterator.next());
      return true;
    }

    public Spliterator<T> trySplit() {
      return null;
    }

    public long estimateSize() {
      return Long.MAX_VALUE;
    }

    public int characteristics() {
      return characteristics;
    }
  }
}
