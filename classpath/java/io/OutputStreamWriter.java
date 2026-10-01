/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.io;

import avian.Utf8;

public class OutputStreamWriter extends Writer {
  private final OutputStream out;
  private final java.nio.charset.Charset charset;

  public OutputStreamWriter(OutputStream out) {
    this(out, (java.nio.charset.Charset) null);
  }

  public OutputStreamWriter(OutputStream out, java.nio.charset.Charset charset) {
    if (out == null) throw new NullPointerException();
    this.out = out;
    this.charset = charset;
  }

  public OutputStreamWriter(OutputStream out, String charsetName)
      throws UnsupportedEncodingException {
    this(out, charsetName == null ? null : java.nio.charset.Charset.forName(charsetName));
  }
  
  public void write(char[] b, int offset, int length) throws IOException {
    if (charset == null || "UTF-8".equals(charset.name())) {
      out.write(Utf8.encode(b, offset, length));
      return;
    }
    out.write(new String(b, offset, length).getBytes(charset));
  }

  public void flush() throws IOException {
    out.flush();
  }

  public void close() throws IOException {
    out.close();
  }
}
