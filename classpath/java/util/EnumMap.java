/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

public class EnumMap<K extends Enum<K>, V> extends AbstractMap<K, V> {
  private final Class<K> keyType;
  private final LinkedHashMap<K, V> map = new LinkedHashMap<K, V>();

  public EnumMap(Class<K> keyType) {
    if (keyType == null) throw new NullPointerException();
    if (keyType.getEnumConstants() == null) {
      throw new IllegalArgumentException(String.valueOf(keyType));
    }
    this.keyType = keyType;
  }

  public EnumMap(EnumMap<K, ? extends V> other) {
    this(other.keyType);
    map.putAll(other.map);
  }

  public EnumMap(Map<K, ? extends V> other) {
    if (other instanceof EnumMap) {
      EnumMap<K, ? extends V> enumMap = (EnumMap<K, ? extends V>) other;
      this.keyType = enumMap.keyType;
      map.putAll(enumMap.map);
      return;
    }
    if (other.isEmpty()) throw new IllegalArgumentException();
    K first = other.keySet().iterator().next();
    if (first == null) throw new NullPointerException();
    this.keyType = first.getDeclaringClass();
    putAll(other);
  }

  private void checkKey(K key) {
    if (key == null) throw new NullPointerException();
    if (key.getDeclaringClass() != keyType) throw new ClassCastException();
  }

  public boolean isEmpty() {
    return map.isEmpty();
  }

  public int size() {
    return map.size();
  }

  public boolean containsKey(Object key) {
    return map.containsKey(key);
  }

  public boolean containsValue(Object value) {
    return map.containsValue(value);
  }

  public V get(Object key) {
    return map.get(key);
  }

  public V put(K key, V value) {
    checkKey(key);
    return map.put(key, value);
  }

  public void putAll(Map<? extends K, ? extends V> elts) {
    for (Map.Entry<? extends K, ? extends V> entry : elts.entrySet()) {
      put(entry.getKey(), entry.getValue());
    }
  }

  public V remove(Object key) {
    return map.remove(key);
  }

  public void clear() {
    map.clear();
  }

  public Set<Entry<K, V>> entrySet() {
    return map.entrySet();
  }

  public Set<K> keySet() {
    return map.keySet();
  }

  public Collection<V> values() {
    return map.values();
  }

  public boolean equals(Object other) {
    return map.equals(other);
  }

  public int hashCode() {
    return map.hashCode();
  }
}
