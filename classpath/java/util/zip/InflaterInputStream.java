/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.zip;

import java.io.InputStream;
import java.io.IOException;
import java.io.EOFException;

public class InflaterInputStream extends InputStream {
  private final InputStream in;
  private final Inflater inflater;
  private final byte[] buffer;
  // Raw DEFLATE (nowrap) has no trailer. zlib reports it needs another
  // input byte before it will return Z_STREAM_END. ZipFile sets this so a
  // finished entry is not reported as a truncated stream.
  boolean nowrapEofPadding;
  private boolean eofPaddingFed;

  public InflaterInputStream(InputStream in, Inflater inflater, int bufferSize)
  {
    this.in = in;
    this.inflater = inflater;
    this.buffer = new byte[bufferSize];
  }

  public InflaterInputStream(InputStream in, Inflater inflater) {
    this(in, inflater, 4 * 1024);
  }

  public InflaterInputStream(InputStream in) {
    this(in, new Inflater());
  }

  public int read() throws IOException {
    byte[] buffer = new byte[1];
    int c = read(buffer);
    return (c < 0 ? c : (buffer[0] & 0xFF));
  }

  public int read(byte[] b, int offset, int length) throws IOException {
    if (inflater.finished()) {
      return -1;
    }

    while (true) {
      if (inflater.needsInput()) {
        int count = in.read(buffer);
        if (count > 0) {
          inflater.setInput(buffer, 0, count);
        } else if (nowrapEofPadding && !eofPaddingFed) {
          eofPaddingFed = true;
          buffer[0] = 0;
          inflater.setInput(buffer, 0, 1);
        } else {
          throw new EOFException();
        }
      }

      try {
        int count = inflater.inflate(b, offset, length);
        if (count > 0) {
          return count;
        } else if (inflater.needsDictionary()) {
          throw new IOException("missing dictionary");
        } else if (inflater.finished()) {
          return -1;
        }
      } catch (DataFormatException e) {
        throw new IOException(e);
      }
    }
  }

  public int available() throws IOException {
    return inflater.finished() ? 0 : 1;
  }

  public void close() throws IOException {
    in.close();
    inflater.dispose();
  }
}
