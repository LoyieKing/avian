Avian - A lightweight Java Virtual Machine (JVM)
================================================

[![Build Status](https://travis-ci.org/LoyieKing/avian.png?branch=master)](https://travis-ci.org/LoyieKing/avian)

Quick Start
-----------

These are examples of building Avian on various operating systems for
the x86_64 architecture.  You may need to modify JAVA_HOME according
to where the JDK is installed on your system.  In all cases, be sure
to use forward slashes in the path.

#### on Linux:
    $ export JAVA_HOME=/usr/lib/jvm/java-7-openjdk-amd64
    $ make
    $ build/linux-x86_64/avian -cp build/linux-x86_64/test Hello

#### on Mac OS X:
    $ export JAVA_HOME=$(/usr/libexec/java_home)
    $ make
    $ build/macosx-x86_64/avian -cp build/macosx-x86_64/test Hello

#### on Windows (Cygwin):
    $ git clone git@github.com:ReadyTalk/win64.git ../win64
    $ export JAVA_HOME="/cygdrive/c/Program Files/Java/jdk1.7.0_45"
    $ make
    $ build/windows-x86_64/avian -cp build/windows-x86_64/test Hello

#### on FreeBSD:
    $ export JAVA_HOME=/usr/local/openjdk7
    $ gmake
    $ build/freebsd-x86_64/avian -cp build/freebsd-x86_64/test Hello


Introduction
------------

Avian is a lightweight virtual machine and class library designed to
provide a useful subset of Java's features, suitable for building
self-contained applications.


JDWP
----

Avian can speak JDWP (`dt_socket`) with `jdb`, IntelliJ, or VS Code.
`server=y` listens; `server=n` connects to a debugger that is already
listening. Both sides use the same `JDWP-Handshake` (each writes the 14
bytes, then reads them) and the same packets after that.

    avian -Xrunjdwp:transport=dt_socket,server=y,suspend=y,address=5005 -cp . MyMain
    jdb -attach 5005

    # debugger listens first; the VM connects (dt_socket client mode)
    avian -Xrunjdwp:transport=dt_socket,server=n,suspend=y,address=127.0.0.1:5005 -cp . MyMain

`-agentlib:jdwp=transport=dt_socket,server=y,suspend=y,address=5005` is
accepted as well. `address=host:port` splits on the last `:`.
`address=0` with `server=y` prints the chosen port on stderr
(`Listening for transport dt_socket at address: N`). `server=n` prints
`Connecting to transport dt_socket at address: host:port` and retries
the connect briefly. Set `AVIAN_JDWP_LOG=1` to log every JDWP command.
Windows sockets are not implemented: the flag is accepted and then
ignored, and the process is not suspended.

Suspension is cooperative. A thread stops at a safepoint the VM already
has: every bytecode while interpreting, every bytecode of a method that
was JIT-compiled after JDWP was enabled (a `CodeMemory` bitset, patched
with `CodeMemory::patch`, gates the call), and the existing
`idleIfNecessary` / interpreter safepoint for everything else. The
stopped thread enters the Idle state, so GC can run, then blocks on a
monitor. There is no preemptive suspend. Methods compiled before the
agent, and boot-image / AOT code, honor suspend only at those existing
safepoints and will not hit a breakpoint in their own bodies.

Single-step is at bytecode granularity (`size=min`) or, when a line
table exists, line granularity. Breakpoints are bytecode indices (jdb's
`stop in` / `stop at` both become a location).

On top of that, JDWP is still just a client of the same event layer:

* `Method.VariableTable` comes from the class file `LocalVariableTable`
  (javac `-g`). `StackFrame.GetValues` / `SetValues` read and write
  locals that exist. The interpreter uses the operand stack. The JIT
  uses the same slot map as GC, with parameters reversed by
  `parameterFootprint`. A compiled frame that cannot be read returns
  `OPAQUE_FRAME` (32) instead of a guessed value. The generic-signature
  string in the variable table is empty.
* `StackFrame.ThisObject` is local 0 for an instance frame, and null
  for a static or native frame.
* Heap objects the debugger names get a stable id (id 0 is null). An id
  is a strong root only after `ObjectReference.DisableCollection`
  (command 7); `EnableCollection` (command 8) drops the pin. An id whose
  object was collected stays that id and reads back as null
  (`INVALID_OBJECT` is 20), rather than being reused.
  `ObjectReference.ReferenceType` and `GetValues` (instance fields) and
  `ReferenceType.GetValues` (statics) run on the suspended thread.
  `DisposeObjects` drops ids.
* `ReferenceType.Instances` (set 2, command 16) and
  `ObjectReference.ReferringObjects` (set 9, command 10) walk the heap
  (`canGetInstanceInfo`). `maxInstances` / `maxReferrers` of 0 means
  "all", capped at 64. Subtypes are included. The walk runs on a
  suspended thread and is accurate only while threads are suspended.
* Exception events (caught and uncaught) and `ThreadStart` /
  `ThreadDeath`. The main thread does not pass through `Thread.start`,
  so it often has no `ThreadStart`. A JIT throw suspends at the throw
  site (the unwinder no longer defers it to the catch). A compiled
  invoke that throws while the caller is suspended does not unwind that
  frame; the exception is returned to the debugger.
* Field access and modification, in the interpreter and in methods
  JIT-compiled after JDWP is enabled. Same checkpoint/patch idea as
  breakpoints. Float and double modification events are reported after
  the store, so the bits are the value that was written (not zero).
  Access events carry no value, matching JDWP.
* Monitor events, from `monitorEnter` / `Object.wait` in the runtime, so
  both the interpreter and JIT (including methods compiled before the
  agent) see them: 43 contended enter, 44 contended entered, 45 wait
  (with the timeout), 46 waited. `timed_out` on 46 is approximated.
  `canRequestMonitorEvents` is set.
* `RedefineClasses` for a bytecode-only swap. The new code object is
  installed, and methods compiled after the redefine are invalidated
  back to the default thunk (virtual methods included). Schema changes,
  added or deleted methods, and verifier failures are rejected with the
  JDWP redefine errors (62-69). A frame already running the old body
  keeps it. A direct call already patched to the old address is not
  rewritten. Boot-image classes are not re-parsed.
* `PopFrames` (StackFrame command 4, top frame only) and
  `ForceEarlyReturn` (ThreadReference command 14, `canForceEarlyReturn`).
  The interpreter pushes the early-return value, or a zero for a plain
  pop, and does not re-execute the invoke. A compiled frame is left by
  jumping to the caller's return address with that value in the return
  register (`vmJump`, or `vmJumpFloat` on x86_64 and arm64). The code
  range is captured when the thread stops, so a redefine while it is
  suspended does not hide the frame. Not the current frame is 33, a
  type mismatch is 34. i386 and arm32 have no x87/VFP move in
  `vmJumpFloat`, so a float or double early return there is not placed
  in the floating return register.
* `ClassType.InvokeMethod` and `ObjectReference.InvokeMethod` run on a
  suspended thread, which is enough for jdb `print` / `eval` of a simple
  call. The invoke does not hit nested breakpoints.
* Class unload (event kind 9) when a class loaded by a custom
  `ClassLoader` is actually unloaded. The loader's `jclass` is a weak
  reference, and a native finalizer reports `Lname;` once the loader is
  unreachable. Bootstrap and application loaders are not unloaded.
  The event has no thread and does not suspend. `System.gc` is a major
  collection; it does not promise a particular class dies on a given
  call.
* Generic signatures are the class-file `Signature` attribute, not a
  source string. `ReferenceType.SignatureWithGeneric` (command 13),
  `ReferenceType.FieldsWithGeneric` (14), and
  `ReferenceType.MethodsWithGeneric` (15). For `Box<T>` the class
  signature looks like `<T:Ljava/lang/Object;>Ljava/lang/Object;`, the
  field like `TT;`, the method like `()TT;`.

`VirtualMachine.Capabilities` and `CapabilitiesNew` advertise field
watch, redefine, pop frames, instance info, monitor events, and early
return.

Not implemented: preemptive suspend, breakpoints inside boot-image /
AOT code, source-debug extension, generic signatures on local
variables, and Windows sockets. A 32-bit build compiles and passes the
`TARGET_THREAD_*` constant check (i386 `uint64` aligns to 4, so the
MyThread offsets differ from LP64). The i386 interpreter passes the
JDWP test. The i386 JIT aborts in the register allocator when a method
is compiled with a checkpoint on every bytecode, so compile-mode JDWP
does not run there yet. CI itself is 64-bit only.


JIT
---

`process=compile` now folds four things the old JIT left as calls or
checks. The three allocation fast paths are 64-bit only. The passes
live under `src/compile/`.
Range-check elimination is its own translation unit. The thread-chunk
`new`, the trivial `<init>` inliner, and the young-object reference
store are included from `src/compile.cpp`, because `Frame` and
`Context` are local to that file. A backward unconditional `goto`
reads `Machine::exclusive` and calls the safepoint only when a
collection is waiting.

The thread-local buffer now grows and shrinks inside a reserved young
region. This does not add SIMD, a general inliner, escape analysis, or
an inlined remembered-set walk. 32-bit builds keep the calls.

`docs/jit.md` describes the passes and the layout constraints.
`docs/benchmark.md` and `docs/MicroBench.java` are the OpenJDK 8
comparison. On an Apple M4 Pro, Zulu 8 1.8.0_345, `-Xmx256m -Xss1m`,
`mode=fast`, the same bytecode (median of five trials; Avian ran
twice):

    benchmark   Avian          OpenJDK 8   ratio
    loop        322–323 ms     178 ms      1.8×
    fib(38)     128–137 ms     69 ms       1.9–2.0×
    array       335–336 ms     104 ms      3.2×
    alloc       99 ms          55 ms       1.8×

Every trial checksum matched. Before these passes, on the same
machine, array was about 9.5× and alloc about 9.4×.


Supported Platforms
-------------------

Avian can currently target the following platforms:

  * Linux (i386, x86_64, ARM, and ARM64)
  * Windows (i386 and x86_64)
  * Mac OS X (i386 and x86_64)
  * Apple iOS (i386, x86_64, ARM, and ARM64)
  * FreeBSD (i386, x86_64)


Building
--------

Build requirements include:

  * GNU make 3.80 or later
  * GCC 4.6 or later
      or LLVM Clang 3.1 or later (see use-clang option below)
  * JDK 1.6 or later
  * MinGW 3.4 or later (only if compiling for Windows)
  * zlib 1.2.3 or later

Earlier versions of some of these packages may also work but have not
been tested.

The build is directed by a single makefile and may be influenced via
certain flags described below, all of which are optional.

    $ make \
        platform={linux,windows,macosx,ios,freebsd} \
        arch={i386,x86_64,arm,arm64} \
        process={compile,interpret} \
        mode={debug,debug-fast,fast,small} \
        lzma=<lzma source directory> \
        bootimage={true,false} \
        tails={true,false} \
        continuations={true,false} \
        use-clang={true,false} \
        openjdk=<openjdk installation directory> \
        openjdk-src=<openjdk source directory> \
        android=<android source directory> \
        ios-version=<iOS minimum version>

    Using CLion With This Makefile
    ------------------------------

    This project uses a legacy Makefile with command substitutions in compile
    rules. CLion's Makefile parser may report warnings such as:

      Couldn't parse command line ...
      No input file specified

    Those warnings are often IDE parsing noise rather than real compiler errors.
    For accurate indexing and navigation in CLion, use a compilation database
    generated by [Bear](https://github.com/rizsotto/Bear):

      $ brew install bear
      $ bear --output compile_commands.json -- \
        make platform=macosx arch=arm64 mode=debug process=compile build

    Then open/import `compile_commands.json` in CLion (Compilation Database
    project). This bypasses the Makefile parser limitations.

    If your JDK is newer and the Makefile's auto Java version detection fails,
    override `java-version` explicitly when generating the database, for example:

      $ bear --output compile_commands.json -- \
        make platform=macosx arch=arm64 mode=debug process=compile \
        java-version=8 build

  * `platform` - the target platform
    * _default:_ output of $(uname -s | tr [:upper:] [:lower:]),
normalized in some cases (e.g. CYGWIN_NT-5.1 -> windows)

  * `arch` - the target architecture
    * _default:_ output of $(uname -m), normalized in some cases
(e.g. i686 -> i386)

  * `process` - choice between pure interpreter or JIT compiler
    * _default:_ compile

  * `mode` - which set of compilation flags to use to determine
optimization level, debug symbols, and whether to enable
assertions
    * _default:_ fast

  * `lzma` - if set, support use of LZMA to compress embedded JARs and
boot images.  The value of this option should be a directory
containing a recent LZMA SDK (available [here](http://www.7-zip.org/sdk.html)).  Currently, only version 9.20 of
the SDK has been tested, but other versions might work.
    * _default:_ not set

  * `armv6` - if true, don't use any instructions newer than armv6.  By
default, we assume the target is armv7 or later, and thus requires explicit
memory barrier instructions to ensure cache coherency

  * `bootimage` - if true, create a boot image containing the pre-parsed
class library and ahead-of-time compiled methods.  This option is
only valid for process=compile builds.  Note that you may need to
specify both build-arch=x86_64 and arch=x86_64 on 64-bit systems
where "uname -m" prints "i386".
    * _default:_ false

  * `tails` - if true, optimize each tail call by replacing the caller's
stack frame with the callee's.  This convention ensures proper
tail recursion, suitable for languages such as Scheme.  This
option is only valid for process=compile builds.
    * _default:_ false

  * `continuations` - if true, support continuations via the
avian.Continuations methods callWithCurrentContinuation and
dynamicWind.  See Continuations.java for details.  This option is
only valid for process=compile builds.
    * _default:_ false

  * `use-clang` - if true, use LLVM's clang instead of GCC to build.
Note that this does not currently affect cross compiles, only
native builds.
    * _default:_ false

  * `openjdk` - if set, use the OpenJDK class library instead of the
default Avian class library.  See "Building with the OpenJDK Class
Library" below for details.
    * _default:_ not set

  * `openjdk-src` - if this and the openjdk option above are both set,
build an embeddable VM using the OpenJDK class library.  The JNI
components of the OpenJDK class library will be built from the
sources found under the specified directory.  See "Building with
the OpenJDK Class Library" below for details.
    * _default:_ not set

  * `android` - if set, use the Android class library instead of the
default Avian class library.  See "Building with the Android Class
Library" below for details.
    * _default:_ not set

  * `ios-version` - the minimum iOS SDK version which will be used
when compiling for ios target. Do not use a value 11.0 or larger,
if you want to support 32 bit version. This option is only valid
for platform=ios .
    * _default:_ 8.0

These flags determine the name of the directory used for the build.
The name always starts with _${platform}-${arch}_, and each non-default
build option is appended to the name.  For example, a debug build with
bootimage enabled on Linux/x86_64 would be built in
_build/linux-x86_64-debug-bootimage_.  This allows you to build with
several different sets of options independently and even
simultaneously without doing a clean build each time.

Note that not all combinations of these flags are valid.  For instance,
non-jailbroken iOS devices do not allow JIT compilation, so only
process=interpret or bootimage=true builds will run on such
devices.  See [here](https://github.com/ReadyTalk/hello-ios) for an
example of an Xcode project for iOS which uses Avian.

If you are compiling for Windows, you may either cross-compile using
MinGW or build natively on Windows under Cygwin.

#### Installing Cygwin:

  __1.__ Download and run setup.exe from [cygwin's website](http://www.cygwin.com), installing the base
  system and these packages: make, gcc-mingw-g++,
  mingw64-i686-gcc-g++, mingw64-x86_64-gcc-g++, and (optionally) git.

You may also find our win32 repository useful: (run this from the
directory containing the avian directory)

    $ git clone git@github.com:ReadyTalk/win32.git

This gives you the Windows JNI headers, zlib headers and library, and
a few other useful libraries like OpenSSL, libjpeg, and libpng.
There's also a win64 repository for 64-bit builds:

      $ git clone git@github.com:ReadyTalk/win64.git


JIT Code Memory
---------------

With `process=compile`, generated machine code lives in a region owned by
`CodeMemory` (`include/avian/system/code-memory.h`), the only code in the
VM that writes to executable memory.  Code is assembled into ordinary
memory, linked against its final address, and then committed; call sites
are later retargeted through the same interface.  How the region is made
writable depends on the platform:

  * macOS: a `MAP_JIT` mapping.  On Apple silicon it is switched between
writable and executable per thread (`pthread_jit_write_protect_np`)
inside the commit and patch operations only.  The `avian` executable must
be signed with the `com.apple.security.cs.allow-jit` entitlement (the
makefile does this).
  * Linux: one memfd mapped twice, read+execute where the code runs and
read+write where it is written, so no page is ever writable and
executable at once.  If that isn't possible (no `memfd_create`,
`vm.memfd_noexec=2`, restrictive security policy) the VM silently falls
back to a single read/write/execute mapping.
  * Elsewhere: a single read/write/execute mapping.

The environment variable `AVIAN_CODE_MEMORY` forces a backend: `rwx`
everywhere, `dual-map` on Linux, `map-jit` on macOS.

**Debugging generated code on Linux:** debuggers set software
breakpoints by writing an instruction into the target with `ptrace`, and
the kernel refuses such a write to the dual-mapped executable view
(`EFAULT`: it is a shared mapping that isn't writable, so there is no
copy-on-write fallback).  Nothing is corrupted, but the breakpoint is not
planted.  gdb reports this as
`Cannot insert breakpoint N. Cannot access memory at address 0x...` for
an address outside any known object; for an address inside a registered
JIT method (see "Debugging JIT code" below) it instead shows the
breakpoint as `<PENDING>` in `info breakpoints` without an error, and it
never triggers.  So:

  * If the VM is started under a debugger (`gdb --args avian ...`,
`lldb -- avian ...`), it notices (`TracerPid` in `/proc/self/status`),
prints `avian: debugger detected; using rwx code memory ...` and uses the
read/write/execute backend, where software breakpoints work normally.
  * If a debugger attaches later, use hardware breakpoints (`hbreak` in
gdb, `breakpoint set -H` in lldb; a few are available), or restart the
VM under the debugger or with `AVIAN_CODE_MEMORY=rwx`.
  * `AVIAN_CODE_MEMORY` always takes precedence over the automatic choice.

Breakpoints in the VM itself are not affected.

Debugging JIT code
------------------

With `process=compile` the VM describes the code it generates to native
debuggers through the standard GDB JIT interface
(`__jit_debug_register_code` / `__jit_debug_descriptor`).  Each time a
method or thunk is published it registers a small in-memory ELF object
containing a symbol for it, named `Class.method(signature)` (for
example `java/lang/String.valueOf(I)Ljava/lang/String;`) or
`avian_thunk_<name>`, plus an `.eh_frame` describing its frame so that
backtraces continue through Java frames into the VM and back.  Code
loaded from a boot image is registered in one batch at startup.

gdb supports this out of the box:

    $ gdb --args build/linux-x86_64-debug/avian -cp classes Hello
    (gdb) break 'Hello.greet(Ljava/lang/String;)V'   # quotes required
    (gdb) run
    (gdb) bt
    #0  0x... in Hello.greet(Ljava/lang/String;)V ()
    #1  0x... in Hello.main([Ljava/lang/String;)V ()
    #2  0x... in vmInvoke ()
    #3  0x... in (anonymous namespace)::local::invoke (...) at src/compile.cpp:...
    (gdb) info symbol $pc
    Hello.greet(Ljava/lang/String;)V in section .text of <in-memory@0x...>

Breakpoints on methods that haven't been compiled yet are pending and
resolve when the method is compiled.  When attaching to a running VM on
Linux, use `hbreak` instead of `break` (see "Debugging generated code on
Linux" above).

lldb supports the same interface through its `jit-loader.gdb` plugin.
It is enabled by default on Linux; on macOS it must be turned on (before
`run` or attaching):

    (lldb) settings set plugin.jit-loader.gdb.enable on
    (lldb) breakpoint set -n 'Hello.greet(Ljava/lang/String;)V'
    (lldb) run
    (lldb) bt

Registration is controlled by `AVIAN_JIT_DEBUG_INFO`: `1` turns it on,
`0` off.  If unset, it is on in debug builds (`mode=debug`) and whenever
a debugger is already attached when the VM starts, and off otherwise.
Registration costs a few hundred bytes plus the name per compiled
method and, only while a debugger is attached, a debugger stop per
registration (roughly 0.5 ms each under gdb), which production runs
shouldn't pay; set `AVIAN_JIT_DEBUG_INFO=1` to debug a release build
you intend to attach to later.

Limitations:

  * There are no line tables or variable locations, only symbols and
frame (unwind) information.  Epilogues aren't described, so a backtrace
taken on a method's final instructions may be wrong.
  * Boot-image methods are described as if their frame were set up from
the first instruction, so a backtrace stopped inside the prologue of a
boot-image method is wrong; stopping at the entry of a method compiled
at run time is fine.  In boot-image builds those methods also appear a
second time under the executable's own static symbols.
  * Code is registered until the VM shuts down (compiled code is never
freed once published).
  * `__jit_debug_register_code` is a process-global symbol, so an
embedding that also links another JIT defining it will see a clash.

Building with the Microsoft Visual C++ Compiler
-----------------------------------------------

You can also build using the MSVC compiler, which makes debugging with
tools like WinDbg and Visual Studio much easier.  Note that you will
still need to have GCC installed - MSVC is only used to compile the
C++ portions of the VM, while the assembly code and helper tools are
built using GCC.

*Note that the MSVC build isn't tested regularly, so is fairly likely to be broken.*

Avian targets MSVC 11 and above (it uses c++ features not available in older versions).

To build with MSVC, install Cygwin as described above and set the
following environment variables:

    $ export PATH="/usr/local/bin:/usr/bin:/bin:/usr/X11R6/bin:/cygdrive/c/Program Files/Microsoft Visual Studio 11.0/Common7/IDE:/cygdrive/c/Program Files/Microsoft Visual Studio 11.0/VC/BIN:/cygdrive/c/Program Files/Microsoft Visual Studio 11.0/Common7/Tools:/cygdrive/c/WINDOWS/Microsoft.NET/Framework/v3.5:/cygdrive/c/WINDOWS/Microsoft.NET/Framework/v2.0.50727:/cygdrive/c/Program Files/Microsoft Visual Studio 11.0/VC/VCPackages:/cygdrive/c/Program Files/Microsoft SDKs/Windows/v6.0A/bin:/cygdrive/c/WINDOWS/system32:/cygdrive/c/WINDOWS:/cygdrive/c/WINDOWS/System32/Wbem"
    $ export LIBPATH="C:\WINDOWS\Microsoft.NET\Framework\v3.5;C:\WINDOWS\Microsoft.NET\Framework\v2.0.50727;C:\Program Files\Microsoft Visual Studio 11.0\VC\LIB;"
    $ export VCINSTALLDIR="C:\Program Files\Microsoft Visual Studio 11.0\VC"
    $ export LIB="C:\Program Files\Microsoft Visual Studio 11.0\VC\LIB;C:\Program Files\Microsoft SDKs\Windows\v6.0A\lib;"
    $ export INCLUDE="C:\Program Files\Microsoft Visual Studio 11.0\VC\INCLUDE;C:\Program Files\Microsoft SDKs\Windows\v6.0A\include;"

Adjust these definitions as necessary according to your MSVC
installation.

Finally, build with the msvc flag set to the MSVC tool directory:

    $ make msvc="/cygdrive/c/Program Files/Microsoft Visual Studio 11.0/VC"


Building with the OpenJDK Class Library
---------------------------------------

By default, Avian uses its own lightweight class library.  However,
that library only contains a relatively small subset of the classes
and methods included in the JRE.  If your application requires
features beyond that subset, you may want to tell Avian to use
OpenJDK's class library instead.  To do so, specify the directory
where OpenJDK is installed, e.g.:

    $ make openjdk=/usr/lib/jvm/java-7-openjdk

This will build Avian as a conventional JVM (e.g. libjvm.so) which
loads its boot class library and native libraries (e.g. libjava.so)
from _/usr/lib/jvm/java-7-openjdk/jre_ at runtime.  Note that you must
use an absolute path here, or else the result will not work when run
from other directories.  In this configuration, OpenJDK needs to
remain installed for Avian to work, and you can run applications like
this:

    $ build/linux-x86_64-openjdk/avian-dynamic -cp /path/to/my/application \
        com.example.MyApplication

Alternatively, you can enable a stand-alone build using OpenJDK by
specifying the location of the OpenJDK source code, e.g.:

    $ make openjdk=$(pwd)/../jdk7/build/linux-amd64/j2sdk-image \
        openjdk-src=$(pwd)/../jdk7/jdk/src

You must ensure that the path specified for openjdk-src does not have
any spaces in it; make gets confused when dependency paths include
spaces, and we haven't found away around that except to avoid paths
with spaces entirely.

The result of such a build is a self-contained binary which does not
depend on external libraries, jars, or other files.  In this case, the
specified paths are used only at build time; anything needed at
runtime is embedded in the binary.  Thus, the process of running an
application is simplified:

    $ build/linux-x86_64-openjdk-src/avian -cp /path/to/my/application \
        com.example.MyApplication

Note that the resulting binary will be very large due to the size of
OpenJDK's class library.  This can be mitigated using UPX, preferably
an LZMA-enabled version:

    $ upx --lzma --best build/linux-x86_64-openjdk-src/avian

You can reduce the size futher for embedded builds by using ProGuard
and the supplied openjdk.pro configuration file (see "Embedding with
ProGuard and a Boot Image" below).  Note that you'll still need to use
vm.pro in that case -- openjdk.pro just adds additional constraints
specific to the OpenJDK port.  Also see
[app.mk](https://github.com/ReadyTalk/avian-swt-examples/blob/master/app.mk)
in the _avian-swt-examples_ project for an example of using Avian,
OpenJDK, ProGuard, and UPX in concert.

Here are some examples of how to install OpenJDK and build Avian with
it on various OSes:

#### Debian-based Linux:
_Conventional build:_

    $ apt-get install openjdk-7-jdk
    $ make openjdk=/usr/lib/jvm/java-7-openjdk test

_Stand-alone build:_

    $ apt-get install openjdk-7-jdk
    $ apt-get source openjdk-7-jdk
    $ apt-get build-dep openjdk-7-jdk
    $ (cd openjdk-7-7~b147-2.0 && dpkg-buildpackage)
    $ make openjdk=/usr/lib/jvm/java-7-openjdk \
        openjdk-src=$(pwd)/openjdk-7-7~b147-2.0/build/openjdk/jdk/src \
        test

#### Mac OS X:
_Prerequisite:_ Build OpenJDK 7 according to [this site](https://wikis.oracle.com/display/OpenJDK/Mac+OS+X+Port).

_Conventional build:_

    $ make openjdk=$(pwd)/../jdk7u-dev/build/macosx-amd64/j2sdk-image test

_Stand-alone build:_

    $ make openjdk=$(pwd)/../jdk7u-dev/build/macosx-amd64/j2sdk-image \
        openjdk-src=$(pwd)/../p/jdk7u-dev/jdk/src test

#### Windows (Cygwin):
_Prerequisite:_ Build OpenJDK 7 according to [this site](http://weblogs.java.net/blog/simonis/archive/2011/10/28/yaojowbi-yet-another-openjdk-windows-build-instruction).  Alternatively, use https://github.com/alexkasko/openjdk-unofficial-builds.

_Conventional build:_

    $ make openjdk=$(pwd)/../jdk7u-dev/build/windows-i586/j2sdk-image test

_Stand-alone build:_

    $ make openjdk=$(pwd)/../jdk7u-dev/build/windows-i586/j2sdk-image \
        openjdk-src=$(pwd)/../p/jdk7u-dev/jdk/src test

Currently, only OpenJDK 7 is supported.  Later versions might work,
but have not yet been tested.


Building with the Android Class Library
---------------------------------------
As an alternative to both the Avian and OpenJDK class libaries, you
can also build with the Android class library. Now it should work on Linux, OS X and Windows.

The simpliest way to build Avian with Android classpath is to use `avian-pack` project: https://github.com/bigfatbrowncat/avian-pack

Avian-pack consists of Avian itself with some Android components (such as libcore and icu4c).

Note that we use the upstream OpenSSL repository and apply the
Android patches to it.  This is because it is not clear how to build
the Android fork of OpenSSL directly without checking out and building
the entire platform.  As of this writing, the patches apply cleanly
against OpenSSL 1.0.1h, so that's the tag we check out, but this may
change in the future when the Android fork rebases against a new
OpenSSL version.

Installing
----------

Installing Avian is as simple as copying the executable to the desired
directory:

    $ cp build/${platform}-${arch}/avian ~/bin/


Embedding
---------

The following series of commands illustrates how to produce a
stand-alone executable out of a Java application using Avian.

Note: if you are building on Cygwin, prepend "x86_64-w64-mingw32-" or
"i686-w64-mingw32-" to the ar, g++, gcc, strip, and dlltool commands
below (e.g. x86_64-w64-mingw32-gcc).

__1.__ Build Avian, create a new directory, and populate it with the
VM object files and bootstrap classpath jar.

    $ make
    $ mkdir hello
    $ cd hello
    $ ar x ../build/${platform}-${arch}/libavian.a
    $ cp ../build/${platform}-${arch}/classpath.jar boot.jar

__2.__ Build the Java code and add it to the jar.

    $ cat >Hello.java <<EOF
    public class Hello {
      public static void main(String[] args) {
        System.out.println("hello, world!");
      }
    }
    EOF
     $ javac -bootclasspath boot.jar Hello.java
     $ jar u0f boot.jar Hello.class

__3.__ Make an object file out of the jar.

    $ ../build/${platform}-${arch}/binaryToObject/binaryToObject boot.jar \
         boot-jar.o _binary_boot_jar_start _binary_boot_jar_end ${platform} ${arch}

If you've built Avian using the `lzma` option, you may optionally
compress the jar before generating the object:

      ../build/$(platform}-${arch}-lzma/lzma/lzma encode boot.jar boot.jar.lzma
         && ../build/${platform}-${arch}-lzma/binaryToObject/binaryToObject \
           boot.jar.lzma boot-jar.o _binary_boot_jar_start _binary_boot_jar_end \
           ${platform} ${arch}

Note that you'll need to specify "-Xbootclasspath:[lzma.bootJar]"
instead of "-Xbootclasspath:[bootJar]" in the next step if you've used
LZMA to compress the jar.

__4.__ Write a driver which starts the VM and runs the desired main
method.  Note the bootJar function, which will be called by the VM to
get a handle to the embedded jar.  We tell the VM about this jar by
setting the boot classpath to "[bootJar]".

    $ cat >embedded-jar-main.cpp <<EOF
    #include "stdint.h"
    #include "jni.h"
    #include "stdlib.h"

    #if (defined __MINGW32__) || (defined _MSC_VER)
    #  define EXPORT __declspec(dllexport)
    #else
    #  define EXPORT __attribute__ ((visibility("default"))) \
      __attribute__ ((used))
    #endif

    #if (! defined __x86_64__) && ((defined __MINGW32__) || (defined _MSC_VER))
    #  define SYMBOL(x) binary_boot_jar_##x
    #else
    #  define SYMBOL(x) _binary_boot_jar_##x
    #endif

    extern "C" {

      extern const uint8_t SYMBOL(start)[];
      extern const uint8_t SYMBOL(end)[];

      EXPORT const uint8_t*
      bootJar(size_t* size)
      {
        *size = SYMBOL(end) - SYMBOL(start);
        return SYMBOL(start);
      }

    } // extern "C"

    extern "C" void __cxa_pure_virtual(void) { abort(); }

    int
    main(int ac, const char** av)
    {
      JavaVMInitArgs vmArgs;
      vmArgs.version = JNI_VERSION_1_2;
      vmArgs.nOptions = 1;
      vmArgs.ignoreUnrecognized = JNI_TRUE;

      JavaVMOption options[vmArgs.nOptions];
      vmArgs.options = options;

      options[0].optionString = const_cast<char*>("-Xbootclasspath:[bootJar]");

      JavaVM* vm;
      void* env;
      JNI_CreateJavaVM(&vm, &env, &vmArgs);
      JNIEnv* e = static_cast<JNIEnv*>(env);

      jclass c = e->FindClass("Hello");
      if (not e->ExceptionCheck()) {
        jmethodID m = e->GetStaticMethodID(c, "main", "([Ljava/lang/String;)V");
        if (not e->ExceptionCheck()) {
          jclass stringClass = e->FindClass("java/lang/String");
          if (not e->ExceptionCheck()) {
            jobjectArray a = e->NewObjectArray(ac-1, stringClass, 0);
            if (not e->ExceptionCheck()) {
              for (int i = 1; i < ac; ++i) {
                e->SetObjectArrayElement(a, i-1, e->NewStringUTF(av[i]));
              }

              e->CallStaticVoidMethod(c, m, a);
            }
          }
        }
      }

      int exitCode = 0;
      if (e->ExceptionCheck()) {
        exitCode = -1;
        e->ExceptionDescribe();
      }

      vm->DestroyJavaVM();

      return exitCode;
    }
    EOF

__on Linux:__

     $ g++ -I$JAVA_HOME/include -I$JAVA_HOME/include/linux \
         -D_JNI_IMPLEMENTATION_ -c embedded-jar-main.cpp -o main.o

__on Mac OS X:__

     $ g++ -I$JAVA_HOME/include -I$JAVA_HOME/include/darwin \
         -D_JNI_IMPLEMENTATION_ -c embedded-jar-main.cpp -o main.o

__on Windows:__

     $ g++ -fno-exceptions -fno-rtti -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/win32" \
         -D_JNI_IMPLEMENTATION_ -c embedded-jar-main.cpp -o main.o

__5.__ Link the objects produced above to produce the final
executable, and optionally strip its symbols.

__on Linux:__

    $ g++ -rdynamic *.o -ldl -lpthread -lz -o hello
    $ strip --strip-all hello

__on Mac OS X:__

    $ g++ -rdynamic *.o -ldl -lpthread -lz -o hello -framework CoreFoundation
    $ strip -S -x hello

__on Windows:__

    $ dlltool -z hello.def *.o
    $ dlltool -d hello.def -e hello.exp
    $ gcc hello.exp *.o -L../../win32/lib -lmingwthrd -lm -lz -lws2_32 \
        -lIphlpapi -mwindows -mconsole -o hello.exe
    $ strip --strip-all hello.exe

Embedding with ProGuard and a Boot Image
----------------------------------------

The following illustrates how to embed an application as above, except
this time we preprocess the code using ProGuard and build a boot image
from it for quicker startup.  The pros and cons of using ProGuard are
as follow:

 * Pros: ProGuard will eliminate unused code, optimize the rest, and
   obfuscate it as well for maximum space savings

 * Cons: increased build time, especially for large applications, and
   extra effort needed to configure it for applications which rely
   heavily on reflection and/or calls to Java from native code

For boot image builds:

 * Pros: the boot image build pre-parses all the classes and compiles
   all the methods, obviating the need for JIT compilation at runtime.
   This also makes garbage collection faster, since the pre-parsed
   classes are never visited.

 * Cons: the pre-parsed classes and AOT-compiled methods take up more
   space in the executable than the equivalent class files.  In
   practice, this can make the executable 30-50% larger.  Also, AOT
   compilation does not yet yield significantly faster or smaller code
   than JIT compilation.  Finally, floating point code may be slower
   on 32-bit x86 since the compiler cannot assume SSE2 support will be
   available at runtime, and the x87 FPU is not supported except via
   out-of-line helper functions.

Note you can use ProGuard without using a boot image and vice-versa,
as desired.

The following instructions assume we are building for Linux/x86_64.
Please refer to the previous example for guidance on other platforms.

__1.__ Build Avian, create a new directory, and populate it with the
VM object files.

    $ make bootimage=true
    $ mkdir hello
    $ cd hello
    $ ar x ../build/linux-x86_64-bootimage/libavian.a

__2.__ Create a stage1 directory and extract the contents of the
class library jar into it.

    $ mkdir stage1
    $ (cd stage1 && jar xf ../../build/linux-x86_64-bootimage/classpath.jar)

__3.__ Build the Java code and add it to stage1.

     $ cat >Hello.java <<EOF
    public class Hello {
      public static void main(String[] args) {
        System.out.println("hello, world!");
      }
    }
    EOF
     $ javac -bootclasspath stage1 -d stage1 Hello.java

__4.__ Create a ProGuard configuration file specifying Hello.main as
the entry point.

     $ cat >hello.pro <<EOF
    -keep class Hello {
       public static void main(java.lang.String[]);
     }
    EOF

__5.__ Run ProGuard with stage1 as input and stage2 as output.

     $ java -jar ../../proguard4.6/lib/proguard.jar \
         -dontusemixedcaseclassnames -injars stage1 -outjars stage2 \
         @../vm.pro @hello.pro

(note: The -dontusemixedcaseclassnames option is only needed when
building on systems with case-insensitive filesystems such as Windows
and OS X.  Also, you'll need to add -ignorewarnings if you use the
OpenJDK class library since the openjdk-src build does not include all
the JARs from OpenJDK, and thus ProGuard will not be able to resolve
all referenced classes.  If you actually plan to use such classes at
runtime, you'll need to add them to stage1 before running ProGuard.
Finally, you'll need to add @../openjdk.pro to the above command when
using the OpenJDK library.)

__6.__ Build the boot and code images.

     $ ../build/linux-x86_64-bootimage/bootimage-generator \
        -cp stage2 \
        -bootimage bootimage-bin.o \
        -codeimage codeimage-bin.o \
        -hostvm ../build/linux-x86_64-interpret/libjvm.so

Note that you can override the default names for the start and end
symbols in the boot/code image by also passing:

    -bootimage-symbols my_bootimage_start:my_bootimage_end \
    -codeimage-symbols my_codeimage_start:my_codeimage_end

__7.__ Write a driver which starts the VM and runs the desired main
method.  Note the bootimageBin function, which will be called by the
VM to get a handle to the embedded boot image.  We tell the VM about
this function via the "avian.bootimage" property.

Note also that this example includes no resources besides class files.
If our application loaded resources such as images and properties
files via the classloader, we would also need to embed the jar file
containing them.  See the previous example for instructions.

    $ cat >bootimage-main.cpp <<EOF
    #include "stdint.h"
    #include "jni.h"

    #if (defined __MINGW32__) || (defined _MSC_VER)
    #  define EXPORT __declspec(dllexport)
    #else
    #  define EXPORT __attribute__ ((visibility("default")))
    #endif

    #if (! defined __x86_64__) && ((defined __MINGW32__) || (defined _MSC_VER))
    #  define BOOTIMAGE_BIN(x) binary_bootimage_bin_##x
    #  define CODEIMAGE_BIN(x) binary_codeimage_bin_##x
    #else
    #  define BOOTIMAGE_BIN(x) _binary_bootimage_bin_##x
    #  define CODEIMAGE_BIN(x) _binary_codeimage_bin_##x
    #endif

    extern "C" {

      extern const uint8_t BOOTIMAGE_BIN(start)[];
      extern const uint8_t BOOTIMAGE_BIN(end)[];

      EXPORT const uint8_t*
      bootimageBin(size_t* size)
      {
        *size = BOOTIMAGE_BIN(end) - BOOTIMAGE_BIN(start);
        return BOOTIMAGE_BIN(start);
      }

      extern const uint8_t CODEIMAGE_BIN(start)[];
      extern const uint8_t CODEIMAGE_BIN(end)[];

      EXPORT const uint8_t*
      codeimageBin(size_t* size)
      {
        *size = CODEIMAGE_BIN(end) - CODEIMAGE_BIN(start);
        return CODEIMAGE_BIN(start);
      }

    } // extern "C"

    int
    main(int ac, const char** av)
    {
      JavaVMInitArgs vmArgs;
      vmArgs.version = JNI_VERSION_1_2;
      vmArgs.nOptions = 2;
      vmArgs.ignoreUnrecognized = JNI_TRUE;

      JavaVMOption options[vmArgs.nOptions];
      vmArgs.options = options;

      options[0].optionString
        = const_cast<char*>("-Davian.bootimage=bootimageBin");

      options[1].optionString
        = const_cast<char*>("-Davian.codeimage=codeimageBin");

      JavaVM* vm;
      void* env;
      JNI_CreateJavaVM(&vm, &env, &vmArgs);
      JNIEnv* e = static_cast<JNIEnv*>(env);

      jclass c = e->FindClass("Hello");
      if (not e->ExceptionCheck()) {
        jmethodID m = e->GetStaticMethodID(c, "main", "([Ljava/lang/String;)V");
        if (not e->ExceptionCheck()) {
          jclass stringClass = e->FindClass("java/lang/String");
          if (not e->ExceptionCheck()) {
            jobjectArray a = e->NewObjectArray(ac-1, stringClass, 0);
            if (not e->ExceptionCheck()) {
              for (int i = 1; i < ac; ++i) {
                e->SetObjectArrayElement(a, i-1, e->NewStringUTF(av[i]));
              }

              e->CallStaticVoidMethod(c, m, a);
            }
          }
        }
      }

      int exitCode = 0;
      if (e->ExceptionCheck()) {
        exitCode = -1;
        e->ExceptionDescribe();
      }

      vm->DestroyJavaVM();

      return exitCode;
    }
    EOF

     $ g++ -I$JAVA_HOME/include -I$JAVA_HOME/include/linux \
         -D_JNI_IMPLEMENTATION_ -c bootimage-main.cpp -o main.o

__8.__ Link the objects produced above to produce the final
 executable, and optionally strip its symbols.

    $ g++ -rdynamic *.o -ldl -lpthread -lz -o hello
    $ strip --strip-all hello


Trademarks
----------

Oracle and Java are registered trademarks of Oracle and/or its
affiliates.  Other names may be trademarks of their respective owners.

The Avian project is not affiliated with Oracle.
