/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.concurrent.atomic;

import java.lang.reflect.Field;

public abstract class AtomicReferenceFieldUpdater<T, V> {
  protected AtomicReferenceFieldUpdater() {}

  public static <U, W> AtomicReferenceFieldUpdater<U, W> newUpdater(
      Class<U> tclass, Class<W> vclass, String fieldName) {
    if (tclass == null || vclass == null || fieldName == null) throw new NullPointerException();
    try {
      Field field = tclass.getDeclaredField(fieldName);
      field.setAccessible(true);
      if (field.getType() != vclass) {
        throw new ClassCastException(fieldName);
      }
      return new ReflectionUpdater<U, W>(field);
    } catch (NoSuchFieldException e) {
      throw new RuntimeException(e);
    }
  }

  public abstract boolean compareAndSet(T obj, V expect, V update);

  public abstract boolean weakCompareAndSet(T obj, V expect, V update);

  public abstract void set(T obj, V newValue);

  public abstract void lazySet(T obj, V newValue);

  public abstract V get(T obj);

  public V getAndSet(T obj, V newValue) {
    V current = get(obj);
    set(obj, newValue);
    return current;
  }

  private static final class ReflectionUpdater<T, V> extends AtomicReferenceFieldUpdater<T, V> {
    private final Field field;

    ReflectionUpdater(Field field) {
      this.field = field;
    }

    public boolean compareAndSet(T obj, V expect, V update) {
      synchronized (obj) {
        try {
          Object current = field.get(obj);
          if (current == expect) {
            field.set(obj, update);
            return true;
          }
          return false;
        } catch (IllegalAccessException e) {
          throw new RuntimeException(e);
        }
      }
    }

    public boolean weakCompareAndSet(T obj, V expect, V update) {
      return compareAndSet(obj, expect, update);
    }

    public void set(T obj, V newValue) {
      try {
        field.set(obj, newValue);
      } catch (IllegalAccessException e) {
        throw new RuntimeException(e);
      }
    }

    public void lazySet(T obj, V newValue) {
      set(obj, newValue);
    }

    @SuppressWarnings("unchecked")
    public V get(T obj) {
      try {
        return (V) field.get(obj);
      } catch (IllegalAccessException e) {
        throw new RuntimeException(e);
      }
    }
  }
}
