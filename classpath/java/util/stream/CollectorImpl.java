/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.stream;

import java.util.Collections;
import java.util.Set;
import java.util.function.BiConsumer;
import java.util.function.BinaryOperator;
import java.util.function.Function;
import java.util.function.Supplier;

final class CollectorImpl<T, A, R> implements Collector<T, A, R> {
  private final Supplier<A> supplier;
  private final BiConsumer<A, T> accumulator;
  private final BinaryOperator<A> combiner;
  private final Function<A, R> finisher;

  CollectorImpl(Supplier<A> supplier, BiConsumer<A, T> accumulator, BinaryOperator<A> combiner,
      Function<A, R> finisher) {
    this.supplier = supplier;
    this.accumulator = accumulator;
    this.combiner = combiner;
    this.finisher = finisher;
  }

  public Supplier<A> supplier() { return supplier; }

  public BiConsumer<A, T> accumulator() { return accumulator; }

  public BinaryOperator<A> combiner() { return combiner; }

  public Function<A, R> finisher() { return finisher; }

  public Set<Characteristics> characteristics() {
    return Collections.emptySet();
  }
}
