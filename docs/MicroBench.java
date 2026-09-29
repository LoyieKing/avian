// Single-thread microbenchmarks that stay inside the Java subset Avian
// implements. Timed with currentTimeMillis because Avian's nanoTime is
// derived from the millisecond clock.
public class MicroBench {
  static volatile long sink;

  static final int LOOP_ITERS = 100000000;
  static final int FIB_N = 38;
  static final int ARRAY_LEN = 1000000;
  static final int ARRAY_PASSES = 400;
  static final int ALLOC_N = 40000000;
  static final int WARMUP = 2;
  static final int TRIALS = 5;

  static int[] data;

  static long mix(long s, int i) {
    s = (s * 1664525L) + 1013904223L;
    s ^= (long) i * 31L;
    s += (s >>> 13);
    return s;
  }

  static long loopWork() {
    long s = 0x1234abcdL;
    for (int i = 0; i < LOOP_ITERS; i++) {
      s = mix(s, i);
    }
    return s;
  }

  static long fib(int n) {
    if (n < 2) {
      return n;
    }
    return fib(n - 1) + fib(n - 2);
  }

  static long fibWork() {
    return fib(FIB_N);
  }

  static void fillArray() {
    data = new int[ARRAY_LEN];
    for (int i = 0; i < ARRAY_LEN; i++) {
      data[i] = i * 31;
    }
  }

  static long arrayWork() {
    int[] a = data;
    int n = a.length;
    long s = 0;
    for (int p = 0; p < ARRAY_PASSES; p++) {
      for (int i = 0; i < n; i++) {
        int v = a[i] + 1;
        a[i] = v;
        s += v;
      }
    }
    return s;
  }

  static final class Cell {
    int v;
    Cell next;

    Cell(int v, Cell next) {
      this.v = v;
      this.next = next;
    }
  }

  static int allocWork() {
    int sum = 0;
    Cell head = null;
    for (int i = 0; i < ALLOC_N; i++) {
      head = new Cell(i, head);
      if ((i & 31) == 31) {
        Cell c = head;
        int k = 0;
        while (c != null && k < 8) {
          sum += c.v;
          c = c.next;
          k++;
        }
        head = null;
      }
    }
    return sum;
  }

  static long run(int which) {
    if (which == 0) {
      return loopWork();
    }
    if (which == 1) {
      return fibWork();
    }
    if (which == 2) {
      return arrayWork();
    }
    return allocWork();
  }

  static void report(String name, int which) {
    for (int i = 0; i < WARMUP; i++) {
      long s = run(which);
      sink = s;
      System.out.println(name + " warmup=" + i + " checksum=" + s);
    }
    long[] samples = new long[TRIALS];
    for (int t = 0; t < TRIALS; t++) {
      if (which == 2) {
        fillArray();
      }
      long t0 = System.currentTimeMillis();
      long s = run(which);
      long dt = System.currentTimeMillis() - t0;
      sink = s;
      samples[t] = dt;
      System.out.println(name + " trial=" + t + " ms=" + dt + " checksum=" + s);
    }
    for (int i = 1; i < TRIALS; i++) {
      long v = samples[i];
      int j = i;
      while (j > 0 && samples[j - 1] > v) {
        samples[j] = samples[j - 1];
        j--;
      }
      samples[j] = v;
    }
    long sum = 0;
    for (int i = 0; i < TRIALS; i++) {
      sum += samples[i];
    }
    System.out.println(name + " SUMMARY best_ms=" + samples[0]
        + " median_ms=" + samples[TRIALS / 2]
        + " avg_ms=" + (sum / TRIALS));
  }

  public static void main(String[] args) {
    if (args.length > 0 && "ping".equals(args[0])) {
      System.out.println("ok");
      return;
    }
    System.out.println("MicroBench start");
    fillArray();
    report("loop", 0);
    report("fib", 1);
    report("array", 2);
    report("alloc", 3);
    System.out.println("MicroBench done");
  }
}
