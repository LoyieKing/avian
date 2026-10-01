/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.lang;

public final class Long extends Number implements Comparable<Long> {
  public static final long MIN_VALUE = -9223372036854775808l;
  public static final long MAX_VALUE =  9223372036854775807l;

  public static final Class TYPE = avian.Classes.forCanonicalName("J");

  private final long value;

  public Long(long value) {
    this.value = value;
  }

  public Long(String s) {
    this.value = parseLong(s);
  }

  public static Long valueOf(String value) {
    return new Long(value);
  }

  public static Long valueOf(long value) {
    return new Long(value);
  }

  public int compareTo(Long o) {
    return value > o.value ? 1 : (value < o.value ? -1 : 0);
  }

  public boolean equals(Object o) {
    return o instanceof Long && ((Long) o).value == value;
  }

  public int hashCode() {
    return hashCode(value);
  }

  public static int hashCode(long value) {
    return (int) (value ^ (value >>> 32));
  }

  public String toString() {
    return String.valueOf(value);
  }

  public static String toString(long v, int radix) {
    if (radix < 1 || radix > 36) {
      throw new IllegalArgumentException("radix " + radix + " not in [1,36]");
    }

    if (v == 0) {
      return "0";
    }

    boolean negative = v < 0;

    int size = (negative ? 1 : 0);
    for (long n = v; n != 0; n /= radix) ++size;

    char[] array = new char[size];

    int i = size - 1;
    for (long n = v; n != 0; n /= radix) {
      long digit = n % radix;
      if (negative) digit = -digit;

      if (digit >= 0 && digit <= 9) {
        array[i] = (char) ('0' + digit);
      } else {
        array[i] = (char) ('a' + (digit - 10));
      }
      --i;
    }

    if (negative) {
      array[i] = '-';
    }

    return new String(array, 0, size, false);
  }

  public static String toString(long v) {
    return toString(v, 10);
  }

  public static String toHexString(long v) {
    return toString(v, 16);
  }

  public static String toOctalString(long v) {
    return toString(v, 8);
  }

  public static String toBinaryString(long v) {
    return toString(v, 2);
  }

  public byte byteValue() {
    return (byte) value;
  }

  public short shortValue() {
    return (short) value;
  }

  public int intValue() {
    return (int) value;
  }

  public long longValue() {
    return value;
  }

  public float floatValue() {
    return (float) value;
  }

  public double doubleValue() {
    return (double) value;
  }

  public static int signum(long v) {
    if (v == 0)     return  0;
    else if (v > 0) return  1;
    else            return -1;
  }

  private static long pow(long a, long b) {
    long c = 1;
    for (int i = 0; i < b; ++i) c *= a;
    return c;
  }

  public static long parseLong(String s) {
    return parseLong(s, 10);
  } 

  public static long parseLong(String s, int radix) {
    int i = 0;
    long number = 0;
    boolean negative = s.startsWith("-");
    int length = s.length();
    if (negative) {
      i = 1;
      -- length;
    }

    long factor = pow(radix, length - 1);
    for (; i < s.length(); ++i) {
      char c = s.charAt(i);
      int digit = Character.digit(c, radix);
      if (digit >= 0) {
        number += digit * factor;
        factor /= radix;
      } else {
        throw new NumberFormatException("invalid character " + c + " code " +
                                        (int) c);
      }
    }

    if (negative) {
      number = -number;
    }

    return number;
  }

  public static int compare(long x, long y) {
    return (x < y) ? -1 : ((x == y) ? 0 : 1);
  }

  public static int compareUnsigned(long x, long y) {
    return compare(x + MIN_VALUE, y + MIN_VALUE);
  }

  public static int numberOfLeadingZeros(long i) {
    int x = (int) (i >>> 32);
    return x == 0
      ? 32 + Integer.numberOfLeadingZeros((int) i)
      : Integer.numberOfLeadingZeros(x);
  }

  public static int numberOfTrailingZeros(long i) {
    int x = (int) i;
    return x == 0
      ? 32 + Integer.numberOfTrailingZeros((int) (i >>> 32))
      : Integer.numberOfTrailingZeros(x);
  }

  public static int bitCount(long i) {
    i = i - ((i >>> 1) & 0x5555555555555555L);
    i = (i & 0x3333333333333333L) + ((i >>> 2) & 0x3333333333333333L);
    i = (i + (i >>> 4)) & 0x0f0f0f0f0f0f0f0fL;
    i = i + (i >>> 8);
    i = i + (i >>> 16);
    i = i + (i >>> 32);
    return (int) i & 0x7f;
  }

  public static long highestOneBit(long i) {
    i |= i >> 1;
    i |= i >> 2;
    i |= i >> 4;
    i |= i >> 8;
    i |= i >> 16;
    i |= i >> 32;
    return i - (i >>> 1);
  }

  public static long lowestOneBit(long i) {
    return i & -i;
  }

  public static long reverseBytes(long i) {
    i = (i & 0x00ff00ff00ff00ffL) << 8 | (i >>> 8) & 0x00ff00ff00ff00ffL;
    return (i << 48) | ((i & 0xffff0000L) << 16)
        | ((i >>> 16) & 0xffff0000L) | (i >>> 48);
  }

  public static long rotateLeft(long i, int distance) {
    return (i << distance) | (i >>> -distance);
  }

  public static long rotateRight(long i, int distance) {
    return (i >>> distance) | (i << -distance);
  }

  public static long divideUnsigned(long dividend, long divisor) {
    if (divisor < 0L) {
      return compareUnsigned(dividend, divisor) < 0 ? 0L : 1L;
    }
    if (dividend >= 0L) {
      return dividend / divisor;
    }
    long quotient = 0L;
    long remainder = 0L;
    for (int bit = 63; bit >= 0; --bit) {
      remainder = (remainder << 1) | ((dividend >>> bit) & 1L);
      if (compareUnsigned(remainder, divisor) >= 0) {
        remainder -= divisor;
        quotient |= 1L << bit;
      }
    }
    return quotient;
  }

  public static long remainderUnsigned(long dividend, long divisor) {
    if (divisor < 0L) {
      return compareUnsigned(dividend, divisor) < 0 ? dividend : dividend - divisor;
    }
    if (dividend >= 0L) {
      return dividend % divisor;
    }
    long remainder = 0L;
    for (int bit = 63; bit >= 0; --bit) {
      remainder = (remainder << 1) | ((dividend >>> bit) & 1L);
      if (compareUnsigned(remainder, divisor) >= 0) {
        remainder -= divisor;
      }
    }
    return remainder;
  }

  public static Long decode(String string) {
    if (string.startsWith("-")) {
      if (string.startsWith("-0") || string.startsWith("-#")) {
        return valueOf(-decode(string.substring(1)).longValue());
      }
    } else if (string.startsWith("0")) {
      char c = string.length() < 2 ? (char) -1 : string.charAt(1);
      if (c == 'x' || c == 'X') {
        return valueOf(parseLong(string.substring(2), 16));
      }
      return valueOf(parseLong(string, 8));
    } else if (string.startsWith("#")) {
      return valueOf(parseLong(string.substring(1), 16));
    }
    return valueOf(parseLong(string, 10));
  }
}
