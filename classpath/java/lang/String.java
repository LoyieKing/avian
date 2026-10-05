/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.lang;

import java.io.Serializable;
import java.io.UnsupportedEncodingException;
import java.util.Comparator;
import java.util.Formatter;
import java.util.Locale;
import java.util.regex.Pattern;

public final class String
  implements Comparable<String>, CharSequence, Serializable
{
  private static final String UTF_8_ENCODING = "UTF-8";
  private static final String ISO_8859_1_ENCODING = "ISO-8859-1";
  private static final String LATIN_1_ENCODING = "LATIN-1";
  private static final String DEFAULT_ENCODING = UTF_8_ENCODING;

  public static Comparator<String> CASE_INSENSITIVE_ORDER
    = new Comparator<String>() {
    @Override
    public int compare(String a, String b) {
      return a.compareToIgnoreCase(b);
    }
  };

  // Modified UTF-8 payload. Exactly one of data and unsafe_data is live.
  // unsafe_data is the address of a u2 big-endian byte length followed by
  // that many Modified UTF-8 bytes, in memory the GC does not move.
  private final byte[] data;
  private final long unsafe_data;
  private final int length;
  private int hashCode;

  private static final class Encoded {
    final byte[] data;
    final int length;

    Encoded(byte[] data, int length) {
      this.data = data;
      this.length = length;
    }
  }

  private String(Encoded encoded) {
    this.data = encoded.data;
    this.unsafe_data = 0;
    this.length = encoded.length;
    this.hashCode = 0;
  }

  public String() {
    this(new Encoded(new byte[0], 0));
  }

  public String(char[] data, int offset, int length, boolean copy) {
    this(encode(data, offset, length));
  }

  public String(char[] data, int offset, int length) {
    this(data, offset, length, true);
  }

  public String(char[] data) {
    this(data, 0, data.length);
  }

  public String(byte bytes[], int offset, int length, String charsetName)
    throws UnsupportedEncodingException
  {
    this(decodeCharset(bytes, offset, length, charsetName));
  }

  // bytes are Modified UTF-8. copy == false shares the array when the
  // range is the whole array, or the whole array except a trailing 0.
  public String(byte[] data, int offset, int length, boolean copy) {
    this(copyMutf8(data, offset, length, copy));
  }

  public String(byte[] data, int offset, int length) {
    this(data, offset, length, true);
  }

  public String(byte[] data) {
    this(decodeKnown(data, 0, data.length, DEFAULT_ENCODING));
  }

  public String(String s) {
    this.data = s.data;
    this.unsafe_data = s.unsafe_data;
    this.length = s.length;
    this.hashCode = s.hashCode;
  }

  public String(byte[] data, String charset)
    throws UnsupportedEncodingException
  {
    this(data, 0, data.length, charset);
  }

  public String(byte[] data, java.nio.charset.Charset charset) {
    this(data, 0, data.length, charset);
  }

  public String(byte[] data, int offset, int length,
                java.nio.charset.Charset charset)
  {
    this(decodeCharsetObject(data, offset, length, charset));
  }

  public String(byte bytes[], int highByte, int offset, int length) {
    this(encodeHigh(bytes, highByte, offset, length));
  }

  @Override
  public String toString() {
    return this;
  }

  @Override
  public int length() {
    return length;
  }

  @Override
  public int hashCode() {
    int h = hashCode;
    if (h == 0 && length != 0) {
      if (data == null) {
        h = unsafeHash(this);
      } else {
        byte[] b = data;
        int n = payload(b);
        if (n == length) {
          for (int i = 0; i < n; ++i) {
            h = (h * 31) + (b[i] & 0xff);
          }
        } else {
          Seq seq = new Seq(b);
          for (int i = 0; i < length; ++i) {
            h = (h * 31) + seq.next();
          }
        }
      }
      hashCode = h;
    }
    return h;
  }

  @Override
  public boolean equals(Object o) {
    if (this == o) {
      return true;
    } else if (o instanceof String) {
      String s = (String) o;
      if (s.length != length) {
        return false;
      }
      if (data != null && s.data != null) {
        byte[] a = data;
        byte[] b = s.data;
        int n = payload(a);
        if (n != payload(b)) {
          return false;
        }
        for (int i = 0; i < n; ++i) {
          if (a[i] != b[i]) {
            return false;
          }
        }
        return true;
      }
      return unsafeEquals(this, s);
    } else {
      return false;
    }
  }

  public boolean equalsIgnoreCase(String s) {
    if (this == s) {
      return true;
    } else {
      return s != null && s.length == length && compareToIgnoreCase(s) == 0;
    }
  }

  public boolean contentEquals(CharSequence cs) {
    if (cs.length() != length) return false;
    Seq seq = new Seq(bytes());
    for (int i = 0; i < length; ++i) {
      if (seq.next() != cs.charAt(i)) return false;
    }
    return true;
  }

  @Override
  public int compareTo(String s) {
    if (this == s) return 0;

    // Unmanaged headers are not Java arrays. Scan them in place.
    if (data == null && unsafeByteLength(unsafe_data) == length) {
      if (s.data != null && payload(s.data) == s.length) {
        return unsafeCompare(unsafe_data, s.data, s.length);
      }
      if (s.data == null && unsafeByteLength(s.unsafe_data) == s.length) {
        return unsafeCompareHeader(unsafe_data, s.unsafe_data);
      }
    } else if (s.data == null && data != null && payload(data) == length
               && unsafeByteLength(s.unsafe_data) == s.length) {
      return -unsafeCompare(s.unsafe_data, data, length);
    }

    byte[] a = bytes();
    byte[] b = s.bytes();
    int n = length < s.length ? length : s.length;
    if (payload(a) == length && payload(b) == s.length) {
      int m = n;
      for (int i = 0; i < m; ++i) {
        int d = (a[i] & 0xff) - (b[i] & 0xff);
        if (d != 0) return d;
      }
      return length - s.length;
    }

    Seq sa = new Seq(a);
    Seq sb = new Seq(b);
    for (int i = 0; i < n; ++i) {
      int d = sa.next() - sb.next();
      if (d != 0) return d;
    }
    return length - s.length;
  }

  public int compareToIgnoreCase(String s) {
    if (this == s) return 0;

    int n = length < s.length ? length : s.length;
    Seq sa = new Seq(bytes());
    Seq sb = new Seq(s.bytes());
    for (int i = 0; i < n; ++i) {
      int d = Character.toLowerCase(sa.next())
        - Character.toLowerCase(sb.next());
      if (d != 0) return d;
    }
    return length - s.length;
  }

  public String trim() {
    byte[] b = bytes();
    Seq seq = new Seq(b);
    int start = -1;
    int startByte = 0;
    for (int i = 0; i < length; ++i) {
      int at = seq.i;
      char c = seq.next();
      if (! Character.isWhitespace(c)) {
        start = i;
        startByte = at;
        break;
      }
    }

    if (start < 0) return "";

    int end = length;
    int endByte = payload(b);
    if (endByte == length) {
      for (int i = length - 1; i >= start; --i) {
        if (! Character.isWhitespace((char) (b[i] & 0xff))) {
          end = i + 1;
          endByte = end;
          break;
        }
      }
    } else {
      int[] ends = new int[length + 1];
      ends[0] = 0;
      Seq walk = new Seq(b);
      for (int i = 0; i < length; ++i) {
        walk.next();
        ends[i + 1] = walk.i;
      }
      for (int i = length - 1; i >= start; --i) {
        Seq one = new Seq(b);
        one.i = ends[i];
        if (! Character.isWhitespace(one.next())) {
          end = i + 1;
          endByte = ends[end];
          startByte = ends[start];
          break;
        }
      }
    }

    if (start >= end) return "";
    if (start == 0 && end == length) return this;
    return slice(b, startByte, endByte, end - start);
  }

  public String toLowerCase() {
    return changeCase(false);
  }

  public String toUpperCase() {
    return changeCase(true);
  }

  private String changeCase(boolean upper) {
    byte[] b = bytes();
    Seq seq = new Seq(b);
    for (int i = 0; i < length; ++i) {
      char ch = seq.next();
      char mapped = upper ? Character.toUpperCase(ch) : Character.toLowerCase(ch);
      if (mapped != ch) {
        char[] chars = new char[length];
        Seq again = new Seq(b);
        for (int j = 0; j < length; ++j) {
          char c = again.next();
          chars[j] = upper ? Character.toUpperCase(c) : Character.toLowerCase(c);
        }
        return new String(chars, 0, length, false);
      }
    }
    return this;
  }

  public int indexOf(int c) {
    return indexOf(c, 0);
  }

  public int indexOf(int c, int start) {
    if (start < 0) start = 0;
    if (c < 0 || c > 0xffff) return -1;
    if (data == null && unsafeByteLength(unsafe_data) == length) {
      if (c >= 0x80) return -1;
      return unsafeIndexOfByte(unsafe_data, start, c);
    }
    byte[] b = bytes();
    if (payload(b) == length && c < 0x80) {
      for (int i = start; i < length; ++i) {
        if ((b[i] & 0xff) == c) return i;
      }
      return -1;
    }
    Seq seq = new Seq(b);
    for (int i = 0; i < start && i < length; ++i) seq.next();
    for (int i = start; i < length; ++i) {
      if (seq.next() == c) return i;
    }
    return -1;
  }

  public int lastIndexOf(int ch) {
    return lastIndexOf(ch, length - 1);
  }

  public int indexOf(String s) {
    return indexOf(s, 0);
  }

  public int indexOf(String s, int start) {
    if (s.length == 0) return start;
    if (start < 0) start = 0;
    if (start > length - s.length) return -1;

    if (data == null && unsafeByteLength(unsafe_data) == length) {
      if (s.data != null && payload(s.data) == s.length) {
        return unsafeIndexOf(unsafe_data, start, s.data, s.length);
      }
      if (s.data == null && unsafeByteLength(s.unsafe_data) == s.length) {
        return unsafeIndexOfHeader(unsafe_data, start, s.unsafe_data);
      }
    }

    byte[] a = bytes();
    byte[] b = s.bytes();
    if (payload(a) == length && payload(b) == s.length) {
      int last = length - s.length;
      for (int i = start; i <= last; ++i) {
        int j = 0;
        for (; j < s.length; ++j) {
          if (a[i + j] != b[j]) break;
        }
        if (j == s.length) return i;
      }
      return -1;
    }

    for (int i = start; i <= length - s.length; ++i) {
      if (regionEquals(a, i, b, s.length)) return i;
    }
    return -1;
  }

  public int lastIndexOf(String s) {
    return lastIndexOf(s, length - s.length);
  }

  public int lastIndexOf(String s, int lastIndex) {
    if (s.length == 0) return lastIndex;
    int i = length - s.length;
    if (lastIndex < i) i = lastIndex;
    if (data == null && unsafeByteLength(unsafe_data) == length) {
      if (s.data != null && payload(s.data) == s.length) {
        return unsafeLastIndexOf(unsafe_data, i, s.data, s.length);
      }
      if (s.data == null && unsafeByteLength(s.unsafe_data) == s.length) {
        return unsafeLastIndexOfHeader(unsafe_data, i, s.unsafe_data);
      }
    }
    byte[] a = bytes();
    byte[] b = s.bytes();
    for (; i >= 0; --i) {
      if (regionEquals(a, i, b, s.length)) return i;
    }
    return -1;
  }

  public String replace(char oldChar, char newChar) {
    if (oldChar == newChar) return this;
    if (data == null && unsafeByteLength(unsafe_data) == length) {
      if (oldChar == 0 || oldChar >= 0x80) return this;
      if (newChar != 0 && newChar < 0x80) {
        if (unsafeIndexOfByte(unsafe_data, 0, oldChar) < 0) return this;
        byte[] buf = unsafeCopy(unsafe_data, 0, length);
        byte oldByte = (byte) oldChar;
        byte newByte = (byte) newChar;
        for (int i = 0; i < length; ++i) {
          if (buf[i] == oldByte) buf[i] = newByte;
        }
        return new String(new Encoded(buf, length));
      }
    }
    byte[] b = bytes();
    if (payload(b) == length && oldChar != 0 && newChar != 0
        && oldChar < 0x80 && newChar < 0x80) {
      byte[] buf = new byte[length];
      byte oldByte = (byte) oldChar;
      byte newByte = (byte) newChar;
      boolean changed = false;
      for (int i = 0; i < length; ++i) {
        if (b[i] == oldByte) {
          buf[i] = newByte;
          changed = true;
        } else {
          buf[i] = b[i];
        }
      }
      if (! changed) return this;
      return new String(new Encoded(buf, length));
    }

    char[] chars = new char[length];
    Seq seq = new Seq(b);
    boolean changed = false;
    for (int i = 0; i < length; ++i) {
      char c = seq.next();
      if (c == oldChar) {
        chars[i] = newChar;
        changed = true;
      } else {
        chars[i] = c;
      }
    }
    if (! changed) return this;
    return new String(chars, 0, length, false);
  }

  public String substring(int start) {
    return substring(start, length);
  }

  public String substring(int start, int end) {
    if (start < 0)
      throw new StringIndexOutOfBoundsException(start);
    else if (end > length)
      throw new StringIndexOutOfBoundsException(end);
    int newLen = end - start;
    if (newLen < 0)
      throw new StringIndexOutOfBoundsException(newLen);

    if (start == 0 && end == length)
      return this;
    else if (newLen == 0)
      return "";

    if (data == null && unsafeByteLength(unsafe_data) == length) {
      return new String(new Encoded(unsafeCopy(unsafe_data, start, newLen), newLen));
    }

    byte[] b = bytes();
    if (payload(b) == length) {
      return slice(b, start, end, newLen);
    }
    Seq seq = new Seq(b);
    for (int i = 0; i < start; ++i) seq.next();
    int from = seq.i;
    for (int i = start; i < end; ++i) seq.next();
    return slice(b, from, seq.i, newLen);
  }

  public boolean startsWith(String s) {
    return startsWith(s, 0);
  }

  public boolean startsWith(String s, int start) {
    if (start < 0 || (long) start > (long) length - s.length) {
      return false;
    }
    if (data == null && unsafeByteLength(unsafe_data) == length) {
      if (s.data != null && payload(s.data) == s.length) {
        return unsafeStarts(unsafe_data, start, s.data, s.length);
      }
      if (s.data == null && unsafeByteLength(s.unsafe_data) == s.length) {
        return unsafeStartsHeader(unsafe_data, start, s.unsafe_data);
      }
    }
    return regionEquals(bytes(), start, s.bytes(), s.length);
  }

  public boolean endsWith(String s) {
    if (length < s.length) return false;
    return startsWith(s, length - s.length);
  }

  public String concat(String s) {
    if (s.length() == 0) {
      return this;
    } else {
      return this + s;
    }
  }

  public void getBytes(int srcOffset, int srcLength, byte[] dst, int dstOffset)
  {
    if (srcOffset < 0)
      throw new StringIndexOutOfBoundsException(srcOffset);
    else if (srcOffset + srcLength > length)
      throw new StringIndexOutOfBoundsException(srcOffset + srcLength);
    else if (srcLength < 0)
      throw new StringIndexOutOfBoundsException(srcLength);

    byte[] b = bytes();
    if (payload(b) == length) {
      if (srcLength > 0) System.arraycopy(b, srcOffset, dst, dstOffset, srcLength);
      return;
    }
    Seq seq = new Seq(b);
    for (int i = 0; i < srcOffset; ++i) seq.next();
    for (int i = 0; i < srcLength; ++i) {
      dst[dstOffset + i] = (byte) seq.next();
    }
  }

  public byte[] getBytes() {
    byte[] latin = latin1();
    if (latin != null) {
      byte[] out = new byte[length];
      if (length > 0) System.arraycopy(latin, 0, out, 0, length);
      return out;
    }
    try {
      return getBytes(DEFAULT_ENCODING);
    } catch (java.io.UnsupportedEncodingException ex) {
      throw new RuntimeException(
        "Default '" + DEFAULT_ENCODING + "' encoding not handled", ex);
    }
  }

  public byte[] getBytes(String format)
    throws java.io.UnsupportedEncodingException
  {
    String fmt = format.trim().toUpperCase();
    // One byte per char is already UTF-8 and Latin-1. Copy it once.
    if (data == null && unsafeByteLength(unsafe_data) == length
        && (fmt.equals(DEFAULT_ENCODING) || fmt.equals(ISO_8859_1_ENCODING)
            || fmt.equals(LATIN_1_ENCODING) || fmt.equals("US-ASCII")
            || fmt.equals("ASCII"))) {
      return unsafeBytes(unsafe_data);
    }
    byte[] b = bytes();
    int n = payload(b);
    if (DEFAULT_ENCODING.equals(fmt)) {
      if (isUtf8Compatible(b)) {
        byte[] out = new byte[n];
        System.arraycopy(b, 0, out, 0, n);
        return out;
      }
      return encodeUtf8(b);
    } else if (ISO_8859_1_ENCODING.equals(fmt) || LATIN_1_ENCODING.equals(fmt)
               || "US-ASCII".equals(fmt) || "ASCII".equals(fmt)) {
      byte[] out = new byte[length];
      if (n == length) {
        System.arraycopy(b, 0, out, 0, length);
      } else {
        Seq seq = new Seq(b);
        for (int i = 0; i < length; ++i) out[i] = (byte) seq.next();
      }
      return out;
    } else if ("UTF-16BE".equals(fmt) || "UTF-16LE".equals(fmt)
               || "UTF-16".equals(fmt)) {
      boolean little = "UTF-16LE".equals(fmt);
      boolean bom = "UTF-16".equals(fmt);
      byte[] out = new byte[length * 2 + (bom ? 2 : 0)];
      int p = 0;
      if (bom) {
        out[p++] = (byte) 0xFE;
        out[p++] = (byte) 0xFF;
      }
      Seq seq = new Seq(b);
      for (int i = 0; i < length; ++i) {
        char c = seq.next();
        if (little) {
          out[p++] = (byte) c;
          out[p++] = (byte) (c >>> 8);
        } else {
          out[p++] = (byte) (c >>> 8);
          out[p++] = (byte) c;
        }
      }
      return out;
    } else {
      throw new java.io.UnsupportedEncodingException(
        "Encoding " + format + " not supported");
    }
  }

  public byte[] getBytes(java.nio.charset.Charset charset) {
    if (charset == null) throw new NullPointerException();
    try {
      return getBytes(charset.name());
    } catch (java.io.UnsupportedEncodingException e) {
      throw new java.nio.charset.UnsupportedCharsetException(charset.name());
    }
  }

  public void getChars(int srcOffset, int srcEnd, char[] dst, int dstOffset)
  {
    if (srcOffset < 0)
      throw new StringIndexOutOfBoundsException(srcOffset);
    else if (srcEnd > length)
      throw new StringIndexOutOfBoundsException(srcEnd);

    int srcLength = srcEnd - srcOffset;
    if (data == null) {
      copyChars(srcOffset, srcLength, dst, dstOffset);
      return;
    }
    byte[] b = bytes();
    if (payload(b) == length) {
      for (int i = 0; i < srcLength; ++i) {
        dst[dstOffset + i] = (char) (b[srcOffset + i] & 0xff);
      }
      return;
    }
    Seq seq = new Seq(b);
    for (int i = 0; i < srcOffset; ++i) seq.next();
    for (int i = 0; i < srcLength; ++i) dst[dstOffset + i] = seq.next();
  }

  public char[] toCharArray() {
    char[] b = new char[length];
    getChars(0, length, b, 0);
    return b;
  }

  @Override
  public char charAt(int index) {
    if (index < 0 || index >= length) {
      throw new StringIndexOutOfBoundsException(index);
    }
    if (data != null) {
      if (payload(data) == length) return (char) (data[index] & 0xff);
      return charAt(data, index);
    }
    if (unsafeByteLength(unsafe_data) == length) {
      return (char) (unsafeByte(unsafe_data, index) & 0xff);
    }
    return charAt(bytes(), index);
  }

  public String[] split(String regex) {
    return split(regex, 0);
  }

  public String[] split(String regex, int limit) {
    return Pattern.compile(regex).split(this, limit);
  }

  @Override
  public CharSequence subSequence(int start, int end) {
    return substring(start, end);
  }

  public boolean matches(String regex) {
    return Pattern.matches(regex, this);
  }

  public String replaceFirst(String regex, String replacement) {
    return Pattern.compile(regex).matcher(this).replaceFirst(replacement);
  }

  public String replaceAll(String regex, String replacement) {
    return Pattern.compile(regex).matcher(this).replaceAll(replacement);
  }

  public String replace(CharSequence target, CharSequence replace) {
    if (target.length() == 0) {
      return this.infuse(replace.toString());
    }

    String targetString = target.toString();
    String replaceString = replace.toString();
    int targetSize = target.length();

    StringBuilder returnValue = new StringBuilder();
    String unhandled = this;

    int index = -1;
    while ((index = unhandled.indexOf(targetString)) != -1) {
      returnValue.append(unhandled.substring(0, index)).append(replaceString);
      unhandled = unhandled.substring(index + targetSize, unhandled.length());
    }

    returnValue.append(unhandled);
    return returnValue.toString();
  }

  private String infuse(String infuseWith) {
    StringBuilder retVal = new StringBuilder();
    for (int i = 0; i < length; i++) {
      retVal.append(infuseWith).append(substring(i, i + 1));
    }
    retVal.append(infuseWith);
    return retVal.toString();
  }

  public native String intern();

  public static String format(String fmt, Object... args) {
    final Formatter formatter = new Formatter();
    final String result = formatter.format(fmt, args).toString();
    formatter.close();
    return result;
  }

  public static String format(Locale l, String fmt, Object... args) {
    final Formatter formatter = new Formatter();
    final String result = formatter.format(l, fmt, args).toString();
    formatter.close();
    return result;
  }

  public static String valueOf(Object s) {
    return s == null ? "null" : s.toString();
  }

  public static String valueOf(boolean v) {
    return Boolean.toString(v);
  }

  public static String valueOf(byte v) {
    return Byte.toString(v);
  }

  public static String valueOf(short v) {
    return Short.toString(v);
  }

  public static String valueOf(char v) {
    return Character.toString(v);
  }

  public static String valueOf(int v) {
    return Integer.toString(v);
  }

  public static String valueOf(long v) {
    return Long.toString(v);
  }

  public static String valueOf(float v) {
    return Float.toString(v);
  }

  public static String valueOf(double v) {
    return Double.toString(v);
  }

  public static String valueOf(char[] data, int offset, int length) {
    return new String(data, offset, length);
  }

  public static String valueOf(char[] data) {
    return valueOf(data, 0, data.length);
  }

  public int lastIndexOf(int ch, int lastIndex) {
    if (ch < 0 || ch > 0xffff) return -1;
    if (lastIndex >= length) lastIndex = length - 1;
    if (data == null && unsafeByteLength(unsafe_data) == length) {
      if (ch >= 0x80) return -1;
      return unsafeLastIndexOfByte(unsafe_data, lastIndex, ch);
    }
    byte[] b = bytes();
    if (payload(b) == length && ch < 0x80) {
      for (int i = lastIndex; i >= 0; --i) {
        if ((b[i] & 0xff) == ch) return i;
      }
      return -1;
    }
    for (int i = lastIndex; i >= 0; --i) {
      if (charAt(b, i) == ch) return i;
    }
    return -1;
  }

  public boolean regionMatches(int thisOffset, String match, int matchOffset,
                               int length)
  {
    return regionMatches(false, thisOffset, match, matchOffset, length);
  }

  public boolean regionMatches(boolean ignoreCase, int thisOffset,
                               String match, int matchOffset, int length)
  {
    if (thisOffset < 0 || matchOffset < 0 || length < 0
        || (long) thisOffset > (long) this.length - length
        || (long) matchOffset > (long) match.length - length) {
      return false;
    }
    String a = substring(thisOffset, thisOffset + length);
    String b = match.substring(matchOffset, matchOffset + length);
    if (ignoreCase) {
      return a.equalsIgnoreCase(b);
    } else {
      return a.equals(b);
    }
  }

  public boolean isEmpty() {
    return length == 0;
  }

  public boolean contains(CharSequence match) {
    return indexOf(match.toString()) != -1;
  }

  public int codePointAt(int offset) {
    return Character.codePointAt(this, offset);
  }

  public int codePointCount(int start, int end) {
    return Character.codePointCount(this, start, end);
  }

  private static boolean simpleCase(Locale locale) {
    return locale == Locale.ENGLISH || locale == Locale.US || locale == Locale.ROOT;
  }

  public String toUpperCase(Locale locale) {
    if (simpleCase(locale)) {
      return toUpperCase();
    } else {
      throw new UnsupportedOperationException("toUpperCase("+locale+')');
    }
  }

  public String toLowerCase(Locale locale) {
    if (simpleCase(locale)) {
      return toLowerCase();
    } else {
      throw new UnsupportedOperationException("toLowerCase("+locale+')');
    }
  }

  private static native byte unsafeByte(long pointer, int index);

  private static native int unsafeByteLength(long pointer);

  private static native byte[] unsafeBytes(long pointer);

  private static native int unsafeIndexOfByte(long header, int from, int b);

  private static native int unsafeLastIndexOfByte(long header, int from, int b);

  private static native int unsafeIndexOf(long header, int from, byte[] needle, int needleLength);

  private static native int unsafeLastIndexOf(long header, int from, byte[] needle, int needleLength);

  private static native int unsafeIndexOfHeader(long header, int from, long needle);

  private static native int unsafeLastIndexOfHeader(long header, int from, long needle);

  private static native int unsafeCompare(long header, byte[] other, int otherLength);

  private static native int unsafeCompareHeader(long header, long other);

  private static native boolean unsafeStarts(long header, int offset, byte[] needle, int needleLength);

  private static native boolean unsafeStartsHeader(long header, int offset, long needle);

  private static native byte[] unsafeCopy(long header, int offset, int count);

  private static native boolean unsafeEquals(String a, String b);

  private static native int unsafeHash(String s);

  private native void copyChars(int srcOffset, int count, char[] dst, int dstOffset);

  private byte[] bytes() {
    if (data != null) return data;
    return unsafeBytes(unsafe_data);
  }

  static String fromAscii(byte[] ascii) {
    return new String(new Encoded(ascii, ascii.length));
  }

  // Modified UTF-8 bytes when every character is one byte, else null.
  // The array may have a trailing 0; callers copy length() bytes.
  byte[] latin1() {
    if (data == null || payload(data) != length) return null;
    return data;
  }

  // Symbol arrays count a trailing 0. A string may share that array.
  // Valid Modified UTF-8 has no raw 0, so the extra byte is not a character.
  private static int payload(byte[] b) {
    int n = b.length;
    if (n > 0 && b[n - 1] == 0) return n - 1;
    return n;
  }

  private static char charAt(byte[] b, int index) {
    int end = payload(b);
    if (end == index || index < 0) {
      throw new StringIndexOutOfBoundsException(index);
    }
    int i = 0;
    int seen = 0;
    while (seen < index) {
      i = skip(b, i, end);
      seen++;
    }
    Seq seq = new Seq(b);
    seq.i = i;
    return seq.next();
  }

  private static String slice(byte[] b, int from, int to, int charLength) {
    byte[] dst = new byte[to - from];
    System.arraycopy(b, from, dst, 0, dst.length);
    return new String(new Encoded(dst, charLength));
  }

  private static boolean regionEquals(byte[] a, int charOffset, byte[] b,
                                      int charLength)
  {
    int aBytes = payload(a);
    int aChars = countChars(a, 0, aBytes);
    if (aBytes == aChars && payload(b) == charLength) {
      for (int i = 0; i < charLength; ++i) {
        if (a[charOffset + i] != b[i]) return false;
      }
      return true;
    }
    Seq sa = new Seq(a);
    for (int i = 0; i < charOffset; ++i) sa.next();
    Seq sb = new Seq(b);
    for (int i = 0; i < charLength; ++i) {
      if (sa.next() != sb.next()) return false;
    }
    return true;
  }

  private static final class Seq {
    final byte[] b;
    int i;

    Seq(byte[] b) {
      this.b = b;
    }

    char next() {
      int a = b[i++] & 0xff;
      if ((a & 0x80) == 0) return (char) a;
      if ((a & 0xe0) == 0xc0) {
        return (char) (((a & 0x1f) << 6) | (b[i++] & 0x3f));
      }
      return (char) (((a & 0x0f) << 12)
                     | ((b[i++] & 0x3f) << 6)
                     | (b[i++] & 0x3f));
    }
  }

  private static int skip(byte[] b, int i, int end) {
    if (i >= end) return end;
    int a = b[i] & 0xff;
    int n = 1;
    if ((a & 0x80) != 0) n = ((a & 0xe0) == 0xc0) ? 2 : 3;
    if (i + n > end) return end;
    return i + n;
  }

  private static int countChars(byte[] b, int offset, int length) {
    int end = offset + length;
    int n = 0;
    for (int i = offset; i < end; i = skip(b, i, end)) n++;
    return n;
  }

  private static void checkRange(int offset, int length, int size) {
    if (offset < 0)
      throw new StringIndexOutOfBoundsException(offset);
    else if (length < 0)
      throw new StringIndexOutOfBoundsException(length);
    else if ((long) offset + length > size)
      throw new StringIndexOutOfBoundsException(offset + length);
  }

  private static Encoded encodeHigh(byte[] bytes, int highByte, int offset,
                                     int length)
  {
    if (bytes == null) throw new NullPointerException();
    checkRange(offset, length, bytes.length);
    char[] chars = new char[length];
    int mask = highByte << 8;
    for (int i = 0; i < length; ++i) {
      chars[i] = (char) ((bytes[offset + i] & 0xFF) | mask);
    }
    return encode(chars, 0, length);
  }

  private static Encoded encode(char[] chars, int offset, int length) {
    if (chars == null) throw new NullPointerException();
    checkRange(offset, length, chars.length);
    int n = 0;
    for (int i = 0; i < length; ++i) {
      char c = chars[offset + i];
      if (c == 0 || c >= 0x80) n += (c >= 0x800 ? 3 : 2);
      else n += 1;
    }
    byte[] out = new byte[n];
    int j = 0;
    for (int i = 0; i < length; ++i) {
      char c = chars[offset + i];
      if (c == 0) {
        out[j++] = (byte) 0xC0;
        out[j++] = (byte) 0x80;
      } else if (c < 0x80) {
        out[j++] = (byte) c;
      } else if (c < 0x800) {
        out[j++] = (byte) (0xC0 | (c >> 6));
        out[j++] = (byte) (0x80 | (c & 0x3f));
      } else {
        out[j++] = (byte) (0xE0 | (c >> 12));
        out[j++] = (byte) (0x80 | ((c >> 6) & 0x3f));
        out[j++] = (byte) (0x80 | (c & 0x3f));
      }
    }
    return new Encoded(out, length);
  }

  private static Encoded copyMutf8(byte[] data, int offset, int length,
                                   boolean copy)
  {
    if (data == null) throw new NullPointerException();
    checkRange(offset, length, data.length);
    int chars = countChars(data, offset, length);
    if (! copy && offset == 0
        && (length == data.length
            || (length + 1 == data.length && data[length] == 0))) {
      return new Encoded(data, chars);
    }
    byte[] b = new byte[length];
    System.arraycopy(data, offset, b, 0, length);
    return new Encoded(b, chars);
  }

  private static boolean knownCharset(String name) {
    return name.equalsIgnoreCase(UTF_8_ENCODING)
      || name.equalsIgnoreCase(ISO_8859_1_ENCODING)
      || name.equalsIgnoreCase(LATIN_1_ENCODING)
      || name.equalsIgnoreCase("US-ASCII")
      || name.equalsIgnoreCase("ASCII");
  }

  private static Encoded decodeCharset(byte[] data, int offset, int length,
                                       String charsetName)
    throws UnsupportedEncodingException
  {
    if (charsetName == null) throw new UnsupportedEncodingException(null);
    if (! knownCharset(charsetName)) {
      throw new UnsupportedEncodingException(charsetName);
    }
    return decodeKnown(data, offset, length, charsetName);
  }

  private static Encoded decodeCharsetObject(byte[] data, int offset, int length,
                                             java.nio.charset.Charset charset)
  {
    if (charset == null) throw new NullPointerException();
    String name = charset.name();
    if (! knownCharset(name)) {
      throw new java.nio.charset.UnsupportedCharsetException(name);
    }
    return decodeKnown(data, offset, length, name);
  }

  private static Encoded decodeKnown(byte[] data, int offset, int length,
                                     String charsetName)
  {
    if (data == null) throw new NullPointerException();
    checkRange(offset, length, data.length);
    if (charsetName.equalsIgnoreCase(ISO_8859_1_ENCODING)
        || charsetName.equalsIgnoreCase(LATIN_1_ENCODING)
        || charsetName.equalsIgnoreCase("US-ASCII")
        || charsetName.equalsIgnoreCase("ASCII")) {
      char[] chars = new char[length];
      for (int i = 0; i < length; ++i) {
        chars[i] = (char) (data[offset + i] & 0xff);
      }
      return encode(chars, 0, length);
    }
    if (isMutf8(data, offset, length)) {
      return copyMutf8(data, offset, length, true);
    }
    return transcodeUtf8(data, offset, length);
  }

  // Modified UTF-8: no raw 0x00 and no 4-byte sequence. C0 80 is accepted.
  private static boolean isMutf8(byte[] b, int offset, int length) {
    int i = offset;
    int end = offset + length;
    while (i < end) {
      int a = b[i] & 0xff;
      if (a == 0) return false;
      if (a < 0x80) {
        i++;
        continue;
      }
      if ((a & 0xe0) == 0xc0) {
        if (i + 1 >= end || (b[i + 1] & 0xc0) != 0x80) return false;
        i += 2;
        continue;
      }
      if ((a & 0xf0) == 0xe0) {
        if (i + 2 >= end || (b[i + 1] & 0xc0) != 0x80
            || (b[i + 2] & 0xc0) != 0x80) return false;
        i += 3;
        continue;
      }
      return false;
    }
    return true;
  }

  private static Encoded transcodeUtf8(byte[] in, int offset, int length) {
    char[] chars = new char[length];
    int n = 0;
    int i = offset;
    int end = offset + length;
    while (i < end) {
      int a = in[i++] & 0xff;
      if (a < 0x80) {
        chars[n++] = (char) a;
        continue;
      }
      if ((a & 0xe0) == 0xc0 && i < end && (in[i] & 0xc0) == 0x80) {
        int b = in[i++] & 0xff;
        chars[n++] = (char) (((a & 0x1f) << 6) | (b & 0x3f));
        continue;
      }
      if ((a & 0xf0) == 0xe0 && i + 1 < end
          && (in[i] & 0xc0) == 0x80 && (in[i + 1] & 0xc0) == 0x80) {
        int b = in[i++] & 0xff;
        int c = in[i++] & 0xff;
        chars[n++] = (char) (((a & 0x0f) << 12) | ((b & 0x3f) << 6) | (c & 0x3f));
        continue;
      }
      if ((a & 0xf8) == 0xf0 && i + 2 < end
          && (in[i] & 0xc0) == 0x80 && (in[i + 1] & 0xc0) == 0x80
          && (in[i + 2] & 0xc0) == 0x80) {
        int b = in[i++] & 0xff;
        int c = in[i++] & 0xff;
        int d = in[i++] & 0xff;
        int cp = ((a & 0x07) << 18) | ((b & 0x3f) << 12)
          | ((c & 0x3f) << 6) | (d & 0x3f);
        if (cp >= 0x10000 && cp <= 0x10ffff) {
          cp -= 0x10000;
          chars[n++] = (char) (0xD800 + (cp >> 10));
          chars[n++] = (char) (0xDC00 + (cp & 0x3ff));
          continue;
        }
      }
      chars[n++] = '\ufffd';
    }
    return encode(chars, 0, n);
  }

  private static boolean isUtf8Compatible(byte[] b) {
    int end = payload(b);
    for (int i = 0; i < end; ) {
      int a = b[i] & 0xff;
      if (a < 0x80) {
        i++;
        continue;
      }
      if ((a & 0xe0) == 0xc0) {
        if (a == 0xc0 && (b[i + 1] & 0xff) == 0x80) return false;
        i += 2;
        continue;
      }
      if (a == 0xed && (b[i + 1] & 0xff) >= 0xa0) return false;
      i += 3;
    }
    return true;
  }

  private static int utf8Size(char c, boolean pair) {
    if (pair) return 4;
    if (c == 0 || c < 0x80) return 1;
    if (c < 0x800) return 2;
    return 3;
  }

  private static byte[] encodeUtf8(byte[] mutf8) {
    int chars = countChars(mutf8, 0, payload(mutf8));
    int n = 0;
    Seq count = new Seq(mutf8);
    for (int i = 0; i < chars; ++i) {
      char c = count.next();
      boolean pair = false;
      if (c >= 0xD800 && c <= 0xDBFF && i + 1 < chars) {
        int saved = count.i;
        char d = count.next();
        if (d >= 0xDC00 && d <= 0xDFFF) {
          pair = true;
          i++;
        } else {
          count.i = saved;
        }
      }
      n += utf8Size(c, pair);
    }
    byte[] out = new byte[n];
    Seq seq = new Seq(mutf8);
    int j = 0;
    for (int i = 0; i < chars; ++i) {
      char c = seq.next();
      if (c >= 0xD800 && c <= 0xDBFF && i + 1 < chars) {
        int saved = seq.i;
        char d = seq.next();
        if (d >= 0xDC00 && d <= 0xDFFF) {
          int cp = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
          out[j++] = (byte) (0xf0 | (cp >> 18));
          out[j++] = (byte) (0x80 | ((cp >> 12) & 0x3f));
          out[j++] = (byte) (0x80 | ((cp >> 6) & 0x3f));
          out[j++] = (byte) (0x80 | (cp & 0x3f));
          i++;
          continue;
        }
        seq.i = saved;
      }
      if (c == 0) {
        out[j++] = 0;
      } else if (c < 0x80) {
        out[j++] = (byte) c;
      } else if (c < 0x800) {
        out[j++] = (byte) (0xc0 | (c >> 6));
        out[j++] = (byte) (0x80 | (c & 0x3f));
      } else {
        out[j++] = (byte) (0xe0 | (c >> 12));
        out[j++] = (byte) (0x80 | ((c >> 6) & 0x3f));
        out[j++] = (byte) (0x80 | (c & 0x3f));
      }
    }
    return out;
  }
}
