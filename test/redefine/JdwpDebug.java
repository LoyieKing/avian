public class JdwpDebug {
  int n;

  static int marker(int x) {
    int y = x + 1;
    y = y + 0;
    return y;
  }

  static int plus(int x) {
    return x + 10;
  }

  void bump() {
    n = n + 1;
  }

  static void boom() {
    try {
      throw new RuntimeException("boom");
    } catch (RuntimeException e) {
    }
  }

  static final class Worker implements Runnable {
    public void run() {
    }
  }

  static void startWorker() {
    Thread w = new Thread(new Worker());
    w.start();
    try {
      w.join();
    } catch (InterruptedException e) {
    }
  }

  static void leaf() {
    int z = 1;
    z = z + 0;
  }

  public static void main(String[] args) {
    int value = marker(41);
    System.out.println("result=" + value);
    JdwpDebug box = new JdwpDebug();
    box.bump();
    boom();
    startWorker();
    leaf();
  }
}
