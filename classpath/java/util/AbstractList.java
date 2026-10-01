/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

public abstract class AbstractList<T> extends AbstractCollection<T>
  implements List<T>
{
  protected int modCount;

  public boolean add(T o) {
    add(size(), o);
    return true;
  }

  public boolean addAll(Collection<? extends T> c) {
    return addAll(size(), c);
  }

  public boolean addAll(int startIndex, Collection<? extends T> c) {
    if (c == null) {
      throw new NullPointerException("Collection is null");
    }

    int index = startIndex;
    boolean changed = false;

    Iterator<? extends T> it = c.iterator();
    while (it.hasNext()) {
      add(index++, it.next());
      changed = true;
    }

    return changed;
  }

  public Iterator<T> iterator() {
    return listIterator();
  }

  public ListIterator<T> listIterator() {
    return new Collections.ArrayListIterator(this);
  }

  public int indexOf(Object o) {
    int i = 0;
    for (T v: this) {
      if (o == null) {
        if (v == null) {
          return i;
        }
      } else if (o.equals(v)) {
        return i;
      }

      ++ i;
    }
    return -1;
  }

  public int lastIndexOf(Object o) {
    int last = -1;
    int i = 0;
    for (T v : this) {
      if (o == null ? v == null : o.equals(v)) {
        last = i;
      }
      ++i;
    }
    return last;
  }

  public ListIterator<T> listIterator(int index) {
    return new Collections.ArrayListIterator<T>(this, index);
  }

  public List<T> subList(int fromIndex, int toIndex) {
    if (fromIndex < 0 || toIndex > size() || fromIndex > toIndex) {
      throw new IndexOutOfBoundsException(
        "from=" + fromIndex + " to=" + toIndex + " size=" + size());
    }
    return new SubList<T>(this, fromIndex, toIndex);
  }

  private static final class SubList<T> extends AbstractList<T> {
    private final List<T> parent;
    private final int offset;
    private int size;

    SubList(List<T> parent, int fromIndex, int toIndex) {
      this.parent = parent;
      this.offset = fromIndex;
      this.size = toIndex - fromIndex;
    }

    public int size() {
      return size;
    }

    public T get(int index) {
      if (index < 0 || index >= size) {
        throw new IndexOutOfBoundsException(String.valueOf(index));
      }
      return parent.get(offset + index);
    }

    public T set(int index, T value) {
      if (index < 0 || index >= size) {
        throw new IndexOutOfBoundsException(String.valueOf(index));
      }
      return parent.set(offset + index, value);
    }

    public void add(int index, T element) {
      if (index < 0 || index > size) {
        throw new IndexOutOfBoundsException(String.valueOf(index));
      }
      parent.add(offset + index, element);
      size++;
    }

    public T remove(int index) {
      if (index < 0 || index >= size) {
        throw new IndexOutOfBoundsException(String.valueOf(index));
      }
      T removed = parent.remove(offset + index);
      size--;
      return removed;
    }
  }

  public boolean equals(Object o) {
    if (o == this) {
      return true;
    }
    if (!(o instanceof List)) {
      return false;
    }
    ListIterator<T> a = listIterator();
    ListIterator<?> b = ((List<?>) o).listIterator();
    while (a.hasNext() && b.hasNext()) {
      T left = a.next();
      Object right = b.next();
      if (left == null ? right != null : !left.equals(right)) {
        return false;
      }
    }
    return !a.hasNext() && !b.hasNext();
  }

  public int hashCode() {
    int hash = 1;
    for (T element : this) {
      hash = 31 * hash + (element == null ? 0 : element.hashCode());
    }
    return hash;
  }
}
