public class SystemProperties {
  private static void expect(boolean v) {
    if (! v) throw new RuntimeException();
  }

  public static void main(String[] args) {
    expect("1.8.0".equals(System.getProperty("java.version")));
    expect("1.8".equals(System.getProperty("java.specification.version")));
    expect("Avian".equals(System.getProperty("java.vendor")));
    expect("Avian".equals(System.getProperty("java.vm.name")));
    expect("UTF-8".equals(System.getProperty("file.encoding")));
  }
}
