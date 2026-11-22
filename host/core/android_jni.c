/* android_jni.c -- what the Android host asks of Java (android/app/src/main/java/.../GTRacingActivity.java).
   Empty on Windows.

   gt_android_http: one HTTP(S) request with HttpURLConnection (the NDK has no TLS library; Windows uses WinHTTP,
   board.c).  The activity's class is found through SDL's activity object, so any thread can call it (a thread
   the JVM did not start cannot find an app class by its name). */
#if defined(__ANDROID__)
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "rt.h"

/* the status (-1: no answer); *reply: the body, NUL-ended, to free (at most max bytes) */
int gt_android_http(const char *method, const char *url, const char *body, int timeout_ms, char **reply, size_t max)
{
    *reply = NULL;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act)
        return -1;
    int status = -1;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "gtHttp",
                                                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)[B")
                      : NULL;
    if (!m) {
        (*env)->ExceptionClear(env);
        rt_log("[jni] GTRacingActivity.gtHttp is missing\n");
        goto out;
    }
    jstring jm = (*env)->NewStringUTF(env, method), ju = (*env)->NewStringUTF(env, url);
    jstring jb = body ? (*env)->NewStringUTF(env, body) : NULL;
    /* the answer: 4 bytes of status (big-endian), then the body */
    jbyteArray r = (jbyteArray)(*env)->CallStaticObjectMethod(env, cls, m, jm, ju, jb, (jint)timeout_ms);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        r = NULL;
    }
    if (r) {
        jsize n = (*env)->GetArrayLength(env, r);
        jbyte *b = n >= 4 ? (*env)->GetByteArrayElements(env, r, NULL) : NULL;
        if (b) {
            status = (int)((u32)(u8)b[0] << 24 | (u32)(u8)b[1] << 16 | (u32)(u8)b[2] << 8 | (u8)b[3]);
            size_t len = (size_t)(n - 4) < max ? (size_t)(n - 4) : max;
            char *out = malloc(len + 1);
            if (out) {
                memcpy(out, b + 4, len);
                out[len] = 0;
                *reply = out;
            }
            (*env)->ReleaseByteArrayElements(env, r, b, JNI_ABORT);
        }
        (*env)->DeleteLocalRef(env, r);
    }
    (*env)->DeleteLocalRef(env, jm);
    (*env)->DeleteLocalRef(env, ju);
    if (jb)
        (*env)->DeleteLocalRef(env, jb);
out:
    if (cls)
        (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return status;
}
#endif
