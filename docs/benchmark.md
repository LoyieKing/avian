# MicroBench 与 OpenJDK 8

同一份字节码，单线程。源码是 [MicroBench.java](MicroBench.java)。计时用 `currentTimeMillis`，因为 Avian 的 `nanoTime` 来自毫秒时钟。

每个项目预热 2 次、正式 5 次。中位数是排序后的 `samples[2]`。`volatile long sink` 接住结果，避免被当成死代码。

| 项目 | 工作量 |
| --- | --- |
| loop | 1 亿次整数混合 |
| fib | `fib(38)` |
| array | 长度 100 万的 `int[]`，扫 400 遍，每格加 1 |
| alloc | `new Cell(i, head)` 四千万次。每 32 个丢掉链表，沿 `next` 最多走 8 步，活对象不超过 32 个 |

array 的第 2 次预热校验和是 `6200234000000000`：`main` 在计时前填过一次数组，第一次预热又改写了它。每次正式试验会重新 `fillArray`，校验和是 `6200074000000000`。

## 怎么跑

不要同时跑 Avian 和 JDK。机器一忙，毫秒计时就会漂。

```sh
export JAVA_HOME=/Library/Java/JavaVirtualMachines/zulu-8.jdk/Contents/Home
make platform=macosx arch=arm64 mode=fast process=compile -j8

"$JAVA_HOME/bin/javac" -d /tmp/avian-bench docs/MicroBench.java

build/macosx-arm64/avian -Xmx256m -Xss1m -cp /tmp/avian-bench MicroBench
"$JAVA_HOME/bin/java" -Xmx256m -Xss1m -cp /tmp/avian-bench MicroBench
```

比较对象是 Zulu 8（1.8.0_345，aarch64，默认混合模式）。Avian 用自己的 classpath，不是 OpenJDK 的 `rt.jar`。字节码由上面的 `javac` 生成，两边跑同一份 class。

## 2026-09-30 这一轮

Apple M4 Pro，macOS，arm64。文件拆开并重新编译之后测的。两轮 Avian，然后一轮 JDK。校验和每轮都相同。

| 项目 | Avian 第 1 轮中位 | Avian 第 2 轮中位 | OpenJDK 8 中位 | 倍数 |
| --- | --- | --- | --- | --- |
| loop | 323 ms | 322 ms | 178 ms | 1.8× |
| fib(38) | 128 ms | 137 ms | 69 ms | 1.9–2.0× |
| array | 335 ms | 336 ms | 104 ms | 3.2× |
| alloc | 99 ms | 99 ms | 55 ms | 1.8× |

最佳 / 平均：

| 项目 | Avian 第 1 轮 | Avian 第 2 轮 | OpenJDK 8 |
| --- | --- | --- | --- |
| loop | 321 / 327 | 320 / 325 | 170 / 178 |
| fib(38) | 122 / 130 | 126 / 135 | 67 / 68 |
| array | 330 / 334 | 332 / 337 | 103 / 104 |
| alloc | 98 / 99 | 97 / 98 | 54 / 58 |

校验和：

| 项目 | 值 |
| --- | --- |
| loop | 4007451445307964056 |
| fib(38) | 39088169 |
| array 正式试验 | 6200074000000000 |
| alloc | 667894464 |

拆文件之前，同一台机器上 alloc 的中位数是 95 ms 和 97 ms，校验和也是 667894464。这次两轮都是 99 ms，试验值落在 97–100 ms。这次移动没有改生成的代码。

## 这些 pass 之前

同一台机器、同一套参数下，更早一轮的中位数是：

| 项目 | 当时的 Avian | 当时的 OpenJDK 8 | 倍数 |
| --- | --- | --- | --- |
| loop | 307 ms | 177 ms | 1.7× |
| fib(38) | 130 ms | 69 ms | 1.9× |
| array | 993 ms | 104 ms | 9.5× |
| alloc | 524 ms | 56 ms | 9.4× |

array 从大约 9.5× 收到大约 3.2×，主要是向后跳转不再每次都调用 safepoint，再加上证明过的边界检查被去掉。alloc 从大约 9.4× 收到大约 1.8×：先是把 64KB 块的 bump 内联进 `new`，再内联 `Cell` 的构造器，然后年轻对象的 `next` 不再调用 `setMaybeNull`。loop 和 fib 仍在 1.8× 到 2.0×。

空进程的启动当时是 Avian 约 9.9 ms，JDK 约 55 ms。这一轮没有重测启动。
