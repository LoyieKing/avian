/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.nio.charset;

public class Charset {
  private final String canonical;

  protected Charset(String canonicalName, String[] aliases) {
    if (canonicalName == null) throw new IllegalArgumentException("Null charset name");
    this.canonical = canonicalName;
  }

  private Charset(String canonicalName) {
    this(canonicalName, null);
  }

  public static Charset forName(String charsetName) {
    if (charsetName == null) throw new IllegalArgumentException("Null charset name");
    String name = charsetName.trim();
    if (name.length() == 0) throw new IllegalCharsetNameException(charsetName);
    String upper = name.toUpperCase();
    if (listed(upper, "|UTF-8|UTF8|")) return new Charset("UTF-8");
    if (listed(upper, "|ISO-8859-1|ISO8859-1|ISO8859_1|LATIN-1|LATIN1|L1|IBM819|CP819|CSISOLATIN1|")) {
      return new Charset("ISO-8859-1");
    }
    if (listed(upper, "|US-ASCII|ASCII|ANSI_X3.4-1968|CP367|IBM367|ISO646-US|CSASCII|")) {
      return new Charset("US-ASCII");
    }
    if (listed(upper, "|UTF-16|UTF16|")) return new Charset("UTF-16");
    if (listed(upper, "|UTF-16BE|UTF16BE|UNICODEBIG|")) return new Charset("UTF-16BE");
    if (listed(upper, "|UTF-16LE|UTF16LE|UNICODELITTLE|")) return new Charset("UTF-16LE");
    if (listed(upper, "|UTF-32|UTF32|")) return new Charset("UTF-32");
    if (listed(upper, "|UTF-32BE|UTF32BE|")) return new Charset("UTF-32BE");
    if (listed(upper, "|UTF-32LE|UTF32LE|")) return new Charset("UTF-32LE");
    throw new UnsupportedCharsetException(charsetName);
  }

  public static Charset defaultCharset() {
    return forName("UTF-8");
  }

  public static boolean isSupported(String charsetName) {
    try {
      forName(charsetName);
      return true;
    } catch (IllegalArgumentException e) {
      return false;
    }
  }

  public final String name() {
    return canonical;
  }

  public String displayName() {
    return canonical;
  }

  public final boolean equals(Object other) {
    return other instanceof Charset && canonical.equals(((Charset) other).canonical);
  }

  public final int hashCode() {
    return canonical.hashCode();
  }

  public final String toString() {
    return canonical;
  }

  private static boolean listed(String upper, String names) {
    return names.indexOf("|" + upper + "|") >= 0;
  }
}
