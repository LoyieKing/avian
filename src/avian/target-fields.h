/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_TARGET_FIELDS_H
#define AVIAN_TARGET_FIELDS_H

#ifdef TARGET_BYTES_PER_WORD
#if (TARGET_BYTES_PER_WORD == 8)

#define TARGET_THREAD_EXCEPTION 80
#define TARGET_THREAD_EXCEPTIONSTACKADJUSTMENT 2336
#define TARGET_THREAD_EXCEPTIONOFFSET 2344
#define TARGET_THREAD_EXCEPTIONHANDLER 2352

#define TARGET_THREAD_IP 2296
#define TARGET_THREAD_STACK 2304
#define TARGET_THREAD_NEWSTACK 2312
#define TARGET_THREAD_SCRATCH 2320
#define TARGET_THREAD_CONTINUATION 2328
#define TARGET_THREAD_TAILADDRESS 2360
#define TARGET_THREAD_VIRTUALCALLTARGET 2368
#define TARGET_THREAD_VIRTUALCALLINDEX 2376
#define TARGET_THREAD_HEAPIMAGE 2384
#define TARGET_THREAD_CODEIMAGE 2392
#define TARGET_THREAD_THUNKTABLE 2400
#define TARGET_THREAD_DYNAMICTABLE 2408
#define TARGET_THREAD_STACKLIMIT 2456

#elif(TARGET_BYTES_PER_WORD == 4)

// MyThread follows vm::Thread. The JDWP fields on Thread (debugSuspend
// through debugSnap) are 52 bytes on i386 (uint64 aligns to 4), which
// shifts every MyThread offset by that amount from the pre-debug layout.
// Measured with g++ -m32 offsetof: Thread is 2200 bytes, so ip is 2200.
#define TARGET_THREAD_EXCEPTION 44
#define TARGET_THREAD_EXCEPTIONSTACKADJUSTMENT 2220
#define TARGET_THREAD_EXCEPTIONOFFSET 2224
#define TARGET_THREAD_EXCEPTIONHANDLER 2228

#define TARGET_THREAD_IP 2200
#define TARGET_THREAD_STACK 2204
#define TARGET_THREAD_NEWSTACK 2208
#define TARGET_THREAD_SCRATCH 2212
#define TARGET_THREAD_CONTINUATION 2216
#define TARGET_THREAD_TAILADDRESS 2232
#define TARGET_THREAD_VIRTUALCALLTARGET 2236
#define TARGET_THREAD_VIRTUALCALLINDEX 2240
#define TARGET_THREAD_HEAPIMAGE 2244
#define TARGET_THREAD_CODEIMAGE 2248
#define TARGET_THREAD_THUNKTABLE 2252
#define TARGET_THREAD_DYNAMICTABLE 2256
#define TARGET_THREAD_STACKLIMIT 2280

#else
#error
#endif
#else
#error
#endif

#endif
