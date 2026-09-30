/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.lang.invoke;

import static avian.Assembler.*;
import static avian.Stream.set4;
import static avian.Stream.write1;
import static avian.Stream.write2;
import static avian.Stream.write4;

import avian.Assembler;
import avian.Classes;
import avian.ConstantPool;
import avian.ConstantPool.PoolEntry;
import avian.SystemClassLoader;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

// Java 9 invokedynamic string concatenation (JEP 280).
// Recipe tags: \u0001 is the next argument, \u0002 is the next constant.
public class StringConcatFactory {
  private static int nextNumber = 0;

  public static CallSite makeConcat(MethodHandles.Lookup lookup,
                                    String name,
                                    MethodType concatType)
  {
    int n = concatType.parameterArray().length;
    StringBuilder recipe = new StringBuilder();
    for (int i = 0; i < n; ++i) {
      recipe.append('\u0001');
    }
    return makeConcatWithConstants(
        lookup, name, concatType, recipe.toString(), new Object[0]);
  }

  public static CallSite makeConcatWithConstants(MethodHandles.Lookup lookup,
                                                 String name,
                                                 MethodType concatType,
                                                 String recipe,
                                                 Object... constants)
  {
    byte[] classData = makeConcatClass(concatType, recipe, constants);
    return makeCallSite(concatType, classData);
  }

  private static void append(ByteArrayOutputStream out,
                             List<PoolEntry> pool,
                             String spec)
    throws IOException
  {
    write1(out, invokevirtual);
    write2(out, ConstantPool.addMethodRef(
               pool, "java/lang/StringBuilder", "append", spec) + 1);
  }

  private static String appendSpec(String paramSpec) {
    if (paramSpec.equals("I") || paramSpec.equals("B") || paramSpec.equals("S")) {
      return "(I)Ljava/lang/StringBuilder;";
    } else if (paramSpec.equals("C")) {
      return "(C)Ljava/lang/StringBuilder;";
    } else if (paramSpec.equals("Z")) {
      return "(Z)Ljava/lang/StringBuilder;";
    } else if (paramSpec.equals("J")) {
      return "(J)Ljava/lang/StringBuilder;";
    } else if (paramSpec.equals("F")) {
      return "(F)Ljava/lang/StringBuilder;";
    } else if (paramSpec.equals("D")) {
      return "(D)Ljava/lang/StringBuilder;";
    } else if (paramSpec.equals("Ljava/lang/String;")) {
      return "(Ljava/lang/String;)Ljava/lang/StringBuilder;";
    } else {
      return "(Ljava/lang/Object;)Ljava/lang/StringBuilder;";
    }
  }

  private static byte[] makeConcatClass(MethodType concatType,
                                        String recipe,
                                        Object[] constants)
  {
    List<PoolEntry> pool = new ArrayList<PoolEntry>();
    List<MethodType.Parameter> params = new ArrayList<MethodType.Parameter>();
    for (MethodType.Parameter p: concatType.parameters()) {
      params.add(p);
    }

    ByteArrayOutputStream out = new ByteArrayOutputStream();
    try {
      write2(out, 5); // max stack
      int locals = concatType.footprint();
      if (locals < 1) locals = 1;
      write2(out, locals);
      write4(out, 0); // code length, patched below

      write1(out, new_);
      write2(out, ConstantPool.addClass(pool, "java/lang/StringBuilder") + 1);
      write1(out, dup);
      write1(out, invokespecial);
      write2(out, ConstantPool.addMethodRef(
                 pool, "java/lang/StringBuilder", "<init>", "()V") + 1);

      int argi = 0;
      int csti = 0;
      int i = 0;
      while (i < recipe.length()) {
        char c = recipe.charAt(i);
        if (c == '\u0001') {
          if (argi >= params.size()) {
            throw new RuntimeException("string concat recipe has too many arguments");
          }
          MethodType.Parameter p = params.get(argi++);
          if (p.position() > 255) {
            throw new RuntimeException("string concat slot out of range");
          }
          write1(out, p.load());
          write1(out, p.position());
          append(out, pool, appendSpec(p.spec()));
          ++i;
        } else if (c == '\u0002') {
          if (constants == null || csti >= constants.length) {
            throw new RuntimeException("string concat recipe has too many constants");
          }
          Object constant = constants[csti++];
          if (constant == null) {
            write1(out, 1); // aconst_null
          } else if (constant instanceof String) {
            write1(out, ldc_w);
            write2(out, ConstantPool.addString(pool, (String) constant) + 1);
          } else {
            throw new RuntimeException(
                "string concat constant type " + constant.getClass().getName());
          }
          append(out, pool, "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
          ++i;
        } else {
          int start = i;
          while (i < recipe.length()) {
            char n = recipe.charAt(i);
            if (n == '\u0001' || n == '\u0002') break;
            ++i;
          }
          String literal = recipe.substring(start, i);
          write1(out, ldc_w);
          write2(out, ConstantPool.addString(pool, literal) + 1);
          append(out, pool, "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
        }
      }
      if (argi != params.size()) {
        throw new RuntimeException("string concat recipe did not consume every argument");
      }

      write1(out, invokevirtual);
      write2(out, ConstantPool.addMethodRef(
                 pool, "java/lang/StringBuilder", "toString", "()Ljava/lang/String;") + 1);
      write1(out, areturn);
      write2(out, 0); // exception handlers
      write2(out, 0); // code attributes

      byte[] code = out.toByteArray();
      set4(code, 4, code.length - 12);

      int id;
      synchronized (StringConcatFactory.class) {
        id = ++nextNumber;
      }
      String className = "java/lang/invoke/StringConcat$" + id;
      int nameIndex = ConstantPool.addClass(pool, className);
      int superIndex = ConstantPool.addClass(pool, "java/lang/Object");
      Assembler.MethodData method = new Assembler.MethodData(
          ACC_PUBLIC | ACC_STATIC,
          ConstantPool.addUtf8(pool, "concat"),
          ConstantPool.addUtf8(pool, concatType.toMethodDescriptorString()),
          code);

      ByteArrayOutputStream classOut = new ByteArrayOutputStream();
      Assembler.writeClass(
          classOut,
          pool,
          nameIndex,
          superIndex,
          new int[0],
          new Assembler.FieldData[0],
          new Assembler.MethodData[] { method });
      return classOut.toByteArray();
    } catch (IOException e) {
      RuntimeException error = new RuntimeException();
      error.initCause(e);
      throw error;
    } catch (RuntimeException e) {
      throw e;
    }
  }

  private static CallSite makeCallSite(MethodType invokedType, byte[] classData) {
    try {
      return new CallSite(new MethodHandle(
          MethodHandle.REF_invokeStatic,
          invokedType.loader,
          Classes.toVMMethod(avian.SystemClassLoader.getClass(
              avian.Classes.defineVMClass(
                  invokedType.loader, classData, 0, classData.length))
              .getMethod("concat", invokedType.parameterArray()))));
    } catch (NoSuchMethodException e) {
      RuntimeException error = new RuntimeException();
      error.initCause(e);
      throw error;
    }
  }
}
