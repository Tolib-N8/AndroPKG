// Plain C NDK library: what a typical JNI library does with bionic.
#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "ndktest"

static jstring hello(JNIEnv* env, jclass cls, jint n, jdouble d) {
    char* buf = malloc(256);
    snprintf(buf, 256, "n=%d d=%.2f hex=%#x str=%s big=%lld", n, d, n, "ok", 1LL << 40);
    jstring s = (*env)->NewStringUTF(env, buf);
    free(buf);
    return s;
}

static jint callback(JNIEnv* env, jclass cls, jobject activity) {
    jclass c = (*env)->GetObjectClass(env, activity);
    jmethodID m = (*env)->GetMethodID(env, c, "combine", "(IJFLjava/lang/String;)I");
    jstring s = (*env)->NewStringUTF(env, "abcd");
    // 3 + 4 + 25 + 4 = 36
    return (*env)->CallIntMethod(env, activity, m, 3, (jlong)4, 2.5f, s);
}

typedef struct { pthread_mutex_t m; pthread_cond_t c; int sum, done; } shared_t;
static shared_t g = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};

static void* worker(void* arg) {
    int v = (int)(long)arg;
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&g.m);
        g.sum += v;
        pthread_mutex_unlock(&g.m);
    }
    pthread_mutex_lock(&g.m);
    g.done++;
    pthread_cond_signal(&g.c);
    pthread_mutex_unlock(&g.m);
    return NULL;
}

static jint threads(JNIEnv* env, jclass cls, jint n) {
    pthread_t t[16];
    for (int i = 0; i < n; i++) pthread_create(&t[i], NULL, worker, (void*)(long)(i + 1));
    pthread_mutex_lock(&g.m);
    while (g.done < n) pthread_cond_wait(&g.c, &g.m);
    pthread_mutex_unlock(&g.m);
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    return g.sum;  // 1000 * (1 + 2 + ... + n)
}

static jstring files(JNIEnv* env, jclass cls, jstring jdir) {
    const char* dir = (*env)->GetStringUTFChars(env, jdir, NULL);
    char path[512], out[256];
    snprintf(path, sizeof path, "%s/native.txt", dir);
    FILE* f = fopen(path, "w");
    fprintf(f, "written by %s pid>0=%d\n", "ndktest", getpid() > 0);
    fclose(f);
    struct stat st;
    int r = stat(path, &st);
    int entries = 0;
    DIR* d = opendir(dir);
    for (struct dirent* e; d && (e = readdir(d));) if (e->d_name[0] != '.') entries++;
    if (d) closedir(d);
    errno = 0;
    int missing = open("/nonexistent/file", 0);
    snprintf(out, sizeof out, "stat=%d size=%lld reg=%d entries>0=%d enoent=%d", r, (long long)st.st_size,
             S_ISREG(st.st_mode), entries > 0, missing < 0 && errno == ENOENT);
    (*env)->ReleaseStringUTFChars(env, jdir, dir);
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass c = (*env)->FindClass(env, "dev/andropkg/test/ndk/MainActivity");
    static const JNINativeMethod methods[] = {
        {"hello", "(ID)Ljava/lang/String;", (void*)hello},
        {"callback", "(Ldev/andropkg/test/ndk/MainActivity;)I", (void*)callback},
        {"threads", "(I)I", (void*)threads},
        {"files", "(Ljava/lang/String;)Ljava/lang/String;", (void*)files},
    };
    if ((*env)->RegisterNatives(env, c, methods, 4) != 0) return JNI_ERR;
    __android_log_print(ANDROID_LOG_INFO, TAG, "JNI_OnLoad: %d methods, pi=%.3f, %s", 4, 3.14159, "registered");
    return JNI_VERSION_1_6;
}
