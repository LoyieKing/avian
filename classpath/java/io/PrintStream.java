/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */
package java.io;

public class PrintStream extends OutputStream {
  private final OutputStream out;
  private final boolean autoFlush;
  // Protects only the shared OutputStream. Encoding happens before this
  // lock is taken, so a line's allocation and UTF-8 conversion do not
  // keep every other printer parked, and do not run while this monitor
  // is held across a safepoint.
  private final Object bufferLock = new Object();

  private static class Static {
    private static final byte[] newline
      = System.getProperty("line.separator").getBytes();
  }

  public PrintStream(OutputStream out, boolean autoFlush) {
    this.out = out;
    this.autoFlush = autoFlush;
  }

  public PrintStream(OutputStream out, boolean autoFlush, String encoding)
    throws UnsupportedEncodingException
  {
    this.out = out;
    this.autoFlush = autoFlush;

    if (! (encoding.equals("UTF-8") || encoding.equals("ISO-8859-1"))) {
      throw new UnsupportedEncodingException(encoding);
    }
  }

  public PrintStream(OutputStream out) {
    this(out, false);
  }

  private static byte[] withNewline(byte[] text) {
    byte[] nl = Static.newline;
    byte[] line = new byte[text.length + nl.length];
    System.arraycopy(text, 0, line, 0, text.length);
    System.arraycopy(nl, 0, line, text.length, nl.length);
    return line;
  }

  private void writeLocked(byte[] b) throws IOException {
    out.write(b);
  }

  private void flushLocked() {
    try {
      out.flush();
    } catch (IOException e) { }
  }

  public void print(String s) {
    byte[] text = s.getBytes();
    synchronized (bufferLock) {
      try {
        writeLocked(text);
        if (autoFlush) flushLocked();
      } catch (IOException e) { }
    }
  }

  public void print(Object o) {
    print(String.valueOf(o));
  }

  public void print(boolean v) {
    print(String.valueOf(v));
  }

  public void print(char c) {
    print(String.valueOf(c));
  }

  public void print(int v) {
    print(String.valueOf(v));
  }

  public void print(long v) {
    print(String.valueOf(v));
  }

  public void print(float v) {
    print(String.valueOf(v));
  }

  public void print(double v) {
    print(String.valueOf(v));
  }

  public void print(char[] s) {
    print(String.valueOf(s));
  }

  public PrintStream printf(java.util.Locale locale, String format, Object... args) {
    // should this be cached in an instance variable??
    final java.util.Formatter formatter = new java.util.Formatter(this);
    synchronized (bufferLock) {
      formatter.format(locale, format, args);
    }
    return this;
  }

  public PrintStream printf(String format, Object... args) {
    final java.util.Formatter formatter = new java.util.Formatter(this);
    synchronized (bufferLock) {
      formatter.format(format, args);
    }
    return this;
  }

  public PrintStream format(String format, Object... args) {
    return printf(format, args);
  }

  public PrintStream format(java.util.Locale locale, String format, Object... args) {
    return printf(locale, format, args);
  }

  public void println(String s) {
    byte[] line = withNewline(s.getBytes());
    synchronized (bufferLock) {
      try {
        writeLocked(line);
        if (autoFlush) flushLocked();
      } catch (IOException e) { }
    }
  }

  public void println() {
    byte[] nl = Static.newline;
    synchronized (bufferLock) {
      try {
        writeLocked(nl);
        if (autoFlush) flushLocked();
      } catch (IOException e) { }
    }
  }

  public void println(Object o) {
    println(String.valueOf(o));
  }

  public void println(boolean v) {
    println(String.valueOf(v));
  }

  public void println(char c) {
    println(String.valueOf(c));
  }

  public void println(int v) {
    println(String.valueOf(v));
  }

  public void println(long v) {
    println(String.valueOf(v));
  }

  public void println(float v) {
    println(String.valueOf(v));
  }

  public void println(double v) {
    println(String.valueOf(v));
  }

  public void println(char[] s) {
    println(String.valueOf(s));
  }

  public void write(int c) throws IOException {
    synchronized (bufferLock) {
      out.write(c);
      if (autoFlush && c == '\n') flushLocked();
    }
  }

  public void write(byte[] buffer, int offset, int length) throws IOException {
    synchronized (bufferLock) {
      out.write(buffer, offset, length);
      if (autoFlush) flushLocked();
    }
  }

  public void flush() {
    synchronized (bufferLock) {
      flushLocked();
    }
  }

  public void close() {
    synchronized (bufferLock) {
      try {
        out.close();
      } catch (IOException e) { }
    }
  }
}
