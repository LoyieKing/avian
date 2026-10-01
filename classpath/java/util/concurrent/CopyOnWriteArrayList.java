/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.concurrent;

import java.util.AbstractList;
import java.util.Collection;
import java.util.Iterator;
import java.util.List;

public class CopyOnWriteArrayList<E> extends AbstractList<E> implements List<E> {
  private final Object[] empty = new Object[0];
  private Object[] array = empty;

  public CopyOnWriteArrayList() {}

  public CopyOnWriteArrayList(Collection<? extends E> c) {
    addAll(c);
  }

  public CopyOnWriteArrayList(E[] values) {
    array = values.clone();
  }

  public int size() {
    return array.length;
  }

  public E get(int index) {
    return (E) array[index];
  }

  public E set(int index, E element) {
    Object[] next = array.clone();
    E old = (E) next[index];
    next[index] = element;
    array = next;
    return old;
  }

  public void add(int index, E element) {
    if (index < 0 || index > array.length) {
      throw new IndexOutOfBoundsException(String.valueOf(index));
    }
    Object[] next = new Object[array.length + 1];
    System.arraycopy(array, 0, next, 0, index);
    next[index] = element;
    System.arraycopy(array, index, next, index + 1, array.length - index);
    array = next;
  }

  public E remove(int index) {
    E old = get(index);
    Object[] next = new Object[array.length - 1];
    System.arraycopy(array, 0, next, 0, index);
    System.arraycopy(array, index + 1, next, index, array.length - index - 1);
    array = next;
    return old;
  }

  public boolean add(E element) {
    add(size(), element);
    return true;
  }

  public boolean addIfAbsent(E element) {
    if (contains(element)) return false;
    add(element);
    return true;
  }

  public boolean remove(Object element) {
    int index = indexOf(element);
    if (index < 0) return false;
    remove(index);
    return true;
  }

  public Iterator<E> iterator() {
    return new Iterator<E>() {
      private final Object[] snapshot = array;
      private int index;

      public boolean hasNext() {
        return index < snapshot.length;
      }

      public E next() {
        if (!hasNext()) throw new java.util.NoSuchElementException();
        return (E) snapshot[index++];
      }

      public void remove() {
        throw new UnsupportedOperationException();
      }
    };
  }

  public int hashCode() {
    int hash = 1;
    Object[] snapshot = array;
    for (int i = 0; i < snapshot.length; ++i) {
      Object element = snapshot[i];
      hash = 31 * hash + (element == null ? 0 : element.hashCode());
    }
    return hash;
  }
}
