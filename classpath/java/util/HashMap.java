/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

import avian.Data;

import java.util.function.BiConsumer;

public class HashMap<K, V> implements Map<K, V> {
  private static final int MinimumCapacity = 16;

  private int size;
  private Cell[] array;
  private final Helper helper;
  // Cached so get/put do not interface-call the helper on every probe.
  private final boolean plain;

  public HashMap(int capacity, Helper<K, V> helper) {
    if (capacity > 0) {
      array = new Cell[Data.nextPowerOfTwo(capacity)];
    }
    this.helper = helper;
    this.plain = helper != null && helper.directCells();
  }

  public HashMap(int capacity) {
    this(capacity, new MyHelper());
  }

  public HashMap() {
    this(0);
  }

  public HashMap(Map<K, V> map) {
    this(map.size());
    for (Map.Entry<K, V> entry : map.entrySet()) {
      put(entry.getKey(), entry.getValue());
    }
  }

  public String toString() {
    return avian.Data.toString(this);
  }

  public boolean isEmpty() {
    return size() == 0;
  }

  public int size() {
    return size;
  }

  private void grow() {
    if (array == null || size >= array.length * 2) {
      resize(array == null ? MinimumCapacity : array.length * 2);
    }
  }

  private void shrink() {
    if (array.length / 2 >= MinimumCapacity && size <= array.length / 3) {
      resize(array.length / 2);
    }
  }

  private void resize(int capacity) {
    Cell<K, V>[] newArray = null;
    if (capacity != 0) {
      capacity = Data.nextPowerOfTwo(capacity);
      if (array != null && array.length == capacity) {
        return;
      }

      newArray = new Cell[capacity];
      if (array != null) {
        for (int i = 0; i < array.length; ++i) {
          Cell<K, V> next;
          for (Cell<K, V> c = array[i]; c != null; c = next) {
            next = c.next();
            int index = c.hashCode() & (capacity - 1);
            c.setNext(newArray[index]);
            newArray[index] = c;
          }
        }
      }
    }
    array = newArray;
  }

  protected Cell<K, V> find(Object key) {
    if (array == null) return null;
    if (plain) return findPlain(key);
    int index = helper.hash(key) & (array.length - 1);
    for (Cell<K, V> c = array[index]; c != null; c = c.next()) {
      if (helper.equal(key, c.getKey())) {
        return c;
      }
    }
    return null;
  }

  // MyCell fields, no per-step interface calls. Only for helpers that
  // did not override hash or equality and whose cells are MyCells.
  private MyCell<K, V> findPlain(Object key) {
    int hash = key == null ? 0 : key.hashCode();
    int index = hash & (array.length - 1);
    for (MyCell<K, V> c = (MyCell<K, V>) array[index]; c != null;
         c = (MyCell<K, V>) c.next) {
      if (c.hashCode == hash) {
        K stored = c.key;
        if (stored == key || (key != null && key.equals(stored))) return c;
      }
    }
    return null;
  }

  private void insert(Cell<K, V> cell) {
    ++ size;

    grow();

    int index = cell.hashCode() & (array.length - 1);
    cell.setNext(array[index]);
    array[index] = cell;
    afterInsert(cell);
  }

  protected void afterInsert(Cell<K, V> cell) { }

  protected void afterRemove(Cell<K, V> cell) { }

  protected void afterClear() { }

  public void remove(Cell<K, V> cell) {
    int index = cell.hashCode() & (array.length - 1);
    Cell<K, V> p = null;
    for (Cell<K, V> c = array[index]; c != null; c = c.next()) {
      if (c == cell) {
        if (p == null) {
          array[index] = c.next();
        } else {
          p.setNext(c.next());
        }
        -- size;
        afterRemove(cell);
        break;
      }
    }

    shrink();
  }

  private Cell<K, V> putCell(K key, V value) {
    Cell<K, V> c = find(key);
    if (c == null) {
      insert(helper.make(key, value, null));
    } else {
      c.setValue(value);
    }
    return c;
  }

  public boolean containsKey(Object key) {
    return find(key) != null;
  }

  public boolean containsValue(Object value) {
    if (array != null) {
      for (int i = 0; i < array.length; ++i) {
        for (Cell<K, V> c = array[i]; c != null; c = c.next()) {
          if (helper.equal(value, c.getValue())) {
            return true;
          }
        }
      }
    }  
	
    return false;
  }

  public V get(Object key) {
    if (plain && array != null) {
      MyCell<K, V> plain = findPlain(key);
      return plain == null ? null : plain.value;
    }
    Cell<K, V> c = find(key);
    return (c == null ? null : c.getValue());
  }

  public Cell<K, V> removeCell(Object key) {
    Cell<K, V> old = null;
    if (array != null) {
      int index = helper.hash(key) & (array.length - 1);
      Cell<K, V> p = null;
      for (Cell<K, V> c = array[index]; c != null; c = c.next()) {
        if (helper.equal(key, c.getKey())) {
          old = c;
          if (p == null) {
            array[index] = c.next();
          } else {
            p.setNext(c.next());
          }
          -- size;
          afterRemove(old);
          break;
        }
        p = c;
      }

      shrink();
    }
    return old;
  }

