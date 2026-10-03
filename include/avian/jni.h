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
#include <stdint.h>

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
     allocated only as this call returns. */
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
};

#endif /* AVIAN_JNI_H_ */
