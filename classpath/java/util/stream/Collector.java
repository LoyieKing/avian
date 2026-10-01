/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.stream;

import java.util.Set;
import java.util.function.BiConsumer;
import java.util.function.BinaryOperator;
import java.util.function.Function;
import java.util.function.Supplier;

public interface Collector<T, A, R> {
  Supplier<A> supplier();

  BiConsumer<A, T> accumulator();

  BinaryOperator<A> combiner();

  Function<A, R> finisher();

  Set<Characteristics> characteristics();

  enum Characteristics {
    CONCURRENT,
    UNORDERED,
    IDENTITY_FINISH
  }
}
