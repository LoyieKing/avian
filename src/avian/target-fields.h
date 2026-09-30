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

// MyThread follows vm::Thread. heapEnd, three TLAB unsigneds, and
// TlabAverage (12 bytes) sit in Thread and shift every field below
// Thread::heap by 32. makeThread aborts if offsetof disagrees.
#define TARGET_THREAD_EXCEPTION 80
#define TARGET_THREAD_EXCEPTIONSTACKADJUSTMENT 2368
#define TARGET_THREAD_EXCEPTIONOFFSET 2376
#define TARGET_THREAD_EXCEPTIONHANDLER 2384

#define TARGET_THREAD_IP 2328
#define TARGET_THREAD_STACK 2336
#define TARGET_THREAD_NEWSTACK 2344
#define TARGET_THREAD_SCRATCH 2352
#define TARGET_THREAD_CONTINUATION 2360
#define TARGET_THREAD_TAILADDRESS 2392
#define TARGET_THREAD_VIRTUALCALLTARGET 2400
#define TARGET_THREAD_VIRTUALCALLINDEX 2408
#define TARGET_THREAD_HEAPIMAGE 2416
#define TARGET_THREAD_CODEIMAGE 2424
#define TARGET_THREAD_THUNKTABLE 2432
#define TARGET_THREAD_DYNAMICTABLE 2440
#define TARGET_THREAD_STACKLIMIT 2488

#elif(TARGET_BYTES_PER_WORD == 4)

// MyThread follows vm::Thread. The JDWP fields on Thread (debugSuspend
// through debugSnap) are 52 bytes on i386 (uint64 aligns to 4).
// heapEnd is one pointer here, so the TLAB fields add 28 bytes and ip
// moves from 2200 to 2228. makeThread aborts if offsetof disagrees.
#define TARGET_THREAD_EXCEPTION 44
#define TARGET_THREAD_EXCEPTIONSTACKADJUSTMENT 2248
#define TARGET_THREAD_EXCEPTIONOFFSET 2252
#define TARGET_THREAD_EXCEPTIONHANDLER 2256

#define TARGET_THREAD_IP 2228
#define TARGET_THREAD_STACK 2232
#define TARGET_THREAD_NEWSTACK 2236
#define TARGET_THREAD_SCRATCH 2240
#define TARGET_THREAD_CONTINUATION 2244
#define TARGET_THREAD_TAILADDRESS 2260
#define TARGET_THREAD_VIRTUALCALLTARGET 2264
#define TARGET_THREAD_VIRTUALCALLINDEX 2268
#define TARGET_THREAD_HEAPIMAGE 2272
#define TARGET_THREAD_CODEIMAGE 2276
#define TARGET_THREAD_THUNKTABLE 2280
#define TARGET_THREAD_DYNAMICTABLE 2284
#define TARGET_THREAD_STACKLIMIT 2308

#else
#error
#endif
#else
#error
#endif

#endif
