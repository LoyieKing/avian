/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.lang.reflect;

import avian.Classes;

import java.util.ArrayList;
import java.util.List;
import java.util.LinkedList;
import java.util.Map;
import java.util.HashMap;

public class SignatureParser {
  private static final Map<Class, TypeVariable[]> CLASS_TYPE_PARAMETERS
    = new HashMap<Class, TypeVariable[]>();

  private final ClassLoader loader;
  private final char[] array;
  private final String signature;
  private int offset;
  private final Type type;
  private final Map<String, TypeVariable> typeVariables;

  public static Type parse(ClassLoader loader, String signature, Class declaringClass) {
    return new SignatureParser(loader, signature, collectTypeVariables(declaringClass)).type;
  }

  private static Type parse(ClassLoader loader, String signature, Map<String, TypeVariable> typeVariables) {
    return new SignatureParser(loader, signature, typeVariables).type;
  }

  private SignatureParser(ClassLoader loader, String signature, Map<String, TypeVariable> typeVariables) {
    this.loader = loader;
    this.signature = signature;
    array = signature.toCharArray();
    this.typeVariables = typeVariables;
    type = parseType();
    if (offset != array.length) {
      throw new IllegalArgumentException("Extra characters after " + offset
          + ": " + signature);
    }
  }

  public static TypeVariable<?>[] classTypeParameters(Class clz) {
    TypeVariable[] cached = CLASS_TYPE_PARAMETERS.get(clz);
    if (cached != null) return cached;
    if (clz.vmClass.addendum == null || clz.vmClass.addendum.signature == null) {
      return cacheEmpty(clz);
    }
    String signature = Classes.toString((byte[]) clz.vmClass.addendum.signature);
    if (signature.length() == 0 || signature.charAt(0) != '<') {
      return cacheEmpty(clz);
    }
    Map<String, TypeVariable> varsMap = new HashMap<String, TypeVariable>();
    LinkedList<TypeVariableImpl> varsList = new LinkedList<TypeVariableImpl>();
    char[] signChars = signature.toCharArray();
    int[] cursor = new int[] { 1 };
    while (cursor[0] < signChars.length && signChars[cursor[0]] != '>') {
      int colon = signature.indexOf(':', cursor[0]);
      if (colon < 0) {
        throw new RuntimeException("Can't find ':' in " + signature);
      }
      String typeVarName = signature.substring(cursor[0], colon);
      TypeVariableImpl tv = new TypeVariableImpl(typeVarName, Object.class);
      tv.setDeclaration(clz);
      varsList.add(tv);
      varsMap.put(typeVarName, tv);
      cursor[0] = colon + 1;
      if (cursor[0] < signChars.length && signChars[cursor[0]] != ':') {
        cursor[0] = skipFieldSignature(signChars, cursor[0]);
      }
      while (cursor[0] < signChars.length && signChars[cursor[0]] == ':') {
        cursor[0] = skipFieldSignature(signChars, cursor[0] + 1);
      }
    }
    for (TypeVariableImpl tv : varsList) {
      tv.setVars(varsList);
    }
    TypeVariable[] result = new TypeVariable[varsList.size()];
    varsList.toArray(result);
    CLASS_TYPE_PARAMETERS.put(clz, result);
    cursor[0] = 1;
    int n = 0;
    while (cursor[0] < signChars.length && signChars[cursor[0]] != '>') {
      int colon = signature.indexOf(':', cursor[0]);
      TypeVariableImpl tv = varsList.get(n++);
      cursor[0] = colon + 1;
      if (cursor[0] < signChars.length && signChars[cursor[0]] != ':') {
        int start = cursor[0];
        cursor[0] = skipFieldSignature(signChars, cursor[0]);
        Type base = parse(clz.vmClass.loader, signature.substring(start, cursor[0]), varsMap);
        if (base != null) tv.baseType = base;
      }
      while (cursor[0] < signChars.length && signChars[cursor[0]] == ':') {
        cursor[0] = skipFieldSignature(signChars, cursor[0] + 1);
      }
    }
    return result;
  }

  private static TypeVariable[] cacheEmpty(Class clz) {
    TypeVariable[] empty = new TypeVariable[0];
    CLASS_TYPE_PARAMETERS.put(clz, empty);
    return empty;
  }

  private static int skipFieldSignature(char[] s, int i) {
    while (s[i] == '[') i++;
    char c = s[i];
    if (c == 'L' || c == 'T') {
      int angles = 0;
      while (true) {
        char d = s[i++];
        if (d == '<') angles++;
        else if (d == '>') angles--;
        else if (d == ';' && angles == 0) return i;
      }
    }
    return i + 1;
  }

  public static TypeVariable<?>[] methodTypeParameters(Class declaring, String signature) {
    return parseMethod(declaring, signature).vars;
  }

  public static Type[] genericParameterTypes(Class declaring, String signature) {
    return parseMethod(declaring, signature).params;
  }

