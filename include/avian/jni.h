/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_JNI_H_
#define AVIAN_JNI_H_

#include <jni.h>

#include "avian/state_capture.h"

#ifndef __cplusplus
#error AvianJniEnv extends the C++ JNIEnv
#endif

/* The JNIEnv* passed to a native is the VM thread. Its first word is the
   function table, which is also JNIEnv's first word, so the cast adds no
   fields and does not move the pointer:
     reinterpret_cast<AvianJniEnv*>(env)->NewStringFromUnmanagedMutf8(header)
   Do not add data members here. They would overlap the thread. */

struct AvianJniEnv : JNIEnv {
  /* header is unmanaged and immovable: a big-endian u2 byte length, then
     that many Modified UTF-8 bytes. The string stores the address and does
     not copy it. The caller keeps the memory alive for as long as the
     string is reachable. A null header returns null. The result is a
     local reference. */
  jstring NewStringFromUnmanagedMutf8(const void* header)
  {
    typedef jstring(JNICALL* Fn)(JNIEnv*, const void*);
    /* Four reserved words, then the standard JNI slots through GetModule. */
    const unsigned slot = 234;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, header);
  }

  /* Versioned ByteFun state bytes. A null object is a 2-byte array, not a
     null return. The bytes are produced in native memory; one Java array is
     allocated only as this call returns. emitState uses CaptureState instead. */
  jbyteArray SerializeGraph(jobject obj)
  {
    typedef jbyteArray(JNICALL* Fn)(JNIEnv*, jobject);
    const unsigned slot = 235;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, obj);
  }

  /* data is native and is not moved by the garbage collector. It includes
     the version byte. */
  jobject DeserializeGraph(const uint8_t* data, jint length)
  {
    typedef jobject(JNICALL* Fn)(JNIEnv*, const uint8_t*, jint);
    const unsigned slot = 236;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, data, length);
  }

  /* Zeroed instance of the class. It is never moved or freed. A null
     class returns null. The result is a local reference. */
  jobject NewUnmanagedObject(jclass clazz)
  {
    typedef jobject(JNICALL* Fn)(JNIEnv*, jclass);
    const unsigned slot = 237;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, clazz);
  }

  /* header is a big-endian u2 byte length, then that many Modified UTF-8
     bytes. The String and a copy of the bytes live on the unmanaged heap.
     A null header returns null. The result is a local reference. */
  jstring NewUnmanagedStringFromMutf8(const void* header)
  {
    typedef jstring(JNICALL* Fn)(JNIEnv*, const void*);
    const unsigned slot = 238;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, header);
  }

  /* header is already unmanaged and immovable: a big-endian u2 byte
     length, then that many Modified UTF-8 bytes. The String lives on the
     unmanaged heap and stores that address. The bytes are not copied.
     The caller keeps the memory alive. A null header returns null. */
  jstring NewUnmanagedStringFromUnmanagedMutf8(const void* header)
  {
    typedef jstring(JNICALL* Fn)(JNIEnv*, const void*);
    const unsigned slot = 240;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, header);
  }

  /* False for null. True when the object itself is on the unmanaged heap. */
  jboolean IsUnmanaged(jobject obj)
  {
    typedef jboolean(JNICALL* Fn)(JNIEnv*, jobject);
    const unsigned slot = 239;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, obj);
  }

  /* Nestable. Until PopUnmanagedAlloc, every allocation on this thread
     — instances, arrays, and constructors — comes from the unmanaged
     heap and is never moved or freed. Pop the same number of times.
     An unwind past the push restores the previous mode. */
  void PushUnmanagedAlloc()
  {
    typedef void(JNICALL* Fn)(JNIEnv*);
    const unsigned slot = 241;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    table[slot](this);
  }

  void PopUnmanagedAlloc()
  {
    typedef void(JNICALL* Fn)(JNIEnv*);
    const unsigned slot = 242;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    table[slot](this);
  }

  /* header is a big-endian u2 byte length, then that many Modified UTF-8
     bytes. The String lives on the unmanaged heap and stores that address.
     The bytes are not copied. The caller keeps the memory alive and does
     not write it. A null header returns null. */
  jstring NewReadOnlyUnmanagedStringFromMutf8(const void* header)
  {
    typedef jstring(JNICALL* Fn)(JNIEnv*, const void*);
    const unsigned slot = 243;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, header);
  }

  /* Same contract as NewReadOnlyUnmanagedStringFromMutf8. The header is
     already unmanaged. The bytes are not copied. */
  jstring NewReadOnlyUnmanagedStringFromUnmanagedMutf8(const void* header)
  {
    typedef jstring(JNICALL* Fn)(JNIEnv*, const void*);
    const unsigned slot = 244;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, header);
  }

  /* Payload of a read-only unmanaged string: the Modified UTF-8 bytes
     already stored for it, not a copy. byteLength receives the byte count.
     Returns null when the payload could move, and does not copy in that
     case. A null string returns null and writes zero when byteLength is
     set. */
  const char* GetReadOnlyUnmanagedStringUtfChars(jstring s, jint* byteLength)
  {
    typedef const char*(JNICALL* Fn)(JNIEnv*, jstring, jint*);
    const unsigned slot = 245;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, s, byteLength);
  }

  /* No buffer was allocated. This does not free chars. */
  void ReleaseReadOnlyUnmanagedStringUtfChars(jstring s, const char* chars)
  {
    typedef void(JNICALL* Fn)(JNIEnv*, jstring, const char*);
    const unsigned slot = 246;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    table[slot](this, s, chars);
  }

  /* headers[i] is a u2-prefixed Modified UTF-8 header, or null for a null
     slot. Each string aliases that header on the unmanaged heap. The
     array is a normal managed local reference. A negative count returns
     null. */
  jobjectArray NewReadOnlyUnmanagedStringArray(const void* const* headers, jsize count)
  {
    typedef jobjectArray(JNICALL* Fn)(JNIEnv*, const void* const*, jsize);
    const unsigned slot = 247;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, headers, count);
  }

  /* One VM entry. Copies plugin, the class binary name, and the optional
     state name into malloc buffers, and serializes state into body. The
     caller frees every non-null pointer with free. No Java array is
     allocated for the payload. Returns 0, or -1 when a Java exception is
     pending. */
  jint CaptureState(jstring plugin, jobject type, jstring name, jobject state, StateCapture* out)
  {
    typedef jint(JNICALL* Fn)(JNIEnv*, jstring, jobject, jstring, jobject, StateCapture*);
    const unsigned slot = 248;
    const Fn* table = reinterpret_cast<const Fn*>(functions);
    return table[slot](this, plugin, type, name, state, out);
  }
};

#endif /* AVIAN_JNI_H_ */
