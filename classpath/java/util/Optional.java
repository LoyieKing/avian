/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

public final class Optional<T> {
  private final T value;

  private Optional(T value) {
    this.value = value;
  }

  public static <T> Optional<T> empty() {
    return new Optional<T>(null);
  }

  public static <T> Optional<T> of(T value) {
    if (value == null) throw new NullPointerException();
    return new Optional<T>(value);
  }

  public static <T> Optional<T> ofNullable(T value) {
    return new Optional<T>(value);
  }

  public T get() {
    if (value == null) throw new NoSuchElementException();
    return value;
  }

  public boolean isPresent() {
    return value != null;
  }

  public T orElse(T other) {
    return value != null ? value : other;
  }
}
