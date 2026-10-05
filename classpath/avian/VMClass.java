/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package avian;

public class VMClass {
  public short flags;
  public short vmFlags;
  public short fixedSize;
  public byte arrayElementSize;
  public byte arrayDimensions;
  public VMClass arrayElementClass;
  public int runtimeDataIndex;
  public int[] objectMask;
  public byte[] name;
  public byte[] sourceFile;
  public VMClass super_;
  public Object[] interfaceTable;
  public VMMethod[] virtualTable;
  public VMField[] fieldTable;
  /**
   * Methods declared in this class, plus any abstract virtual methods
   * inherited from implemented or extended interfaces.  If addendum
   * is non-null and addendum.declaredMethodCount is non-negative,
   * then the first addendum.declaredMethodCount methods are declared
   * methods, while the rest are abstract virtual methods.  If
   * addendum is null or addendum.declaredMethodCount is negative, all
   * are declared methods.
   */
  public VMMethod[] methodTable;
  public ClassAddendum addendum;
  public Singleton staticTable;
  public ClassLoader loader;
  public byte[] source;
  // Stub addresses. Primitives, so the garbage collector does not trace them.
  public long serializeThunk;
  public long deserializeThunk;
  // Reflection queries rebuild a Method/Field per member. Callers such as
  // kotlin-reflect repeat the same query, so the member objects are kept and
  // each return is a clone of the array.
  public volatile java.lang.reflect.Method[] cachedDeclaredMethods;
  public volatile java.lang.reflect.Method[] cachedPublicMethods;
  public volatile java.lang.reflect.Field[] cachedDeclaredFields;
  public volatile java.lang.reflect.Field[] cachedPublicFields;
  // Dotted binary name. Class.getName fills this once.
  public volatile String binaryName;
}
