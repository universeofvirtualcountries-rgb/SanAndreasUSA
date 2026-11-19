// COMPILE-CHECK STUB - not shipped, not used by the Android build.
//
// The sandbox cannot download Google's NDK, so tools/arm_compile_check.sh
// cross-compiles every translation unit for aarch64 and armv7 with zig's musl
// sysroots to prove the *code* is ABI-clean on both targets. This header gives
// the compiler the JNI surface the mod uses. The real <jni.h> from the NDK is
// used by the GitHub Actions build, where the JNIEnv call table is authoritative.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef uint16_t jchar;
typedef int16_t jshort;
typedef int32_t jint;
typedef int64_t jlong;
typedef float jfloat;
typedef double jdouble;
typedef int32_t jsize;

typedef void *jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jthrowable;
typedef jobject jarray;
typedef jarray jbyteArray;
typedef jarray jbooleanArray;
typedef jarray jcharArray;
typedef jarray jshortArray;
typedef jarray jintArray;
typedef jarray jlongArray;
typedef jarray jfloatArray;
typedef jarray jdoubleArray;
typedef jarray jobjectArray;

typedef void *jmethodID;
typedef void *jfieldID;

struct _jobject;
struct JavaVM;

union jvalue {
    jboolean z;
    jbyte b;
    jchar c;
    jshort s;
    jint i;
    jlong j;
    jfloat f;
    jdouble d;
    jobject l;
};

struct JNIEnv {
    template <class... A> jclass FindClass(const char *, A...) { return nullptr; }
    template <class... A> jmethodID GetMethodID(jclass, const char *, const char *, A...) { return nullptr; }
    template <class... A> jmethodID GetStaticMethodID(jclass, const char *, const char *, A...) { return nullptr; }
    template <class... A> jfieldID GetFieldID(jclass, const char *, const char *, A...) { return nullptr; }

    template <class... A> jstring NewStringUTF(const char *, A...) { return nullptr; }
    template <class... A> const char *GetStringUTFChars(jstring, jboolean *, A...) { return nullptr; }
    template <class... A> void ReleaseStringUTFChars(jstring, const char *, A...) {}

    template <class... A> jobject NewObject(jclass, jmethodID, A...) { return nullptr; }
    template <class... A> jclass GetObjectClass(jobject, A...) { return nullptr; }

    template <class... A> jobject CallObjectMethod(jobject, jmethodID, A...) { return nullptr; }
    template <class... A> jboolean CallBooleanMethod(jobject, jmethodID, A...) { return 0; }
    template <class... A> jbyte CallByteMethod(jobject, jmethodID, A...) { return 0; }
    template <class... A> jchar CallCharMethod(jobject, jmethodID, A...) { return 0; }
    template <class... A> jshort CallShortMethod(jobject, jmethodID, A...) { return 0; }
    template <class... A> jint CallIntMethod(jobject, jmethodID, A...) { return 0; }
    template <class... A> jlong CallLongMethod(jobject, jmethodID, A...) { return 0; }
    template <class... A> jfloat CallFloatMethod(jobject, jmethodID, A...) { return 0; }
    template <class... A> jdouble CallDoubleMethod(jobject, jmethodID, A...) { return 0; }
    template <class... A> void CallVoidMethod(jobject, jmethodID, A...) {}

    template <class... A> jobject CallStaticObjectMethod(jclass, jmethodID, A...) { return nullptr; }
    template <class... A> jint CallStaticIntMethod(jclass, jmethodID, A...) { return 0; }
    template <class... A> void CallStaticVoidMethod(jclass, jmethodID, A...) {}

    template <class... A> jbyteArray NewByteArray(jsize, A...) { return nullptr; }
    template <class... A> jintArray NewIntArray(jsize, A...) { return nullptr; }
    template <class... A> jobjectArray NewObjectArray(jsize, jclass, jobject, A...) { return nullptr; }
    template <class... A> void SetObjectArrayElement(jobjectArray, jsize, jobject, A...) {}
    template <class... A> void GetByteArrayRegion(jbyteArray, jsize, jsize, jbyte *, A...) {}
    template <class... A> void SetByteArrayRegion(jbyteArray, jsize, jsize, const jbyte *, A...) {}

    template <class... A> jboolean ExceptionCheck(A...) { return 0; }
    template <class... A> void ExceptionDescribe(A...) {}
    template <class... A> void ExceptionClear(A...) {}
    template <class... A> jint ThrowNew(jclass, const char *, A...) { return 0; }

    template <class... A> void DeleteLocalRef(jobject, A...) {}
    template <class... A> void DeleteGlobalRef(jobject, A...) {}
    template <class... A> jobject NewGlobalRef(jobject, A...) { return nullptr; }

    template <class... A> jint GetJavaVM(JavaVM **, A...) { return 0; }
};

struct JavaVM {
    template <class... A> jint GetEnv(void **, jint, A...) { return -2; }
    template <class... A> jint AttachCurrentThread(JNIEnv **, void *, A...) { return -1; }
    template <class... A> jint DetachCurrentThread(A...) { return 0; }
    template <class... A> jint AttachCurrentThreadAsDaemon(JNIEnv **, void *, A...) { return -1; }
};

#define JNI_VERSION_1_6 0x00010006
#define JNI_OK 0
#define JNI_ERR (-1)
#define JNI_FALSE 0
#define JNI_TRUE 1
#define JNIEXPORT
#define JNICALL
