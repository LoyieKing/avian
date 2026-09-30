import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

public class ElementTypes {
  @Retention(RetentionPolicy.RUNTIME)
  @Target({ElementType.TYPE_USE, ElementType.TYPE_PARAMETER})
  public @interface Marker {}

  private static void expect(boolean v) {
    if (! v) throw new RuntimeException();
  }

  public static void main(String[] args) {
    expect(ElementType.valueOf("TYPE_PARAMETER") == ElementType.TYPE_PARAMETER);
    expect(ElementType.valueOf("TYPE_USE") == ElementType.TYPE_USE);
    expect(ElementType.valueOf("MODULE") == ElementType.MODULE);
    expect(ElementType.valueOf("RECORD_COMPONENT") == ElementType.RECORD_COMPONENT);
    expect(ElementType.TYPE.ordinal() == 7);
    expect(ElementType.values().length == 12);

    // Linking Marker parses @Target and resolves these enum constants.
    Target target = Marker.class.getAnnotation(Target.class);
    ElementType[] where = target.value();
    expect(where.length == 2);
    expect(where[0] == ElementType.TYPE_USE);
    expect(where[1] == ElementType.TYPE_PARAMETER);
  }
}
