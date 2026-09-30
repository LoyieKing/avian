/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.io;

public class PushbackInputStream extends FilterInputStream {
  protected byte[] buf;
  protected int pos;

  public PushbackInputStream(InputStream in) {
    this(in, 1);
  }

  public PushbackInputStream(InputStream in, int size) {
    super(in);
    buf = new byte[size];
    pos = size;
  }

  public int read() throws IOException {
    if (pos < buf.length) return buf[pos++] & 0xff;
    return super.read();
  }

  public void unread(int b) throws IOException {
    if (pos == 0) throw new IOException();
    buf[--pos] = (byte) b;
  }

  public void unread(byte[] b, int off, int len) throws IOException {
    if (len > pos) throw new IOException();
    pos -= len;
    System.arraycopy(b, off, buf, pos, len);
  }

  public void unread(byte[] b) throws IOException {
    unread(b, 0, b.length);
  }
}
