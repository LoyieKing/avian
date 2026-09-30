import java.io.ByteArrayOutputStream;
import java.lang.reflect.Method;

// Java 8 source cannot emit StringConcatFactory invokedynamic. This test
// builds a version-52 class whose pool contains CONSTANT_Module and
// CONSTANT_Package, and whose methods are invokedynamic calls.
public class StringConcat {
  private static void expect(boolean v) {
    if (! v) throw new RuntimeException();
  }

  private static void expectEquals(String actual, String expected) {
    if (actual == null || ! actual.equals(expected)) {
      throw new RuntimeException(actual + " != " + expected);
    }
  }

  public static void main(String[] args) throws Exception {
    Class<?> c = new Loader().define(classFile());
    Method two = c.getMethod("two", String.class, int.class);
    Method mid = c.getMethod("mid", String.class);

    expectEquals((String) two.invoke(null, "A", Integer.valueOf(3)), "xAy3");
    expectEquals((String) two.invoke(null, "B", Integer.valueOf(4)), "xBy4");
    expectEquals((String) mid.invoke(null, "Z"), "AMIDBZ");
    expect(c.getName().equals("StringConcatProbe"));
  }

  private static class Loader extends ClassLoader {
    public Loader() {
      super(StringConcat.class.getClassLoader());
    }

    public Class define(byte[] bytes) {
      return defineClass("StringConcatProbe", bytes, 0, bytes.length);
    }
  }

  private static final class Bytes extends ByteArrayOutputStream {
    void u1(int v) {
      write(v);
    }

    void u2(int v) {
      write(v >> 8);
      write(v);
    }

    void u4(int v) {
      write(v >> 24);
      write(v >> 16);
      write(v >> 8);
      write(v);
    }

    void bytes(byte[] b) {
      write(b, 0, b.length);
    }
  }

  private static final class Pool {
    final Bytes out = new Bytes();
    int count = 1;

    int utf(String s) {
      int index = count++;
      out.u1(1);
      byte[] b = modifiedUtf8(s);
      out.u2(b.length);
      out.bytes(b);
      return index;
    }

    int cls(int name) {
      return tagged(7, name);
    }

    int str(int utf) {
      return tagged(8, utf);
    }

    int module(int name) {
      return tagged(19, name);
    }

    int pkg(int name) {
      return tagged(20, name);
    }

    int nat(int name, int type) {
      int index = count++;
      out.u1(12);
      out.u2(name);
      out.u2(type);
      return index;
    }

    int method(int owner, int nat) {
      int index = count++;
      out.u1(10);
      out.u2(owner);
      out.u2(nat);
      return index;
    }

    int handle(int kind, int ref) {
      int index = count++;
      out.u1(15);
      out.u1(kind);
      out.u2(ref);
      return index;
    }

    int indy(int bootstrap, int nat) {
      int index = count++;
      out.u1(18);
      out.u2(bootstrap);
      out.u2(nat);
      return index;
    }

    private int tagged(int tag, int value) {
      int index = count++;
      out.u1(tag);
      out.u2(value);
      return index;
    }
  }

  private static byte[] modifiedUtf8(String s) {
    Bytes out = new Bytes();
    for (int i = 0; i < s.length(); ++i) {
      char c = s.charAt(i);
      if (c >= 0x0001 && c <= 0x007f) {
        out.u1(c);
      } else if (c <= 0x07ff) {
        out.u1(0xc0 | (c >> 6));
        out.u1(0x80 | (c & 0x3f));
      } else {
        out.u1(0xe0 | (c >> 12));
        out.u1(0x80 | ((c >> 6) & 0x3f));
        out.u1(0x80 | (c & 0x3f));
      }
    }
    return out.toByteArray();
  }

  private static void code(Bytes cf, int name, int stack, int locals, byte[] code) {
    cf.u2(name);
    cf.u4(12 + code.length);
    cf.u2(stack);
    cf.u2(locals);
    cf.u4(code.length);
    cf.bytes(code);
    cf.u2(0);
    cf.u2(0);
  }

  private static byte[] classFile() {
    Pool pool = new Pool();
    int classProbe = pool.cls(pool.utf("StringConcatProbe"));
    int classObject = pool.cls(pool.utf("java/lang/Object"));
    int nameTwo = pool.utf("two");
    int descTwo = pool.utf("(Ljava/lang/String;I)Ljava/lang/String;");
    int natTwo = pool.nat(nameTwo, descTwo);
    int nameMid = pool.utf("mid");
    int descMid = pool.utf("(Ljava/lang/String;)Ljava/lang/String;");
    int natMid = pool.nat(nameMid, descMid);

    String bsmDesc
        = "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        + "Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/lang/Object;)"
        + "Ljava/lang/invoke/CallSite;";
    int bsm = pool.handle(
        6,
        pool.method(
            pool.cls(pool.utf("java/lang/invoke/StringConcatFactory")),
            pool.nat(pool.utf("makeConcatWithConstants"), pool.utf(bsmDesc))));

    int recipeTwo = pool.str(pool.utf("x\u0001y\u0001"));
    int recipeMid = pool.str(pool.utf("A\u0002B\u0001"));
    int midConst = pool.str(pool.utf("MID"));
    int indyTwo = pool.indy(0, natTwo);
    int indyMid = pool.indy(1, natMid);

    // Parsed and otherwise unused. Unknown tags abort class loading.
    pool.module(pool.utf("java.base"));
    pool.pkg(pool.utf("java/lang"));

    int codeName = pool.utf("Code");
    int bootstrapName = pool.utf("BootstrapMethods");

    Bytes cf = new Bytes();
    cf.u4(0xCAFEBABE);
    cf.u2(0);
    cf.u2(52);
    cf.u2(pool.count);
    cf.bytes(pool.out.toByteArray());
    cf.u2(0x0021);
    cf.u2(classProbe);
    cf.u2(classObject);
    cf.u2(0);
    cf.u2(0);
    cf.u2(2);

    cf.u2(0x0009);
    cf.u2(nameTwo);
    cf.u2(descTwo);
    cf.u2(1);
    code(cf, codeName, 2, 2, new byte[] {
      0x2a, 0x1b, (byte) 0xba, (byte) (indyTwo >> 8), (byte) indyTwo, 0, 0,
      (byte) 0xb0
    });

    cf.u2(0x0009);
    cf.u2(nameMid);
    cf.u2(descMid);
    cf.u2(1);
    code(cf, codeName, 1, 1, new byte[] {
      0x2a, (byte) 0xba, (byte) (indyMid >> 8), (byte) indyMid, 0, 0,
      (byte) 0xb0
    });

    cf.u2(1);
    cf.u2(bootstrapName);
    cf.u4(16);
    cf.u2(2);
    cf.u2(bsm);
    cf.u2(1);
    cf.u2(recipeTwo);
    cf.u2(bsm);
    cf.u2(2);
    cf.u2(recipeMid);
    cf.u2(midConst);
    return cf.toByteArray();
  }
}
