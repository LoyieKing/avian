public class JdwpDebug {
  // Several bytecodes so a single-step leaves bci 0.
  static int marker(int x) {
    int y = x + 1;
    y = y + 0;
    return y;
  }

  public static void main(String[] args) {
    int value = marker(41);
    System.out.println("result=" + value);
  }
}
