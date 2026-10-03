#include <jni.h>
#include "jni-util.h"
#include "avian/jni.h"

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*)
{
  JNIEnv* e;
  if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) {
    return -1;
  }

  jclass c = e->FindClass("JNI");
  if (c == 0) {
    return -1;
  }

  e->SetStaticBooleanField(
      c, e->GetStaticFieldID(c, "onLoadCalled", "Z"), true);

  return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jdouble JNICALL Java_JNI_addDoubles(JNIEnv*,
                                                         jclass,
                                                         jdouble a1,
                                                         jdouble a2,
                                                         jdouble a3,
                                                         jdouble a4,
                                                         jdouble a5,
                                                         jdouble a6,
                                                         jdouble a7,
                                                         jdouble a8,
                                                         jdouble a9,
                                                         jdouble a10,
                                                         jdouble a11,
                                                         jdouble a12,
                                                         jdouble a13,
                                                         jdouble a14,
                                                         jdouble a15,
                                                         jdouble a16,
                                                         jdouble a17,
                                                         jdouble a18,
                                                         jdouble a19,
                                                         jdouble a20)
{
  return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13
         + a14 + a15 + a16 + a17 + a18 + a19 + a20;
}

extern "C" JNIEXPORT jfloat JNICALL Java_JNI_addFloats(JNIEnv*,
                                                       jclass,
                                                       jfloat a1,
                                                       jfloat a2,
                                                       jfloat a3,
                                                       jfloat a4,
                                                       jfloat a5,
                                                       jfloat a6,
                                                       jfloat a7,
                                                       jfloat a8,
                                                       jfloat a9,
                                                       jfloat a10,
                                                       jfloat a11,
                                                       jfloat a12,
                                                       jfloat a13,
                                                       jfloat a14,
                                                       jfloat a15,
                                                       jfloat a16,
                                                       jfloat a17,
                                                       jfloat a18,
                                                       jfloat a19,
                                                       jfloat a20)
{
  return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13
         + a14 + a15 + a16 + a17 + a18 + a19 + a20;
}

extern "C" JNIEXPORT jdouble JNICALL Java_JNI_addMix(JNIEnv*,
                                                     jclass,
                                                     jfloat a1,
                                                     jdouble a2,
                                                     jfloat a3,
                                                     jdouble a4,
                                                     jfloat a5,
                                                     jfloat a6,
                                                     jfloat a7,
                                                     jfloat a8,
                                                     jfloat a9,
                                                     jfloat a10,
                                                     jfloat a11,
                                                     jfloat a12,
                                                     jfloat a13,
                                                     jfloat a14,
                                                     jfloat a15,
                                                     jdouble a16,
                                                     jfloat a17,
                                                     jfloat a18,
                                                     jfloat a19,
                                                     jfloat a20)
{
  return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13
         + a14 + a15 + a16 + a17 + a18 + a19 + a20;
}

extern "C" JNIEXPORT jint JNICALL
    Java_JNI_addStackBoundary2(JNIEnv*, jclass, jobject, jobject, jobject,
                               jint i1, jint i2)
{
  return i1 + i2;
}

extern "C" JNIEXPORT jint JNICALL
    Java_JNI_addStackBoundary3(JNIEnv*, jclass, jobject, jobject, jobject,
                               jint i1, jint i2, jint i3)
{
  return i1 + i2 + i3;
}

extern "C" JNIEXPORT jint JNICALL
    Java_JNI_addStackBoundary4(JNIEnv*, jclass, jobject, jobject, jobject,
                               jint i1, jint i2, jint i3, jint i4)
{
  return i1 + i2 + i3 + i4;
}

extern "C" JNIEXPORT jint JNICALL
    Java_JNI_addStackBoundary5(JNIEnv*, jclass, jobject, jobject, jobject,
                               jint i1, jint i2, jint i3, jint i4, jint i5)
{
  return i1 + i2 + i3 + i4 + i5;
}

extern "C" JNIEXPORT jint JNICALL
    Java_JNI_addStackBoundary6(JNIEnv*, jclass, jobject, jobject, jobject,
                               jint i1, jint i2, jint i3, jint i4, jint i5, jint i6)
{
  return i1 + i2 + i3 + i4 + i5 + i6;
}

extern "C" JNIEXPORT jfloat JNICALL
    Java_JNI_doEcho__F(JNIEnv* e, jclass c, jfloat f)
{
  jvalue value;
  value.f = f;
  jvalue array[] = {value};
  return e->CallStaticFloatMethodA(
      c, e->GetStaticMethodID(c, "echo", "(F)F"), array);
}

extern "C" JNIEXPORT jdouble JNICALL
    Java_JNI_doEcho__D(JNIEnv* e, jclass c, jdouble f)
{
  jvalue value;
  value.d = f;
  jvalue array[] = {value};
  return e->CallStaticDoubleMethodA(
      c, e->GetStaticMethodID(c, "echo", "(D)D"), array);
}

extern "C" JNIEXPORT jlong JNICALL
    Java_JNI_fromReflectedMethod(JNIEnv* e, jclass, jobject method)
{
  return reinterpret_cast<uintptr_t>(e->FromReflectedMethod(method));
}

extern "C" JNIEXPORT jobject JNICALL
    Java_JNI_toReflectedMethod(JNIEnv* e,
                               jclass,
                               jclass c,
                               jlong id,
                               jboolean isStatic)
{
  return e->ToReflectedMethod(c, reinterpret_cast<jmethodID>(id), isStatic);
}

extern "C" JNIEXPORT jint JNICALL
    Java_JNI_callStaticIntMethod(JNIEnv* e, jclass, jclass c, jlong id)
{
  return e->CallStaticIntMethod(c, reinterpret_cast<jmethodID>(id));
}

