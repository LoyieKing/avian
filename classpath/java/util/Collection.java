/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

public interface Collection<T> extends Iterable<T> {
  public int size();

  public boolean isEmpty();

  public boolean contains(Object element);

  public boolean containsAll(Collection<?> c);

  public boolean add(T element);

  public boolean addAll(Collection<? extends T> collection);

  public boolean remove(Object element);

  public boolean removeAll(Collection<?> c);

  public default boolean retainAll(Collection<?> c) {
    if (c == null) {
      throw new NullPointerException();
    }
    boolean modified = false;
    Iterator<T> it = iterator();
    while (it.hasNext()) {
      if (!c.contains(it.next())) {
        it.remove();
        modified = true;
      }
    }
    return modified;
  }

  public default boolean removeIf(java.util.function.Predicate<? super T> filter) {
    boolean removed = false;
    Iterator<T> it = iterator();
    while (it.hasNext()) {
      if (filter.test(it.next())) {
        it.remove();
        removed = true;
      }
    }
    return removed;
  }

  public Object[] toArray();

  public <S> S[] toArray(S[] array);

  public void clear();

  public default java.util.stream.Stream<T> stream() {
    return java.util.stream.RefStream.fromCollection(this);
  }

  public default java.util.stream.Stream<T> parallelStream() {
    return stream();
  }

  public default void forEach(java.util.function.Consumer<? super T> action) {
    Iterator<T> it = iterator();
    while (it.hasNext()) action.accept(it.next());
  }
}
