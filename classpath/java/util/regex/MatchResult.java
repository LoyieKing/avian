/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.regex;

public interface MatchResult {
  int start();

  int start(int group);

  int end();

  int end(int group);

  String group();

  String group(int group);

  int groupCount();
}
