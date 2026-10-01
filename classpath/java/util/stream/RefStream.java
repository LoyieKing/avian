/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.stream;

import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;
import java.util.Optional;
import java.util.Spliterator;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.function.ToIntFunction;

/** Sequential stream. Each intermediate op snapshots the elements it sees. */
public final class RefStream<T> implements Stream<T> {
  private final List<T> data;

  private RefStream(List<T> data) {
    this.data = data;
  }

  public static <T> Stream<T> fromCollection(Collection<T> source) {
    List<T> copy = new ArrayList<T>();
    if (source != null) {
      for (T value : source) copy.add(value);
    }
    return new RefStream<T>(copy);
  }

  public static <T> Stream<T> fromSpliterator(Spliterator<T> spliterator) {
    final List<T> copy = new ArrayList<T>();
    if (spliterator != null) {
      while (spliterator.tryAdvance(new Consumer<T>() {
        public void accept(T value) { copy.add(value); }
      })) { }
    }
    return new RefStream<T>(copy);
  }

  public Stream<T> filter(Predicate<? super T> predicate) {
    List<T> out = new ArrayList<T>();
    for (int i = 0; i < data.size(); i++) {
      T value = data.get(i);
      if (predicate.test(value)) out.add(value);
    }
    return new RefStream<T>(out);
  }

  public <R> Stream<R> map(Function<? super T, ? extends R> mapper) {
    List<R> out = new ArrayList<R>();
    for (int i = 0; i < data.size(); i++) out.add(mapper.apply(data.get(i)));
    return new RefStream<R>(out);
  }

  public <R> Stream<R> flatMap(Function<? super T, ? extends Stream<? extends R>> mapper) {
    List<R> out = new ArrayList<R>();
    for (int i = 0; i < data.size(); i++) {
      Stream<? extends R> nested = mapper.apply(data.get(i));
      if (nested == null) continue;
      RefStream<? extends R> ref = (RefStream<? extends R>) nested;
      for (int j = 0; j < ref.data.size(); j++) out.add(ref.data.get(j));
    }
    return new RefStream<R>(out);
  }

  public IntStream mapToInt(ToIntFunction<? super T> mapper) {
    int[] out = new int[data.size()];
    for (int i = 0; i < data.size(); i++) out[i] = mapper.applyAsInt(data.get(i));
    return new IntPipeline(out);
  }

  public Stream<T> sorted() {
    List<T> out = new ArrayList<T>(data);
    Collections.sort(out);
    return new RefStream<T>(out);
  }

  public Stream<T> sorted(Comparator<? super T> comparator) {
    List<T> out = new ArrayList<T>(data);
    Collections.sort(out, comparator);
    return new RefStream<T>(out);
  }

  public Stream<T> peek(Consumer<? super T> action) {
    for (int i = 0; i < data.size(); i++) action.accept(data.get(i));
    return this;
  }

  public void forEach(Consumer<? super T> action) {
    for (int i = 0; i < data.size(); i++) action.accept(data.get(i));
  }

  public Iterator<T> iterator() {
    return data.iterator();
  }

  public boolean anyMatch(Predicate<? super T> predicate) {
    for (int i = 0; i < data.size(); i++) {
      if (predicate.test(data.get(i))) return true;
    }
    return false;
  }

  public boolean allMatch(Predicate<? super T> predicate) {
    for (int i = 0; i < data.size(); i++) {
      if (!predicate.test(data.get(i))) return false;
    }
    return true;
  }

  public Optional<T> findFirst() {
    if (data.isEmpty()) return Optional.empty();
    return Optional.of(data.get(0));
  }

  public List<T> toList() {
    return new ArrayList<T>(data);
  }

  public <R, A> R collect(Collector<? super T, A, R> collector) {
    A box = collector.supplier().get();
    BiConsumer<A, ? super T> accumulator = collector.accumulator();
    for (int i = 0; i < data.size(); i++) accumulator.accept(box, data.get(i));
    return collector.finisher().apply(box);
  }

  public <R> R collect(Supplier<R> supplier, BiConsumer<R, ? super T> accumulator,
      BiConsumer<R, R> combiner) {
    R box = supplier.get();
    for (int i = 0; i < data.size(); i++) accumulator.accept(box, data.get(i));
    return box;
  }
}
