/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

public abstract class AbstractMap<K,V> extends Object implements Map<K,V> {
  public static class SimpleImmutableEntry<K, V> implements Map.Entry<K, V> {
    private final K key;
    private final V value;

    public SimpleImmutableEntry(K key, V value) {
      this.key = key;
      this.value = value;
    }

    public K getKey() {
      return key;
    }

    public V getValue() {
      return value;
    }

    public V setValue(V value) {
      throw new UnsupportedOperationException();
    }
  }
}
