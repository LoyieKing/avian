/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package avian;

import java.lang.reflect.Array;
import java.lang.reflect.Method;
import java.lang.reflect.InvocationHandler;
import java.util.Arrays;

public class AnnotationInvocationHandler implements InvocationHandler {
  private Object[] data;

  public AnnotationInvocationHandler(Object[] data) {
    this.data = data;
  }

  public Object invoke(Object proxy, Method method, Object[] arguments) {
    String name = method.getName();
    int count = arguments == null ? 0 : arguments.length;
    if (count == 0 && "annotationType".equals(name)) {
      return data[1];
    }
    if (count == 0 && "toString".equals(name)) {
      return annotationToString();
    }
    if (count == 0 && "hashCode".equals(name)) {
      return Integer.valueOf(annotationHashCode());
    }
    if (count == 1 && "equals".equals(name)) {
      return Boolean.valueOf(annotationEquals(arguments[0]));
    }
    for (int i = 2; i < data.length; i += 2) {
      if (name.equals(data[i])) {
        return coerce(data[i + 1], method.getReturnType());
      }
    }
    return coerce(method.getDefaultValue(), method.getReturnType());
  }

  private boolean annotationEquals(Object other) {
    if (!(other instanceof java.lang.annotation.Annotation)) {
      return false;
    }
    if (data[1] != ((java.lang.annotation.Annotation) other).annotationType()) {
      return false;
    }
    try {
      Class type = (Class) data[1];
      Method[] methods = type.getDeclaredMethods();
      for (int i = 0; i < methods.length; ++i) {
        String name = methods[i].getName();
        if ("annotationType".equals(name) || "equals".equals(name)
            || "hashCode".equals(name) || "toString".equals(name)) {
          continue;
        }
        Object mine = value(name, methods[i]);
        Object theirs = methods[i].invoke(other, new Object[0]);
        if (!memberEquals(mine, theirs)) {
          return false;
        }
      }
      return true;
    } catch (RuntimeException e) {
      throw e;
    } catch (Exception e) {
      return false;
    }
  }

  private Object value(String name, Method method) {
    for (int i = 2; i < data.length; i += 2) {
      if (name.equals(data[i])) {
        return coerce(data[i + 1], method.getReturnType());
      }
    }
    return coerce(method.getDefaultValue(), method.getReturnType());
  }

  // Annotation arrays are parsed as Object[] of boxed values. Callers need
  // the member's real array type: int[] is not the same layout as Integer[].
  public static Object coerce(Object value, Class type) {
    if (!(value instanceof Object[]) || type == null || !type.isArray()) {
      return value;
    }
    Object[] raw = (Object[]) value;
    Class component = type.getComponentType();
    if (!component.isPrimitive()) {
      Object[] out = (Object[]) Array.newInstance(component, raw.length);
      for (int i = 0; i < raw.length; ++i) out[i] = raw[i];
      return out;
    }
    if (component == boolean.class) {
      boolean[] out = new boolean[raw.length];
      for (int i = 0; i < raw.length; ++i) out[i] = ((Boolean) raw[i]).booleanValue();
      return out;
    }
    if (component == byte.class) {
      byte[] out = new byte[raw.length];
      for (int i = 0; i < raw.length; ++i) out[i] = ((Byte) raw[i]).byteValue();
      return out;
    }
    if (component == char.class) {
      char[] out = new char[raw.length];
      for (int i = 0; i < raw.length; ++i) out[i] = ((Character) raw[i]).charValue();
      return out;
    }
    if (component == short.class) {
      short[] out = new short[raw.length];
      for (int i = 0; i < raw.length; ++i) out[i] = ((Short) raw[i]).shortValue();
      return out;
    }
    if (component == int.class) {
      int[] out = new int[raw.length];
      for (int i = 0; i < raw.length; ++i) out[i] = ((Integer) raw[i]).intValue();
      return out;
    }
    if (component == long.class) {
      long[] out = new long[raw.length];
      for (int i = 0; i < raw.length; ++i) out[i] = ((Long) raw[i]).longValue();
      return out;
    }
    if (component == float.class) {
      float[] out = new float[raw.length];
      for (int i = 0; i < raw.length; ++i) out[i] = ((Float) raw[i]).floatValue();
      return out;
    }
    if (component == double.class) {
      double[] out = new double[raw.length];
      for (int i = 0; i < raw.length; ++i) out[i] = ((Double) raw[i]).doubleValue();
      return out;
    }
    return value;
  }

  private static boolean memberEquals(Object a, Object b) {
    if (a == b) return true;
    if (a == null || b == null) return false;
    if (a instanceof Object[] && b instanceof Object[]) {
      return Arrays.equals((Object[]) a, (Object[]) b);
    }
    if (a instanceof byte[] && b instanceof byte[]) {
      return Arrays.equals((byte[]) a, (byte[]) b);
    }
    if (a instanceof boolean[] && b instanceof boolean[]) {
      return Arrays.equals((boolean[]) a, (boolean[]) b);
    }
    if (a instanceof char[] && b instanceof char[]) {
      return Arrays.equals((char[]) a, (char[]) b);
    }
    if (a instanceof short[] && b instanceof short[]) {
      return Arrays.equals((short[]) a, (short[]) b);
    }
    if (a instanceof int[] && b instanceof int[]) {
      return Arrays.equals((int[]) a, (int[]) b);
    }
    if (a instanceof long[] && b instanceof long[]) {
      return Arrays.equals((long[]) a, (long[]) b);
    }
    if (a instanceof float[] && b instanceof float[]) {
      return Arrays.equals((float[]) a, (float[]) b);
    }
    if (a instanceof double[] && b instanceof double[]) {
      return Arrays.equals((double[]) a, (double[]) b);
    }
    return a.equals(b);
  }

  private int annotationHashCode() {
    int hash = 0;
    for (int i = 2; i < data.length; i += 2) {
      String name = (String) data[i];
      hash += (127 * name.hashCode()) ^ memberHash(data[i + 1]);
    }
    return hash;
  }

  private static int memberHash(Object value) {
    if (value == null) return 0;
    if (value instanceof Object[]) return Arrays.hashCode((Object[]) value);
    if (value instanceof byte[]) return Arrays.hashCode((byte[]) value);
    if (value instanceof boolean[]) return Arrays.hashCode((boolean[]) value);
    if (value instanceof char[]) return Arrays.hashCode((char[]) value);
    if (value instanceof short[]) return Arrays.hashCode((short[]) value);
    if (value instanceof int[]) return Arrays.hashCode((int[]) value);
    if (value instanceof long[]) return Arrays.hashCode((long[]) value);
    if (value instanceof float[]) return Arrays.hashCode((float[]) value);
    if (value instanceof double[]) return Arrays.hashCode((double[]) value);
    return value.hashCode();
  }

  private String annotationToString() {
    StringBuilder sb = new StringBuilder();
    sb.append('@');
    sb.append(((Class) data[1]).getName());
    sb.append('(');
    for (int i = 2; i < data.length; i += 2) {
      if (i > 2) sb.append(", ");
      sb.append(data[i]);
      sb.append('=');
      sb.append(data[i + 1]);
    }
    sb.append(')');
    return sb.toString();
  }
}
