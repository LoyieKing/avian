/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

public class LinkedHashMap<K, V> extends HashMap<K, V> {
  static final class OrderCell<K, V> extends MyCell<K, V> {
    OrderCell<K, V> before, after;

    OrderCell(K key, V value, Cell<K, V> next, int hash) {
      super(key, value, next, hash);
    }
  }

  static final class OrderHelper<K, V> extends MyHelper<K, V> {
    public Cell<K, V> make(K key, V value, Cell<K, V> next) {
      return new OrderCell<K, V>(key, value, next, hash(key));
    }
  }

  private OrderCell<K, V> head, tail;

  public LinkedHashMap(int capacity) {
    super(capacity, new OrderHelper<K, V>());
  }

  public LinkedHashMap() {
    this(0);
  }

  public LinkedHashMap(Map<K, V> map) {
    this(map.size());
    putAll(map);
  }

  protected void afterInsert(Cell<K, V> cell) {
    OrderCell<K, V> created = (OrderCell<K, V>) cell;
    if (head == null) {
      head = tail = created;
    } else {
      tail.after = created;
      created.before = tail;
      tail = created;
    }
  }

  protected void afterRemove(Cell<K, V> cell) {
    OrderCell<K, V> created = (OrderCell<K, V>) cell;
    OrderCell<K, V> previous = created.before;
    OrderCell<K, V> next = created.after;
    if (previous == null) {
      head = next;
    } else {
      previous.after = next;
    }
    if (next == null) {
      tail = previous;
    } else {
      next.before = previous;
    }
    created.before = created.after = null;
  }

  protected void afterClear() {
    head = tail = null;
  }

  public Set<Entry<K, V>> entrySet() {
    return new EntrySet();
  }

  public Set<K> keySet() {
    return new KeySet();
  }

  public Collection<V> values() {
    return new Values();
  }

  Iterator<Entry<K, V>> iterator() {
    return new MyIterator();
  }

 private class EntrySet extends AbstractSet<Entry<K, V>> {
    public int size() {
      return LinkedHashMap.this.size();
    }

    public boolean isEmpty() {
      return LinkedHashMap.this.isEmpty();
    }

    public boolean contains(Object o) {
      return (o instanceof Entry<?,?>)
        && containsKey(((Entry<?,?>)o).getKey());
    }

    public boolean add(Entry<K, V> e) {
      return put(e.getKey(), e.getValue()) != null;
    }

    public boolean remove(Object o) {
      return (o instanceof Entry<?,?>) && remove((Entry<?,?>)o);
    }

    public boolean remove(Entry<K, V> e) {
      return LinkedHashMap.this.remove(e.getKey()) != null;
    }

    public Object[] toArray() {
      return toArray(new Object[size()]);
    }

    public <T> T[] toArray(T[] array) {
      return avian.Data.toArray(this, array);
    }

    public void clear() {
      LinkedHashMap.this.clear();
    }

    public Iterator<Entry<K, V>> iterator() {
      return new MyIterator();
    }
  }

  private class KeySet extends AbstractSet<K> {
    public int size() {
      return LinkedHashMap.this.size();
    }

    public boolean isEmpty() {
      return LinkedHashMap.this.isEmpty();
    }

    public boolean contains(Object key) {
      return containsKey(key);
    }

    public boolean add(K key) {
      return put(key, null) != null;
    }

    public boolean remove(Object key) {
      return LinkedHashMap.this.remove(key) != null;
    }

    public Object[] toArray() {
      return toArray(new Object[size()]);
    }

    public <T> T[] toArray(T[] array) {
      return avian.Data.toArray(this, array);
    }

    public void clear() {
      LinkedHashMap.this.clear();
    }

    public Iterator<K> iterator() {
      return new avian.Data.KeyIterator(new MyIterator());
    }
  }

  private class Values implements Collection<V> {
    public int size() {
      return LinkedHashMap.this.size();
    }

    public boolean isEmpty() {
      return LinkedHashMap.this.isEmpty();
    }

    public boolean contains(Object value) {
      return containsValue(value);
    }

    public boolean containsAll(Collection<?> c) {
      if (c == null) {
        throw new NullPointerException("collection is null");
      }

      Iterator<?> it = c.iterator();
      while (it.hasNext()) {
        if (! contains(it.next())) {
          return false;
        }
      }

      return true;
    }

    public boolean add(V value) {
      throw new UnsupportedOperationException();
    }

    public boolean addAll(Collection<? extends V> collection) {
      throw new UnsupportedOperationException();
    }

    public boolean remove(Object value) {
      throw new UnsupportedOperationException();
    }

    public boolean removeAll(Collection<?> c) {
      throw new UnsupportedOperationException();
    }

    public Object[] toArray() {
      return toArray(new Object[size()]);
    }

    public <T> T[] toArray(T[] array) {
      return avian.Data.toArray(this, array);
    }

    public void clear() {
      LinkedHashMap.this.clear();
    }

    public Iterator<V> iterator() {
      return new avian.Data.ValueIterator(new MyIterator());
    }
  }

  private class MyIterator implements Iterator<Entry<K, V>> {
    private OrderCell<K, V> cursor = head;
    private OrderCell<K, V> current;

    public Entry<K, V> next() {
      if (cursor == null) {
        throw new NoSuchElementException();
      }
      current = cursor;
      cursor = cursor.after;
      return current;
    }

    public boolean hasNext() {
      return cursor != null;
    }

    public void remove() {
      if (current == null) {
        throw new IllegalStateException();
      }
      LinkedHashMap.this.remove(current.key);
      current = null;
    }
  }
}

