package avian.testing.annotations;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.RUNTIME)
public @interface TestInts {
  public int[] value() default { 2, 1, 0 };
  public String[] names() default { "a" };
}
