/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.lang;

public class ClassValue<T> {
  private final java.util.HashMap<Class<?>, Entry<T>> values = new java.util.HashMap<Class<?>, Entry<T>>();

  protected ClassValue() {}

  protected T computeValue(Class<?> type) {
    throw new UnsupportedOperationException();
  }

  public T get(Class<?> type) {
    if (type == null) throw new NullPointerException();
    synchronized (values) {
      Entry<T> existing = values.get(type);
      if (existing != null) return existing.value;
    }
    T computed = computeValue(type);
    synchronized (values) {
      Entry<T> existing = values.get(type);
      if (existing != null) return existing.value;
      values.put(type, new Entry<T>(computed));
      return computed;
    }
  }

  public void remove(Class<?> type) {
    if (type == null) throw new NullPointerException();
    synchronized (values) {
      values.remove(type);
    }
  }

  private static final class Entry<T> {
    final T value;

    Entry(T value) {
      this.value = value;
    }
  }
}
