/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

import java.util.function.BiConsumer;
import java.util.function.Function;

public interface Map<K, V> {
  public boolean isEmpty();

  public int size();

  public boolean containsKey(Object obj);

  public boolean containsValue(Object obj);

  public V get(Object key);

  public V put(K key, V value);

  public void putAll(Map<? extends K,? extends V> elts);

  public V remove(Object key);

  public void clear();

  public Set<Entry<K, V>> entrySet();

  public Set<K> keySet();

  public Collection<V> values();

  public boolean equals(Object other);

  public int hashCode();

  public default V getOrDefault(Object key, V defaultValue) {
    V v = get(key);
    return (v != null || containsKey(key)) ? v : defaultValue;
  }

  public default V computeIfAbsent(K key, Function<? super K, ? extends V> mapping) {
    if (mapping == null) {
      throw new NullPointerException();
    }
    V v = get(key);
    if (v == null) {
      V newValue = mapping.apply(key);
      if (newValue != null) {
        put(key, newValue);
      }
      return newValue;
    }
    return v;
  }

  public default boolean remove(Object key, Object value) {
    Object cur = get(key);
    if (!Objects.equals(cur, value) || (cur == null && !containsKey(key))) {
      return false;
    }
    remove(key);
    return true;
  }

  public default void forEach(BiConsumer<? super K, ? super V> action) {
    for (Entry<K, V> entry : entrySet()) {
      action.accept(entry.getKey(), entry.getValue());
    }
  }

  public interface Entry<K, V> {
    public K getKey();

    public V getValue();

    public V setValue(V value);

    public static <K extends Comparable<? super K>, V> Comparator<Entry<K, V>> comparingByKey() {
      return new Comparator<Entry<K, V>>() {
        public int compare(Entry<K, V> a, Entry<K, V> b) {
          return a.getKey().compareTo(b.getKey());
        }
      };
    }
  }
}
