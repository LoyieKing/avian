# JIT 改动

`process=compile` 相对 ReadyTalk 上游多了四件事。`new`、平凡构造器和年轻对象存储只有 64 位快路径，32 位仍走原来的调用。向后 `goto` 的轮询按指针宽度读 `Machine::exclusive`，32 位也编译。解释器（`process=interpret`）不编译 `compile.cpp`。AOT（`-DAVIAN_AOT_ONLY`）会编译 `compile.cpp`，但这三个文件的 `#include` 在 `#ifndef AVIAN_AOT_ONLY` 里面，AOT 不走这些快路径。

稳态测量用 `mode=fast`（`-O3 -DNDEBUG`）。下面的数字见 [benchmark.md](benchmark.md)。

## 文件

形状跟 HotSpot C1 的 `RangeCheckElimination` 一样：头文件里一个类、一个静态入口，`.cpp` 里放分析和发射。版权头用 Avian 自己的，不使用 Oracle 的 GPL 头。

| 文件 | 怎么编进 VM | 入口 |
| --- | --- | --- |
| `src/compile/rangeCheckElimination.h/.cpp` | makefile 里单独的翻译单元，只在 `process=compile` 时链接 | `RangeCheckElimination::eliminate` |
| `src/compile/inlineNew.h/.cpp` | 从 `src/compile.cpp` 的 `namespace local` 里 `#include` | `InlineNew::tryCompile` / `flush` |
| `src/compile/trivialConstructor.h/.cpp` | 同上 | `TrivialConstructor::tryCompile` |
| `src/compile/youngObjectStore.h/.cpp` | 同上 | `YoungObjectStore::tryCompile` / `flush` |

后三个不是独立的 `.o`。它们要往 IR 里发指令，而 `Frame` 和 `Context` 只定义在 `compile.cpp` 这个翻译单元里。范围检查消除只读字节码、返回一个按字节码 ip 索引的标记，所以可以单独编译。

向后无条件 `goto` 的 safepoint 轮询留在 `compile.cpp`，挨着原来的 `compileSafePoint`。它只有几十行。

这三个被包含的文件如果被单独拿去编译，会在开头 `#error`。不要把它们加进 makefile 的 `vm-sources`，否则会和 `compile.cpp` 里的定义重复，而且看不到 `Frame`。

## 各做了什么

### 向后 goto 的 safepoint

无条件向后跳的快路径是读 `Machine::exclusive`，通常不调用。有人在等 GC 时才走原来的 `idleIfNecessary`。条件回边仍在跳转前调用 safepoint。调试器打开时（`debug::enabled()`）每条回边都走原来的调用，字段观察点才不会被跳过。

### 范围检查消除

只删掉每次执行都能证明下标在数组内的 `*aload` / `*astore`。Avian 没有去优化，所以证明失败的访问保持原来的边界检查。能删的形状是：循环下标在头之前被存成非负常数，循环里唯一的另一处写是 `iinc 1`，头用 `array.length` 比较这个下标，而且没有人从循环外跳进循环体。

### 线程块上的 `new`

普通对象的 `new`，类已经准备好、没有 finalizer、不是弱引用、填充后的大小落在 1 到 64KB、方法里没有 `jsr` / `wide` / `tableswitch` / `lookupswitch`，就内联成现有的 `allocateSmall`：`heapIndex` 前移，对象头只写类指针。慢路径仍是 `makeNew64`，在整段字节码走完之后再补上。

`new` 占 3 个字节。多出来的两个逻辑 ip 放 bump 和汇合点，慢路径用一条显式跳转出去，再跳回来。不能把慢路径放成下一条字节码的自然落下，否则它会被排到 `return` 后面，方法会直接掉进去。

一次条件跳转同时覆盖「类还要初始化」「有独占 GC」「块满了」。再加一条跳转就需要第三个空位，这条指令没有。

### 平凡构造器

只内联这种方法体：`aload_0`，`invokespecial` 一个空的 `<init>()V`，然后零段或多段（`aload_0`，一条参数 load，`putfield`），最后 `return`。有计算、分支、别的调用，或者空的超类构造对不上，就整段退回原来的调用，不留下半截 IR。

基本类型字段直接存。对象字段默认仍调用 `setMaybeNull`。下面这一条是唯一的例外。

### 年轻对象的引用存储

构造器里恰好有一个对象字段，而且它是最后一次 `putfield` 时，用 `invokespecial` 自己的两个空字节做一次判断：对象指针落在当前线程的 64KB 块里，就直接存引用。块来自 C 分配器，不在 gen2 里，对象也不是 fixie，这种情况下 `needsMark` 为假，记住集不用更新。

空指针、已经提升、固定对象、以及 `makeNew` 没有落在这块里的对象，都走原来的 `setMaybeNull`。空指针仍会抛 `NullPointerException`。这里没有把 `mark()` 展开，也没有把 gen2 的偏移写死。刚分配出来这一点本身不够：`new` 和 `<init>` 之间可以有别的调用，慢路径也可以返回一块之外的对象。

## 发射顺序

`InlineNew::flush` 和 `YoungObjectStore::flush` 都在异常处理表和脏根都编译完之后、释放 stack map 之前。提早 `restoreState` 会把方法剩下的部分接到慢路径上。

`saveState` 会立刻占用条件跳转的一个后继。同一个边不能 `restoreState` 两次。慢路径不要再 `visitLogicalIp` 进自己的 ip；汇合点由走路器接着打开。

`condJump` 之前算出来、只给这条跳转用的值，跳转之后没有 site。快路径上要再用的指针从栈上重新取。`Cell` 的 `this` 和字段值在跳转时还在操作数栈上，所以取得到。

## 这次没有做

- SIMD，以及数组扫描里剩下的地址运算
- 通用方法内联
- 逃逸分析
- 新的分配器，或把 64KB 线程块加大
- 内联记住集、`mark()`，或把 gen2 / nextGen2 的偏移写进编译器
- 无符号边界比较的融合

分配微基准剩下的差距主要在 bump 的地址运算、`int` 字段存储，以及这次对象存储前的范围判断（包括这条分支带来的寄存器溢出）。
