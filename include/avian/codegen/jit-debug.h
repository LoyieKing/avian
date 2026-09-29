/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_CODEGEN_JIT_DEBUG_H
#define AVIAN_CODEGEN_JIT_DEBUG_H

#include <stddef.h>
#include <stdint.h>

#include <avian/util/allocator.h>

namespace avian {
namespace codegen {

// A piece of generated code as a debugger should see it.
struct JitSymbol {
  // NUL-terminated; copied.
  const char* name;
  const uint8_t* start;
  size_t size;

  // Frame layout, for the debugger's unwinder.  From `prologueSize`
  // bytes into the code on, the canonical frame address (the stack
  // pointer before the call that entered this code) is the stack
  // pointer plus `frameSize` bytes, and the return address is saved in
  // the word just below it.  Before that, and everywhere if `frameSize`
  // is 0, the state at entry holds (return address on the stack on x86,
  // in the link register on ARM).  Epilogues are not described.
  unsigned prologueSize;
  unsigned frameSize;
};

// Publishes generated code to debuggers through the GDB JIT interface
// (__jit_debug_register_code / __jit_debug_descriptor), which gdb
// supports natively and lldb through its jit-loader.gdb plugin.
//
// Each call to add() builds one small in-memory ELF relocatable object
// for the host architecture holding a symbol per JitSymbol (in a
// SHT_NOBITS .text placed at the code's address) and an .eh_frame with
// one FDE per symbol, and registers it.  dispose() unregisters
// everything.  Code must only be added once it has been committed and
// published, and must stay in place until dispose().
//
// Thread safety: add() and dispose() may be called from any thread;
// registrations are serialized process-wide, since the descriptor is a
// process global shared by every VM in the process.
class JitDebugInfo {
 public:
  virtual void add(const JitSymbol* symbols, unsigned count) = 0;
  virtual void dispose() = 0;

 protected:
  ~JitDebugInfo()
  {
  }
};

// Returns a JitDebugInfo if debug info should be registered, or null.
// The environment variable AVIAN_JIT_DEBUG_INFO decides when set ("1"
// on, "0" off).  Otherwise it is on in builds with assertions (debug
// builds) and whenever a debugger is attached when the VM starts, and
// off otherwise: registering costs a few hundred bytes and a little
// time per compiled method, which production runs shouldn't pay for,
// while a debugger that attaches later still finds everything
// registered in debug builds.
JitDebugInfo* makeJitDebugInfo(util::Alloc* allocator);

}  // namespace codegen
}  // namespace avian

#endif  // AVIAN_CODEGEN_JIT_DEBUG_H
