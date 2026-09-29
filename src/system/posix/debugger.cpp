/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include <avian/system/debugger.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace avian {
namespace system {

bool debuggerAttached()
{
#if defined(__linux__)
  FILE* status = fopen("/proc/self/status", "re");
  if (status == 0) {
    return false;
  }

  bool traced = false;
  char line[256];
  while (fgets(line, sizeof(line), status)) {
    if (strncmp(line, "TracerPid:", 10) == 0) {
      traced = strtol(line + 10, 0, 10) != 0;
      break;
    }
  }

  fclose(status);
  return traced;
#elif defined(__APPLE__)
  // Apple's documented check (Technical Q&A QA1361).
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  struct kinfo_proc info;
  memset(&info, 0, sizeof(info));
  size_t size = sizeof(info);
  if (sysctl(mib, 4, &info, &size, 0, 0) != 0) {
    return false;
  }
  return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
  return false;
#endif
}

}  // namespace system
}  // namespace avian