  public static Type genericReturnType(Class declaring, String signature) {
    return parseMethod(declaring, signature).ret;
  }

  private static final class ParsedMethod {
    TypeVariable[] vars;
    Type[] params;
    Type ret;
  }

  private static ParsedMethod parseMethod(Class declaring, String signature) {
    Map<String, TypeVariable> varsMap = new HashMap<String, TypeVariable>();
    TypeVariable[] classVars = classTypeParameters(declaring);
    for (int n = 0; n < classVars.length; ++n) {
      varsMap.put(classVars[n].getName(), classVars[n]);
    }
    char[] s = signature.toCharArray();
    int i = 0;
    LinkedList<TypeVariableImpl> own = new LinkedList<TypeVariableImpl>();
    if (s.length > 0 && s[0] == '<') {
      i = 1;
      while (i < s.length && s[i] != '>') {
        int colon = signature.indexOf(':', i);
        String typeVarName = signature.substring(i, colon);
        i = colon + 1;
        TypeVariableImpl tv = new TypeVariableImpl(typeVarName, Object.class);
        own.add(tv);
        varsMap.put(typeVarName, tv);
        if (i < s.length && s[i] != ':') {
          int start = i;
          i = skipFieldSignature(s, i);
          Type base = parse(declaring.vmClass.loader, signature.substring(start, i), varsMap);
          if (base != null) tv.baseType = base;
        }
        while (i < s.length && s[i] == ':') {
          i = skipFieldSignature(s, i + 1);
        }
      }
      if (i < s.length && s[i] == '>') i++;
    }
    for (TypeVariableImpl tv : own) {
      tv.setVars(own);
    }
    if (i >= s.length || s[i] != '(') {
      throw new RuntimeException("method signature missing '(': " + signature);
    }
    i++;
    ArrayList<Type> params = new ArrayList<Type>();
    while (i < s.length && s[i] != ')') {
      int start = i;
      i = skipFieldSignature(s, i);
      Type param = parse(declaring.vmClass.loader, signature.substring(start, i), varsMap);
      params.add(param == null ? Object.class : param);
    }
    if (i >= s.length || s[i] != ')') {
      throw new RuntimeException("method signature missing ')': " + signature);
    }
    i++;
    int start = i;
    i = skipFieldSignature(s, i);
    Type ret = parse(declaring.vmClass.loader, signature.substring(start, i), varsMap);
    ParsedMethod parsed = new ParsedMethod();
    parsed.vars = new TypeVariable[own.size()];
    own.toArray(parsed.vars);
    parsed.params = params.toArray(new Type[params.size()]);
    parsed.ret = ret == null ? Object.class : ret;
    return parsed;
  }

  private Type parseType() {
    char c = array[offset++];
    if (c == '[') {
      final Type component = parseType();
      return new GenericArrayType() {
        public Type getGenericComponentType() {
          return component;
        }
        public String toString() {
          return component.toString() + "[]";
        }
      };
    } else if (c == '*') {
      return wildcard(new Type[] { Object.class }, new Type[0]);
    } else if (c == '+') {
      return wildcard(new Type[] { parseType() }, new Type[0]);
    } else if (c == '-') {
      return wildcard(new Type[] { Object.class }, new Type[] { parseType() });
    } else if (c == 'V') {
      return Void.TYPE;
    } else if (c == 'B') {
      return Byte.TYPE;
    } else if (c == 'C') {
      return Character.TYPE;
    } else if (c == 'D') {
      return Double.TYPE;
    } else if (c == 'F') {
      return Float.TYPE;
    } else if (c == 'I') {
      return Integer.TYPE;
    } else if (c == 'J') {
      return Long.TYPE;
    } else if (c == 'S') {
      return Short.TYPE;
    } else if (c == 'Z') {
      return Boolean.TYPE;
    } else if (c == 'T') {
      int end = signature.indexOf(';', offset);
      if (end < 0) {
        throw new RuntimeException("No semicolon found while parsing signature");
      }
      Type res = typeVariables.get(new String(array, offset, end - offset));
      offset = end + 1;
      return res == null ? Object.class : res;
    } else if (c != 'L') {
      throw new IllegalArgumentException("Unexpected character: " + c + ", signature: " + new String(array, 0, array.length) + ", i = " + offset);
    }
    StringBuilder builder = new StringBuilder();
    Type ownerType = null;
    for (;;) {
      for (;;) {
        c = array[offset++];
        if (c == ';' || c == '<') {
          break;
        }
        builder.append(c == '/' ? '.' : c);
      }
      String rawTypeName = builder.toString();
      Class<?> rawType;
      try {
        rawType = loader.loadClass(rawTypeName);
      } catch (ClassNotFoundException e) {
        throw new RuntimeException("Could not find class " + rawTypeName);
      }

      int lastDollar = rawTypeName.lastIndexOf('$');
      if (lastDollar != -1 && ownerType == null) {
        String ownerName = rawTypeName.substring(0, lastDollar);
        try {
          ownerType = loader.loadClass(ownerName);
        } catch (ClassNotFoundException e) {
          throw new RuntimeException("Could not find class " + ownerName);
        }
      }

      if (c == ';') {
        return rawType;
      }
      List<Type> args = new ArrayList<Type>();
      while (array[offset] != '>') {
        args.add(parseType());
      }
      ++offset;
      c = array[offset++];
      ParameterizedType type = makeType(args.toArray(new Type[args.size()]), ownerType, rawType);
      if (c == ';') {
        return type;
      }
      if (c != '.') {
        throw new RuntimeException("TODO");
      }
      ownerType = type;
      builder.append("$");
    }
  }

