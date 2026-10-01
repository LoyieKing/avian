import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

import avian.testing.annotations.Color;
import avian.testing.annotations.Test;
import avian.testing.annotations.TestComplex;
import avian.testing.annotations.TestEnum;
import avian.testing.annotations.TestInteger;
import avian.testing.annotations.TestInts;

public class Annotations {
  private static void expect(boolean v) {
    if (! v) throw new RuntimeException();
  }

  public static void main(String[] args) throws Exception {
    Method m = Annotations.class.getMethod("foo");

    expect(m.isAnnotationPresent(Test.class));

    expect(((Test) m.getAnnotation(Test.class)).value().equals("couscous"));

    expect(((TestEnum) m.getAnnotation(TestEnum.class)).value()
           .equals(Color.Red));

    expect(((TestInteger) m.getAnnotation(TestInteger.class)).value() == 42);
    
    expect(m.getAnnotations().length == 3);
    
    Method noAnno = Annotations.class.getMethod("noAnnotation");
    expect(noAnno.getAnnotation(Test.class) == null);
    expect(noAnno.getAnnotations().length == 0);
    testProxyDefaultValue();
    testComplexAnnotation();
    testIntArray();
    testParameterAnnotations();
  }

  public static void annotated(@Test("couscous") String name) {
  }

  private static void testParameterAnnotations() throws Exception {
    java.lang.annotation.Annotation[][] present = Annotations.class
      .getMethod("annotated", String.class).getParameterAnnotations();
    expect(present.length == 1);
    expect(present[0].length == 1);
    expect("couscous".equals(((Test) present[0][0]).value()));
    java.lang.annotation.Annotation[][] absent = Annotations.class
      .getMethod("noAnnotation").getParameterAnnotations();
    expect(absent.length == 0);
  }

  @Test("couscous")
  @TestEnum(Color.Red)
  @TestInteger(42)
  public static void foo() {
    
  }

  @TestInts(value = { 2, 1, 0 }, names = { "kind" })
  public static void ints() {
  }

  @TestInts(names = { "kind" })
  public static void intsDefault() {
  }

  private static void testIntArray() throws Exception {
    TestInts annotation = (TestInts) Annotations.class.getMethod("ints").getAnnotation(TestInts.class);
    int[] value = annotation.value();
    expect(value instanceof int[]);
    expect(value.length == 3);
    expect(value[0] == 2 && value[1] == 1 && value[2] == 0);
    expect(annotation.names().length == 1);
    expect("kind".equals(annotation.names()[0]));

    TestInts defaults = (TestInts) Annotations.class.getMethod("intsDefault").getAnnotation(TestInts.class);
    int[] fallback = defaults.value();
    expect(fallback.length == 3);
    expect(fallback[0] == 2 && fallback[1] == 1 && fallback[2] == 0);

    int[] fromMethod = (int[]) TestInts.class.getMethod("value").getDefaultValue();
    expect(fromMethod.length == 3);
    expect(fromMethod[0] == 2 && fromMethod[1] == 1 && fromMethod[2] == 0);
    expect(annotation.equals(Annotations.class.getMethod("ints").getAnnotation(TestInts.class)));
  }
  
  public static void noAnnotation() {
    
  }

  private static void testProxyDefaultValue() {
    ClassLoader loader = Annotations.class.getClassLoader();
    InvocationHandler handler = new InvocationHandler() {
      public Object invoke(Object proxy, Method method, Object... args) {
        return method.getDefaultValue();
      }
    };
    Test test = (Test)
      Proxy.newProxyInstance(loader, new Class[] { Test.class }, handler);
    expect("Hello, world!".equals(test.value()));
  }

  private interface World {
    @TestComplex(arrayValue = { @Test, @Test(value = "7/9") },
      stringValue = "adjunct element", charValue = '7', doubleValue = 0.7778,
      classValue = TestInteger.class)
    int hello();
  }

  private static void testComplexAnnotation(TestComplex annotation)
    throws Exception
  {
    expect(2 == annotation.arrayValue().length);
    expect("Hello, world!".equals(annotation.arrayValue()[0].value()));
    expect("7/9".equals(annotation.arrayValue()[1].value()));
    expect("adjunct element".equals(annotation.stringValue()));
    expect('7' == annotation.charValue());
    expect(0.7778 == annotation.doubleValue());
    expect(TestInteger.class == annotation.classValue());
  }

  public static void testComplexAnnotation() throws Exception {
    ClassLoader loader = Annotations.class.getClassLoader();
    TestComplex annotation = (TestComplex)
      World.class.getMethod("hello").getAnnotation(TestComplex.class);
    testComplexAnnotation(annotation);
    Class clazz = Proxy.getProxyClass(loader, new Class[] { World.class });
    annotation = (TestComplex)
      clazz.getMethod("hello").getAnnotation(TestComplex.class);
    expect(annotation == null);
  }
}
