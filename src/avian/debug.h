/* Copyright (c) 2026, Avian Contributors
   See license.txt for details. */

#ifndef AVIAN_DEBUG_H
#define AVIAN_DEBUG_H

#include <stdint.h>

// JDWP is a client of this layer.  The VM posts class-prepare,
// breakpoint and single-step events here; the JDWP server (and, later,
// any other agent) consumes them.  Metadata the debugger needs is
// copied out of the Java heap at class-prepare time so the JDWP thread
// never touches GC memory.
//
// Suspend model: cooperative and blocking, not preemptive.  A thread
// stops only when it calls checkpoint (every bytecode, when JDWP was
// enabled before that method was compiled or while interpreting) or
// safepoint (the existing idleIfNecessary / interpreter safePoint
// hooks, which is all AOT and already-compiled code can do).  The
// stopped thread enters the Idle state so GC can proceed, then waits
// on a monitor until its suspend count drops.

namespace vm {

class Thread;
class GcClass;
class GcMethod;

namespace debug {

bool enabled();

// Parse the option string of -Xrunjdwp: or -agentlib:jdwp= (no prefix).
// Call before the VM boots.  Unknown transport / server=n leaves JDWP off.
void configure(const char* options);

// Listen, and if suspend=y block the calling thread until a debugger
// resumes the VM.  Call at the end of boot, before the launcher runs.
void boot(Thread* t);

// VM is going away.  Sends VMDeath if a debugger is attached.
void shutdown(Thread* t);

// True when this thread should block at the next checkpoint.
bool mustSuspend(Thread* t);

// Block if mustSuspend.  Used from existing safepoints.
void safepoint(Thread* t);

// `method` is a GcMethod* for the interpreter, or null when `bits` is
// the per-bytecode bitset of a JIT method (see allocateBits).
void checkpoint(Thread* t, int32_t bci, uintptr_t bits, void* method);

// Code-memory bitset for a method that is about to be compiled.
// Returns the address of the byte array (length bytes), or null.
// The pointer is executable-view memory; only CodeMemory::patch may
// write it.  Layout of the block immediately before the return value
// is 16 bytes: uint64 site pointer, uint32 length, uint32 pad.
uint8_t* allocateBits(Thread* t, unsigned length);

// Remember which CodeMemory future bitsets and patches should use.
void setCodeMemory(void* codeMemory);

// After the compiler has assigned machine offsets, publish them.
// `map[bci]` is the machine offset of that bytecode, or -1.  The
// function takes ownership of `map`.  Call before the method's code
// object is replaced (so line numbers are still bytecode indices).
void publishCompiled(Thread* t, GcMethod* method, uint8_t* bits,
                     unsigned length, int32_t* map);

// Filled by the in-process stack walker (interpreter or JIT).
struct WalkerFrame {
  char className[256];
  char methodName[256];
  char spec[256];
  int32_t index;
};

typedef int (*Walker)(Thread* t, WalkerFrame* out, int max);
void registerWalker(Walker walker);

// Convert a machine instruction offset back to a bytecode index.
// Returns -1 if this method has no map (AOT, or compiled before JDWP).
int machineOffsetToBci(const char* className, const char* methodName,
                       const char* spec, int32_t machineOffset);

// RAII hook for resolveSystemClass.  Construct before the class lock
// is taken so the lock is released before the destructor runs.  arm()
// only for a class that was actually inserted.  The destructor copies
// metadata and, if a debugger is waiting for ClassPrepare, blocks.
class ClassPrepareNotifier {
 public:
  ClassPrepareNotifier(Thread* t, GcClass** slot);
  ~ClassPrepareNotifier();
  void arm();

 private:
  Thread* t;
  GcClass** slot;
  bool armed;
};

void noteClassStatus(Thread* t, GcClass* c, bool initialized, bool error);

}  // namespace debug
}  // namespace vm

#endif
