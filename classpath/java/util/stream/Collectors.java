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
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.BiConsumer;
import java.util.function.BinaryOperator;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.function.ToLongFunction;

public final class Collectors {
  private Collectors() { }

  public static <T> Collector<T, ?, List<T>> toList() {
    return new CollectorImpl<T, List<T>, List<T>>(
        new Supplier<List<T>>() {
          public List<T> get() { return new ArrayList<T>(); }
        },
        new BiConsumer<List<T>, T>() {
          public void accept(List<T> list, T value) { list.add(value); }
        },
        new BinaryOperator<List<T>>() {
          public List<T> apply(List<T> left, List<T> right) {
            left.addAll(right);
            return left;
          }
        },
        new Function<List<T>, List<T>>() {
          public List<T> apply(List<T> list) { return list; }
        });
  }

  public static <T> Collector<T, ?, Set<T>> toSet() {
    return new CollectorImpl<T, Set<T>, Set<T>>(
        new Supplier<Set<T>>() {
          public Set<T> get() { return new HashSet<T>(); }
        },
        new BiConsumer<Set<T>, T>() {
          public void accept(Set<T> set, T value) { set.add(value); }
        },
        new BinaryOperator<Set<T>>() {
          public Set<T> apply(Set<T> left, Set<T> right) {
            left.addAll(right);
            return left;
          }
        },
        new Function<Set<T>, Set<T>>() {
          public Set<T> apply(Set<T> set) { return set; }
        });
  }

  public static <T, C extends Collection<T>> Collector<T, ?, C> toCollection(final Supplier<C> factory) {
    return new CollectorImpl<T, C, C>(
        factory,
        new BiConsumer<C, T>() {
          public void accept(C collection, T value) { collection.add(value); }
        },
        new BinaryOperator<C>() {
          public C apply(C left, C right) {
            left.addAll(right);
            return left;
          }
        },
        new Function<C, C>() {
          public C apply(C collection) { return collection; }
        });
  }

  public static <T, K, U> Collector<T, ?, Map<K, U>> toMap(
      final Function<? super T, ? extends K> keyMapper,
      final Function<? super T, ? extends U> valueMapper) {
    return toMap(keyMapper, valueMapper, new BinaryOperator<U>() {
      public U apply(U left, U right) {
        throw new IllegalStateException("Duplicate key");
      }
    }, new Supplier<Map<K, U>>() {
      public Map<K, U> get() { return new HashMap<K, U>(); }
    });
  }

  public static <T, K, U, M extends Map<K, U>> Collector<T, ?, M> toMap(
      final Function<? super T, ? extends K> keyMapper,
      final Function<? super T, ? extends U> valueMapper,
      final BinaryOperator<U> merge,
      final Supplier<M> mapFactory) {
    return new CollectorImpl<T, M, M>(
        mapFactory,
        new BiConsumer<M, T>() {
          public void accept(M map, T value) {
            K key = keyMapper.apply(value);
            U mapped = valueMapper.apply(value);
            if (map.containsKey(key)) map.put(key, merge.apply(map.get(key), mapped));
            else map.put(key, mapped);
          }
        },
        new BinaryOperator<M>() {
          public M apply(M left, M right) {
            for (Map.Entry<K, U> entry : right.entrySet()) {
              K key = entry.getKey();
              if (left.containsKey(key)) left.put(key, merge.apply(left.get(key), entry.getValue()));
              else left.put(key, entry.getValue());
            }
            return left;
          }
        },
        new Function<M, M>() {
          public M apply(M map) { return map; }
        });
  }

  public static <T> Collector<T, ?, Long> summingLong(final ToLongFunction<? super T> mapper) {
    return new CollectorImpl<T, long[], Long>(
        new Supplier<long[]>() {
          public long[] get() { return new long[1]; }
        },
        new BiConsumer<long[], T>() {
          public void accept(long[] box, T value) { box[0] += mapper.applyAsLong(value); }
        },
        new BinaryOperator<long[]>() {
          public long[] apply(long[] left, long[] right) {
            left[0] += right[0];
            return left;
          }
        },
        new Function<long[], Long>() {
          public Long apply(long[] box) { return Long.valueOf(box[0]); }
        });
  }

  public static Collector<CharSequence, ?, String> joining(final CharSequence delimiter) {
    return new CollectorImpl<CharSequence, StringBuilder, String>(
        new Supplier<StringBuilder>() {
          public StringBuilder get() { return new StringBuilder(); }
        },
        new BiConsumer<StringBuilder, CharSequence>() {
          public void accept(StringBuilder builder, CharSequence value) {
            if (builder.length() > 0) builder.append(delimiter);
            builder.append(value);
          }
        },
        new BinaryOperator<StringBuilder>() {
          public StringBuilder apply(StringBuilder left, StringBuilder right) {
            if (left.length() > 0 && right.length() > 0) left.append(delimiter);
            left.append(right);
            return left;
          }
        },
        new Function<StringBuilder, String>() {
          public String apply(StringBuilder builder) { return builder.toString(); }
        });
  }

  public static <T, K, D, A, M extends Map<K, D>> Collector<T, ?, M> groupingBy(
      final Function<? super T, ? extends K> classifier,
      final Supplier<M> mapFactory,
      final Collector<? super T, A, D> downstream) {
    return new CollectorImpl<T, Map<K, A>, M>(
        new Supplier<Map<K, A>>() {
          public Map<K, A> get() { return new LinkedHashMap<K, A>(); }
        },
        new BiConsumer<Map<K, A>, T>() {
          public void accept(Map<K, A> map, T value) {
            K key = classifier.apply(value);
            A box = map.get(key);
            if (box == null && !map.containsKey(key)) {
              box = downstream.supplier().get();
              map.put(key, box);
            }
            downstream.accumulator().accept(box, value);
          }
        },
        new BinaryOperator<Map<K, A>>() {
          public Map<K, A> apply(Map<K, A> left, Map<K, A> right) {
            return left;
          }
        },
        new Function<Map<K, A>, M>() {
          public M apply(Map<K, A> map) {
            M out = mapFactory.get();
            for (Map.Entry<K, A> entry : map.entrySet()) {
              out.put(entry.getKey(), downstream.finisher().apply(entry.getValue()));
            }
            return out;
          }
        });
  }
}
