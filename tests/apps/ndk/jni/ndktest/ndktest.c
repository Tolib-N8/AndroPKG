// Plain C NDK library: what a typical JNI library does with bionic.
#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/configuration.h>
#include <android/log.h>
#include <android/looper.h>
#include <setjmp.h>
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

// libandroid / libjnigraphics: assets, configuration, looper, bitmap pixels.
static jstring platform(JNIEnv* env, jclass cls, jobject jam, jobject bitmap) {
    AAssetManager* am = AAssetManager_fromJava(env, jam);
    AAsset* a = AAssetManager_open(am, "hello.txt", AASSET_MODE_BUFFER);
    char text[64] = "";
    int n = a ? AAsset_read(a, text, sizeof text - 1) : -1;
    if (a) AAsset_close(a);
    AConfiguration* cfg = AConfiguration_new();
    AConfiguration_fromAssetManager(cfg, am);
    int density = AConfiguration_getDensity(cfg);
    AConfiguration_delete(cfg);
    AndroidBitmapInfo info;
    AndroidBitmap_getInfo(env, bitmap, &info);
    uint32_t* px;
    int locked = AndroidBitmap_lockPixels(env, bitmap, (void**)&px);
    for (uint32_t y = 0; locked == 0 && y < info.height; y++)
        for (uint32_t x = 0; x < info.width; x++) px[y * (info.stride / 4) + x] = 0xFF0000FF;  // ABGR: red
    if (locked == 0) AndroidBitmap_unlockPixels(env, bitmap);
    char out[256];
    snprintf(out, sizeof out, "asset=%s(%d) density>0=%d looper=%d bitmap=%ux%u fmt=%d", text, n, density > 0,
             ALooper_forThread() != NULL, info.width, info.height, info.format);
    return (*env)->NewStringUTF(env, out);
}

// Direct buffers made by native code, read and written by Java (libGDX pixmaps); setjmp/longjmp.
static unsigned char g_direct[16];
static jobject directbuf(JNIEnv* env, jclass cls) {
    for (int i = 0; i < 16; i++) g_direct[i] = (unsigned char)(i + 1);
    return (*env)->NewDirectByteBuffer(env, g_direct, sizeof g_direct);
}
static jmp_buf g_jmp;
static int g_depth;
static void jump_from_deep(int n) {
    if (n == 0) longjmp(g_jmp, 42);
    jump_from_deep(n - 1);
    g_depth++;  // never reached
}
static jstring directcheck(JNIEnv* env, jclass cls) {
    volatile int count = 0;
    int r = setjmp(g_jmp);
    count++;
    if (r == 0) jump_from_deep(5);
    char out[128];
    snprintf(out, sizeof out, "written=%02x%02x%02x%02x setjmp=%d count=%d", g_direct[0], g_direct[1], g_direct[2],
             g_direct[3], r, count);
    return (*env)->NewStringUTF(env, out);
}

// Stack arguments from ART (generic JNI): AAPCS64 8-byte slots for Android code.
static jstring stackargs(JNIEnv* env, jclass cls, jlong a, jlong b, jint c, jint d, jint e, jint f, jint g, jint h,
                         jint i, jint j, jbyte k, jshort l, jchar m, jfloat n, jdouble o) {
    char out[160];
    snprintf(out, sizeof out, "%lld %lld %d %d %d %d %d %d %d %d %d %d %d %.1f %.1f", (long long)a, (long long)b, c,
             d, e, f, g, h, i, j, k, l, m, n, o);
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
        {"platform", "(Landroid/content/res/AssetManager;Landroid/graphics/Bitmap;)Ljava/lang/String;", (void*)platform},
        {"directbuf", "()Ljava/nio/ByteBuffer;", (void*)directbuf},
        {"directcheck", "()Ljava/lang/String;", (void*)directcheck},
        {"stackargs", "(JJIIIIIIIIBSCFD)Ljava/lang/String;", (void*)stackargs},
    };
    if ((*env)->RegisterNatives(env, c, methods, 8) != 0) return JNI_ERR;
    __android_log_print(ANDROID_LOG_INFO, TAG, "JNI_OnLoad: %d methods, pi=%.3f, %s", 4, 3.14159, "registered");
    return JNI_VERSION_1_6;
}
