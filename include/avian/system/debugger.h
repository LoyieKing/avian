/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_SYSTEM_DEBUGGER_H
#define AVIAN_SYSTEM_DEBUGGER_H

namespace avian {
namespace system {

// Whether a debugger (or another ptrace-based tracer) is attached to
// this process right now: TracerPid in /proc/self/status on Linux,
// P_TRACED on Darwin.  False where that can't be determined.  This is
// a snapshot; a debugger may attach later.
bool debuggerAttached();

}  // namespace system
}  // namespace avian

#endif  // AVIAN_SYSTEM_DEBUGGER_H
