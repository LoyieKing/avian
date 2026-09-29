public class JdwpDebug {
  int n;
  float f;
  double d;
  static JdwpDebug held;
  static Object mon = new Object();

  // Several bytecodes so a single-step leaves bci 0.
  static int marker(int x) {
    int y = x + 1;
    y = y + 0;
    return y;
  }

  static int plus(int x) {
    return x + 10;
  }

  static int fail() {
    throw new IllegalStateException("fail");
  }

  static int early() {
    return 7;
  }

  void bump() {
    n = n + 1;
    f = 1.5f;
    d = 2.5;
  }

  static void boom() {
    try {
      throw new RuntimeException("boom");
    } catch (RuntimeException e) {
      // caught
    }
  }

  static final class Worker implements Runnable {
    public void run() {
      synchronized (mon) {
        try {
          mon.wait(30);
        } catch (InterruptedException e) {
        }
      }
    }
  }

  static void startWorker() {
    Thread w;
    synchronized (mon) {
      w = new Thread(new Worker());
      w.start();
      try {
        Thread.sleep(200);
      } catch (InterruptedException e) {
      }
    }
    try {
      w.join();
    } catch (InterruptedException e) {
    }
  }

  static class Box<T> {
    T value;
    public T get() {
      return value;
    }
  }

  static class Drop extends ClassLoader {
    static volatile boolean gone;
    Class<?> load(byte[] b) {
      return defineClass("Unloaded", b, 0, b.length);
    }
    protected void finalize() {
      gone = true;
    }
  }

  static void unload() {
    byte[] b = new byte[] {
      (byte)202,(byte)254,(byte)186,(byte)190,0,0,0,52,0,10,10,0,3,0,7,7,0,8,7,0,9,
      1,0,6,60,105,110,105,116,62,1,0,3,40,41,86,1,0,4,67,111,100,101,12,0,4,0,5,
      1,0,8,85,110,108,111,97,100,101,100,1,0,16,106,97,118,97,47,108,97,110,103,47,
      79,98,106,101,99,116,0,33,0,2,0,3,0,0,0,0,0,1,0,1,0,4,0,5,0,1,0,6,0,0,0,17,
      0,1,0,1,0,0,0,5,42,(byte)183,0,1,(byte)177,0,0,0,0,0,0
    };
    Drop d = new Drop();
    Class<?> c = d.load(b);
    if (c == null)
      throw new RuntimeException("define");
    d = null;
    c = null;
    for (int i = 0; i < 8; ++i) {
      System.gc();
      try {
        Thread.sleep(40);
      } catch (InterruptedException e) {
      }
    }
  }

  int peek() {
    return n;
  }

  static volatile int ranLeaf;

  static void leaf() {
    ranLeaf = 1;
  }

  public static void main(String[] args) {
    int value = marker(41);
    System.out.println("result=" + value);
    JdwpDebug box = new JdwpDebug();
    held = box;
    Box<String> generic = new Box<String>();
    generic.value = "g";
    if (generic.get() == null)
      throw new RuntimeException("generic");
    box.bump();
    boom();
    startWorker();
    unload();
    int e = early();
    System.out.println("early=" + e);
    leaf();
    System.out.println("gone=" + Drop.gone);
    System.out.println("leaf=" + ranLeaf);
  }
}

