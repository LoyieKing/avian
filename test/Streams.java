import java.util.ArrayList;
import java.util.Arrays;
import java.util.Iterator;
import java.util.List;
import java.util.Optional;
import java.util.function.Predicate;

public class Streams {
  private static void expect(boolean v) {
    if (!v) throw new RuntimeException();
  }

  public static void main(String[] args) {
    List<String> values = new ArrayList<String>();
    values.add("b");
    values.add("a");
    values.add("c");
    Optional<String> first = values.stream().findFirst();
    expect(first.isPresent() && "b".equals(first.get()));
    expect(!new ArrayList<String>().stream().findFirst().isPresent());
    expect(values.stream().allMatch(new Predicate<String>() {
      public boolean test(String value) { return value.length() == 1; }
    }));
    expect(!values.stream().allMatch(new Predicate<String>() {
      public boolean test(String value) { return "a".equals(value); }
    }));
    List<String> sorted = values.stream().sorted().toList();
    expect(sorted.size() == 3);
    expect("a".equals(sorted.get(0)) && "b".equals(sorted.get(1)) && "c".equals(sorted.get(2)));
    Iterator<String> it = Arrays.stream(new String[] { "x" }).iterator();
    expect(it.hasNext() && "x".equals(it.next()) && !it.hasNext());
  }
}
