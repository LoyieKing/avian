/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.lang.reflect;

import java.lang.annotation.Annotation;

public class Constructor<T> extends AccessibleObject
  implements Member, GenericDeclaration {
  private Method<T> method;

  public Constructor(Method<T> method) {
    this.method = method;
  }

  public boolean equals(Object o) {
    return o instanceof Constructor
      && ((Constructor) o).method.equals(method);
  }

  public boolean isAccessible() {
    return method.isAccessible();
  }

  public void setAccessible(boolean v) {
    method.setAccessible(v);
  }

  public Class<T> getDeclaringClass() {
    return method.getDeclaringClass();
  }

  public Class[] getParameterTypes() {
    return method.getParameterTypes();
  }

  public TypeVariable<?>[] getTypeParameters() {
    return method.getTypeParameters();
  }

  public Type[] getGenericParameterTypes() {
    return method.getGenericParameterTypes();
  }

  public int getModifiers() {
    return method.getModifiers();
  }

  public boolean isSynthetic() {
    return method.isSynthetic();
  }

  public String getName() {
    return method.getName();
  }

  public <T extends Annotation> T getAnnotation(Class<T> class_) {
    return method.getAnnotation(class_);
  }

  public Annotation[] getAnnotations() {
    return method.getAnnotations();
  }

  public Annotation[] getDeclaredAnnotations() {
    return method.getDeclaredAnnotations();
  }

  public Annotation[][] getParameterAnnotations() {
    return method.getParameterAnnotations();
  }

  private static native Object make(avian.VMClass c);

  public T newInstance(Object ... arguments)
    throws InvocationTargetException, InstantiationException,
    IllegalAccessException
  {
    T v = (T) make(method.getDeclaringClass().vmClass);
    method.invoke(v, arguments);
    return v;
  }

  public Class<?>[] getExceptionTypes() {
    throw new UnsupportedOperationException("not yet implemented");
  }
}
