import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.reflect.Method;

public class AnnotationClassLiterals {
  @Retention(RetentionPolicy.RUNTIME)
  public @interface Holds {
    Class<?> value();
  }

  private static void expect(boolean v) {
    if (! v) throw new RuntimeException();
  }

  @Holds(void.class)
  public static void voidHolder() {}

  @Holds(int.class)
  public static void intHolder() {}

  @Holds(String.class)
  public static void stringHolder() {}

  @Holds(String[].class)
  public static void arrayHolder() {}

  private static Class<?> held(String method) throws Exception {
    Method m = AnnotationClassLiterals.class.getMethod(method);
    return ((Holds) m.getAnnotation(Holds.class)).value();
  }

  public static void main(String[] args) throws Exception {
    expect(held("voidHolder") == void.class);
    expect(held("intHolder") == int.class);
    expect(held("stringHolder") == String.class);
    expect(held("arrayHolder") == String[].class);
  }
}