extern "C" JNIEXPORT jobject JNICALL
    Java_JNI_newObject(JNIEnv* e, jclass, jclass c, jlong id)
{
  return e->NewObject(c, reinterpret_cast<jmethodID>(id));
}

extern "C" JNIEXPORT jlong JNICALL
    Java_JNI_fromReflectedField(JNIEnv* e, jclass, jobject field)
{
  return reinterpret_cast<uintptr_t>(e->FromReflectedField(field));
}

extern "C" JNIEXPORT jobject JNICALL
    Java_JNI_toReflectedField(JNIEnv* e,
                              jclass,
                              jclass c,
                              jlong id,
                              jboolean isStatic)
{
  return e->ToReflectedField(c, reinterpret_cast<jfieldID>(id), isStatic);
}

extern "C" JNIEXPORT jint JNICALL
    Java_JNI_getStaticIntField(JNIEnv* e, jclass, jclass c, jlong id)
{
  return e->GetStaticIntField(c, reinterpret_cast<jfieldID>(id));
}

extern "C" JNIEXPORT jobject JNICALL
    Java_JNI_testLocalRef(JNIEnv* e, jclass, jobject o)
{
  return e->NewLocalRef(o);
}

extern "C" JNIEXPORT jstring JNICALL
    Java_JNI_newUnmanaged(JNIEnv* e, jclass, jbyteArray bytes)
{
  if (bytes == 0) {
    return reinterpret_cast<AvianJniEnv*>(e)->NewStringFromUnmanagedMutf8(0);
  }

  jsize n = e->GetArrayLength(bytes);
  if (n < 0 || n > 65535) {
    return 0;
  }
  uint8_t* header = static_cast<uint8_t*>(malloc(static_cast<size_t>(n) + 2));
  if (header == 0) {
    return 0;
  }
  header[0] = static_cast<uint8_t>(static_cast<unsigned>(n) >> 8);
  header[1] = static_cast<uint8_t>(n);
  if (n) {
    e->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(header + 2));
  }
  return reinterpret_cast<AvianJniEnv*>(e)->NewStringFromUnmanagedMutf8(header);
}

extern "C" JNIEXPORT jobject JNICALL
    Java_JNI_newUnmanagedObject(JNIEnv* e, jclass, jclass c)
{
  return reinterpret_cast<AvianJniEnv*>(e)->NewUnmanagedObject(c);
}

extern "C" JNIEXPORT jstring JNICALL
    Java_JNI_newUnmanagedString(JNIEnv* e, jclass, jbyteArray bytes)
{
  if (bytes == 0) {
    return reinterpret_cast<AvianJniEnv*>(e)->NewUnmanagedStringFromMutf8(0);
  }

  jsize n = e->GetArrayLength(bytes);
  if (n < 0 || n > 65535) {
    return 0;
  }
  uint8_t* header = static_cast<uint8_t*>(malloc(static_cast<size_t>(n) + 2));
  if (header == 0) {
    return 0;
  }
  header[0] = static_cast<uint8_t>(static_cast<unsigned>(n) >> 8);
  header[1] = static_cast<uint8_t>(n);
  if (n) {
    e->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(header + 2));
  }
  jstring result
      = reinterpret_cast<AvianJniEnv*>(e)->NewUnmanagedStringFromMutf8(header);
  free(header);
  return result;
}

extern "C" JNIEXPORT jstring JNICALL
    Java_JNI_newUnmanagedStringFromUnmanaged(JNIEnv* e, jclass, jbyteArray bytes)
{
  if (bytes == 0) {
    return reinterpret_cast<AvianJniEnv*>(e)
        ->NewUnmanagedStringFromUnmanagedMutf8(0);
  }

  jsize n = e->GetArrayLength(bytes);
  if (n < 0 || n > 65535) {
    return 0;
  }
  uint8_t* header = static_cast<uint8_t*>(malloc(static_cast<size_t>(n) + 2));
  if (header == 0) {
    return 0;
  }
  header[0] = static_cast<uint8_t>(static_cast<unsigned>(n) >> 8);
  header[1] = static_cast<uint8_t>(n);
  if (n) {
    e->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(header + 2));
  }
  return reinterpret_cast<AvianJniEnv*>(e)
      ->NewUnmanagedStringFromUnmanagedMutf8(header);
}

extern "C" JNIEXPORT jboolean JNICALL
    Java_JNI_mallocIsUnmanaged(JNIEnv*, jclass)
{
  void* block = malloc(32);
  void* object = ::operator new(32);
  bool tagged = block != 0 && object != 0
                && ((reinterpret_cast<uintptr_t>(block) >> 42) == 8)
                && ((reinterpret_cast<uintptr_t>(object) >> 42) == 8);
  free(block);
  ::operator delete(object);
  return tagged;
}

extern "C" JNIEXPORT jboolean JNICALL
    Java_JNI_isUnmanaged(JNIEnv* e, jclass, jobject o)
{
  return reinterpret_cast<AvianJniEnv*>(e)->IsUnmanaged(o);
}

extern "C" JNIEXPORT jobject JNICALL
    Java_Buffers_allocateNative(JNIEnv* e, jclass, jint capacity)
{
  void* p = allocate(e, capacity);
  if (p == 0)
    return 0;

  return e->NewDirectByteBuffer(p, capacity);
}

extern "C" JNIEXPORT void JNICALL
    Java_Buffers_freeNative(JNIEnv* e, jclass, jobject b)
{
  free(e->GetDirectBufferAddress(b));
}