  public V put(K key, V value) {
    if (plain) {
      MyCell<K, V> plain = array == null ? null : findPlain(key);
      if (plain == null) {
        insert(helper.make(key, value, null));
        return null;
      }
      V old = plain.value;
      plain.value = value;
      return old;
    }
    Cell<K, V> c = find(key);
    if (c == null) {
      insert(helper.make(key, value, null));
      return null;
    } else {
      V old = c.getValue();
      c.setValue(value);
      return old;
    }
  }

  public void putAll(Map<? extends K,? extends V> elts) {
    for (Map.Entry<? extends K, ? extends V> entry : elts.entrySet()) {
      put(entry.getKey(), entry.getValue());
    }
  }

  public V remove(Object key) {
    Cell<K, V> c = removeCell((K)key);
    return (c == null ? null : c.getValue());
  }

  public void clear() {
    array = null;
    size = 0;
    afterClear();
  }

  public Set<Entry<K, V>> entrySet() {
    return new Data.EntrySet(new MyEntryMap());
  }

  public Set<K> keySet() {
    return new Data.KeySet(new MyEntryMap());
  }

  public Collection<V> values() {
    return new Data.Values(new MyEntryMap());
  }

  public void forEach(BiConsumer<? super K, ? super V> action) {
    for (Entry<K, V> entry : entrySet()) action.accept(entry.getKey(), entry.getValue());
  }

  Iterator<Entry<K, V>> iterator() {
    return new MyIterator();
  }

  private class MyEntryMap implements Data.EntryMap<K, V> {
    public int size() {
      return HashMap.this.size();
    }

    public Entry<K,V> find(Object key) {
      return HashMap.this.find(key);
    }

    public Entry<K,V> remove(Object key) {
      return removeCell(key);
    }

    public void clear() {
      HashMap.this.clear();
    }

    public Iterator<Entry<K,V>> iterator() {
      return HashMap.this.iterator();
    }
  }

  interface Cell<K, V> extends Entry<K, V> {
    public HashMap.Cell<K, V> next();

    public void setNext(HashMap.Cell<K, V> next);
  }

  interface Helper<K, V> {
    public Cell<K, V> make(K key, V value, Cell<K, V> next);
    
    public int hash(K key);

    public boolean equal(K a, K b);

    // True when make() returns a MyCell and hash/equal are the defaults.
    public boolean directCells();
  }

  static class MyCell<K, V> implements Cell<K, V> {
    public final K key;
    public V value;
    public Cell<K, V> next;
    public int hashCode;

    public MyCell(K key, V value, Cell<K, V> next, int hashCode) {
      this.key = key;
      this.value = value;
      this.next = next;
      this.hashCode = hashCode;
    }

    public K getKey() {
      return key;
    }

    public V getValue() {
      return value;
    }

    public V setValue(V value) {
      V old = this.value;
      this.value = value;
      return old;
    }

    public HashMap.Cell<K, V> next() {
      return next;
    }

    public void setNext(HashMap.Cell<K, V> next) {
      this.next = next;
    }

    public int hashCode() {
      return hashCode;
    }
  }

  static class MyHelper<K, V> implements Helper<K, V> {
    public Cell<K, V> make(K key, V value, Cell<K, V> next) {
      return new MyCell(key, value, next, hash(key));
    }

    public int hash(K a) {
      return (a == null ? 0 : a.hashCode());
    }

    public boolean equal(K a, K b) {
      return (a == null && b == null) || (a != null && a.equals(b));
    }

    public boolean directCells() {
      return true;
    }
  }

  private class MyIterator implements Iterator<Entry<K, V>> {
    private int currentIndex = -1;
    private int nextIndex = -1;
    private Cell<K, V> previousCell;
    private Cell<K, V> currentCell;
    private Cell<K, V> nextCell;

    public MyIterator() {
      hasNext();
    }

    public Entry<K, V> next() {
      if (hasNext()) {
        if (currentCell != null) {
          if (currentCell.next() != null) {
            previousCell = currentCell;
          } else {
            previousCell = null;
          }
        }

        currentCell = nextCell;
        currentIndex = nextIndex;

        nextCell = nextCell.next();

        return currentCell;
      } else {
        throw new NoSuchElementException();
      }
    }

    public boolean hasNext() {
      if (array != null) {
        while (nextCell == null && ++ nextIndex < array.length) {
          if (array[nextIndex] != null) {
            nextCell = array[nextIndex];
            return true;
          }
        }
      }
      return nextCell != null;
    }

    public void remove() {
      if (currentCell != null) {
        if (previousCell == null) {
          array[currentIndex] = currentCell.next();
        } else {
          previousCell.setNext(currentCell.next());
          if (previousCell.next() == null) {
            previousCell = null;
          }
        }
        afterRemove(currentCell);
        currentCell = null;
        -- size;
      } else {
        throw new IllegalStateException();
      }
    }
  }
}
