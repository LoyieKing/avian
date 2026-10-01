/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.concurrent;

import java.util.AbstractSet;
import java.util.Collection;
import java.util.Iterator;

public class CopyOnWriteArraySet<E> extends AbstractSet<E> {
  private final CopyOnWriteArrayList<E> al = new CopyOnWriteArrayList<E>();

  public CopyOnWriteArraySet() {}

  public CopyOnWriteArraySet(Collection<? extends E> c) {
    addAll(c);
  }

  public int size() {
    return al.size();
  }

  public boolean contains(Object o) {
    return al.contains(o);
  }

  public boolean add(E e) {
    return al.addIfAbsent(e);
  }

  public boolean remove(Object o) {
    return al.remove(o);
  }

  public Iterator<E> iterator() {
    return al.iterator();
  }
}