  private static String typeName(Type type) {
    if (type instanceof Class) {
      Class<?> clazz = (Class<?>) type;
      return clazz.getName();
    }
    return type.toString();
  }

  private static WildcardType wildcard(final Type[] upper, final Type[] lower) {
    return new WildcardType() {
      public Type[] getUpperBounds() { return upper; }
      public Type[] getLowerBounds() { return lower; }
      public String toString() {
        if (lower.length > 0) return "? super " + lower[0];
        if (upper.length == 1 && upper[0] == Object.class) return "?";
        return "? extends " + upper[0];
      }
    };
  }

  private static ParameterizedType makeType(final Type[] args, final Type owner, final Type raw) {
    return new ParameterizedType() {
      @Override
        public Type getRawType() {
          return raw;
        }

      @Override
        public Type getOwnerType() {
          return owner;
        }

      @Override
        public Type[] getActualTypeArguments() {
          return args;
        }

      @Override
        public String toString() {
          StringBuilder builder = new StringBuilder();
          builder.append(typeName(raw));
          builder.append('<');
          String sep = "";
          for (Type t : args) {
            builder.append(sep).append(typeName(t));
            sep = ", ";
          }
          builder.append('>');
          return builder.toString();
        }
    };
  }
  
  private static Map<String, TypeVariable> collectTypeVariables(Class clz) {
    Map<String, TypeVariable> varsMap = new HashMap<String, TypeVariable>();
    LinkedList<Class> classList = new LinkedList<Class>();
    for (Class c = clz; c != null; c = c.getDeclaringClass()) {
      classList.addFirst(c);
    }
    
    for (Class cur : classList) {
      final LinkedList<TypeVariableImpl> varsList = new LinkedList<TypeVariableImpl>();
      if (cur.vmClass.addendum != null && cur.vmClass.addendum.signature != null) {
        String signature = Classes.toString((byte[]) cur.vmClass.addendum.signature);
        final char[] signChars = signature.toCharArray();
        try {
          int i = 0;
          if (signChars[i] == '<') {
            i++;
            do {
              final int colon = signature.indexOf(':', i);
              if (colon < 0 || colon + 1 == signChars.length) {
                throw new RuntimeException("Can't find ':' in the signature " + signature + " starting from " + i);
              }
              String typeVarName = new String(signChars, i, colon - i);
              i = colon + 1;
              
              int start = i;
              int angles = 0;
              while (angles > 0 || signChars[i] != ';') {
                if (signChars[i] == '<') angles ++;
                else if (signChars[i] == '>') angles --;
                i++;
              }
              String typeName = new String(signChars, start, i - start + 1);
              final Type baseType = SignatureParser.parse(cur.vmClass.loader, typeName, varsMap);
  
              TypeVariableImpl tv = new TypeVariableImpl(typeVarName, baseType);
              varsList.add(tv);

              i++;
            } while (signChars[i] != '>');
          
          }
        } catch (IndexOutOfBoundsException e) {
          throw new RuntimeException("Signature of " + cur + " is broken (" + signature + ") and can't be parsed", e);
        }
      }
      for (TypeVariableImpl tv : varsList) {
        tv.setVars(varsList);
        varsMap.put(tv.getName(), tv);
      }
      cur = cur.getDeclaringClass();
    };
    return varsMap;
  } 

  private static class TypeVariableImpl implements TypeVariable {
    private String name;
    private Type baseType;
    private TypeVariableImpl[] vars;
    private GenericDeclaration declaration;

    public Type[] getBounds() {
      return new Type[] { baseType };
    }

    public GenericDeclaration getGenericDeclaration() {
      if (declaration != null) return declaration;
      return new GenericDeclaration() {
        public TypeVariable<?>[] getTypeParameters() {
          return vars;
        }
      };
    }

    void setDeclaration(GenericDeclaration declaration) {
      this.declaration = declaration;
    }
    
    public String getName() {
      return name;
    }
    
    TypeVariableImpl(String name, Type baseType) {
      this.name = name;
      this.baseType = baseType;
    }
    
    void setVars(List<TypeVariableImpl> vars) {
      this.vars = new TypeVariableImpl[vars.size()];
      vars.toArray(this.vars);
    }
    
    @Override
    public String toString() {
      return name;
    }
  }
}
