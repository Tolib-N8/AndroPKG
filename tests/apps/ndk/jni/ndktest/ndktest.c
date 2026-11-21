// Plain C NDK library: what a typical JNI library does with bionic.
#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/configuration.h>
#include <android/log.h>
#include <android/looper.h>
#include <setjmp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <dlfcn.h>
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

// AAudio (libaaudio, looked up at run time: the test builds for API 24): a callback stream and a
// blocking write stream, both silent.
static _Atomic int g_aa_callbacks;
static int32_t aa_cb(void* stream, void* user, void* audio, int32_t frames) {
    memset(audio, 0, (size_t)frames * 2 * sizeof(float));
    g_aa_callbacks++;
    return 0;  // CONTINUE
}
static jstring aaudio(JNIEnv* env, jclass cls) {
    void* h = dlopen("libaaudio.so", RTLD_NOW);
    if (!h) return (*env)->NewStringUTF(env, "no libaaudio");
#define F(ret, name, ...) ret (*name)(__VA_ARGS__) = dlsym(h, #name)
    F(int32_t, AAudio_createStreamBuilder, void**);
    F(void, AAudioStreamBuilder_setFormat, void*, int32_t);
    F(void, AAudioStreamBuilder_setChannelCount, void*, int32_t);
    F(void, AAudioStreamBuilder_setDataCallback, void*, void*, void*);
    F(int32_t, AAudioStreamBuilder_openStream, void*, void**);
    F(int32_t, AAudioStreamBuilder_delete, void*);
    F(int32_t, AAudioStream_requestStart, void*);
    F(int32_t, AAudioStream_requestStop, void*);
    F(int32_t, AAudioStream_getState, void*);
    F(int32_t, AAudioStream_getSampleRate, void*);
    F(int64_t, AAudioStream_getFramesWritten, void*);
    F(int32_t, AAudioStream_write, void*, const void*, int32_t, int64_t);
    F(int32_t, AAudioStream_close, void*);
    F(const char*, AAudio_convertStreamStateToText, int32_t);
#undef F
    void *b, *cbStream, *wStream;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setFormat(b, 2);  // FLOAT
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setDataCallback(b, (void*)aa_cb, NULL);
    int32_t r1 = AAudioStreamBuilder_openStream(b, &cbStream);
    int32_t r2 = AAudioStream_requestStart(cbStream);
    usleep(300 * 1000);
    AAudioStream_requestStop(cbStream);
    const char* st = AAudio_convertStreamStateToText(AAudioStream_getState(cbStream));
    int64_t cbWritten = AAudioStream_getFramesWritten(cbStream);
    AAudioStream_close(cbStream);
    AAudioStreamBuilder_setFormat(b, 1);  // I16
    AAudioStreamBuilder_setDataCallback(b, NULL, NULL);
    int32_t r3 = AAudioStreamBuilder_openStream(b, &wStream);
    int32_t rate = AAudioStream_getSampleRate(wStream);
    AAudioStream_requestStart(wStream);
    static int16_t pcm[4800 * 2];
    int32_t wrote = AAudioStream_write(wStream, pcm, 4800, 1000000000LL);
    AAudioStream_close(wStream);
    AAudioStreamBuilder_delete(b);
    char out[160];
    snprintf(out, sizeof out, "open=%d,%d start=%d callbacks>0=%d frames>0=%d state=%s rate=%d write=%d", r1, r3, r2,
             g_aa_callbacks > 0, cbWritten > 0, st, rate, wrote);
    return (*env)->NewStringUTF(env, out);
}

// OpenSL ES: engine, output mix, a buffer-queue PCM player (silent) whose callback re-enqueues.
static _Atomic int g_sl_callbacks;
static int16_t g_sl_buf[2][1024 * 2];
static void sl_cb(SLAndroidSimpleBufferQueueItf bq, void* ctx) {
    int n = ++g_sl_callbacks;
    (*bq)->Enqueue(bq, g_sl_buf[n & 1], sizeof g_sl_buf[0]);
}
static jstring opensl(JNIEnv* env, jclass cls) {
    SLObjectItf engineObj, mixObj, playerObj;
    SLEngineItf engine;
    SLresult r1 = slCreateEngine(&engineObj, 0, NULL, 0, NULL, NULL);
    (*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE);
    (*engineObj)->GetInterface(engineObj, SL_IID_ENGINE, &engine);
    SLresult r2 = (*engine)->CreateOutputMix(engine, &mixObj, 0, NULL, NULL);
    (*mixObj)->Realize(mixObj, SL_BOOLEAN_FALSE);
    SLDataLocator_AndroidSimpleBufferQueue loc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM fmt = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16,
                            SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                            SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource src = {&loc, &fmt};
    SLDataLocator_OutputMix outLoc = {SL_DATALOCATOR_OUTPUTMIX, mixObj};
    SLDataSink sink = {&outLoc, NULL};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
    const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    SLresult r3 = (*engine)->CreateAudioPlayer(engine, &playerObj, &src, &sink, 2, ids, req);
    SLresult r4 = (*playerObj)->Realize(playerObj, SL_BOOLEAN_FALSE);
    SLPlayItf play;
    SLAndroidSimpleBufferQueueItf bq;
    SLVolumeItf vol;
    (*playerObj)->GetInterface(playerObj, SL_IID_PLAY, &play);
    (*playerObj)->GetInterface(playerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &bq);
    (*playerObj)->GetInterface(playerObj, SL_IID_VOLUME, &vol);
    (*bq)->RegisterCallback(bq, sl_cb, NULL);
    (*vol)->SetVolumeLevel(vol, -300);
    SLmillibel level = 0;
    (*vol)->GetVolumeLevel(vol, &level);
    (*bq)->Enqueue(bq, g_sl_buf[0], sizeof g_sl_buf[0]);
    SLresult r5 = (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
    usleep(300 * 1000);
    (*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED);
    SLAndroidSimpleBufferQueueState st;
    (*bq)->GetState(bq, &st);
    SLresult r6 = (*engine)->CreateAudioRecorder(engine, &mixObj, &src, &sink, 0, NULL, NULL);
    (*playerObj)->Destroy(playerObj);
    (*mixObj)->Destroy(mixObj);
    (*engineObj)->Destroy(engineObj);
    char out[160];
    snprintf(out, sizeof out, "results=%u%u%u%u%u callbacks>0=%d played>0=%d volume=%d recorder=%u", r1, r2, r3, r4, r5,
             g_sl_callbacks > 0, st.index > 0, level, r6);
    return (*env)->NewStringUTF(env, out);
}

// Sockets (Linux ABI: sockaddrs, options, flags), getaddrinfo, epoll + eventfd.
static jstring net(JNIEnv* env, jclass cls) {
    int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    int r1 = setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r2 = bind(srv, (struct sockaddr*)&a, sizeof a);
    listen(srv, 4);
    socklen_t al = sizeof a;
    getsockname(srv, (struct sockaddr*)&a, &al);
    int cli = socket(AF_INET, SOCK_STREAM, 0);
    int r3 = connect(cli, (struct sockaddr*)&a, sizeof a);
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int acc = accept4(srv, (struct sockaddr*)&peer, &pl, SOCK_NONBLOCK);
    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int efd = eventfd(0, EFD_NONBLOCK);
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 1};
    epoll_ctl(ep, EPOLL_CTL_ADD, acc, &ev);
    ev.data.u64 = 2;
    epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
    struct epoll_event got[4];
    int none = epoll_wait(ep, got, 4, 0);
    send(cli, "ping", 4, MSG_NOSIGNAL);
    eventfd_write(efd, 5);
    int n = 0, seen = 0;
    for (int tries = 0; tries < 10 && seen != 3; tries++) {
        n = epoll_wait(ep, got, 4, 100);
        for (int i = 0; i < n; i++) seen |= (int)got[i].data.u64;
    }
    char buf[8] = {0};
    ssize_t rd = recv(acc, buf, sizeof buf - 1, 0);
    eventfd_t v = 0;
    eventfd_read(efd, &v);
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(cli, SOL_SOCKET, SO_ERROR, &err, &el);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICHOST};
    struct addrinfo* res = NULL;
    int gai = getaddrinfo("127.0.0.1", "443", &hints, &res);
    char ip[64] = "";
    if (gai == 0) inet_ntop(res->ai_family, &((struct sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof ip);
    int gport = gai == 0 ? ntohs(((struct sockaddr_in*)res->ai_addr)->sin_port) : 0;
    int fam = gai == 0 ? res->ai_family : -1;
    if (res) freeaddrinfo(res);
    close(cli); close(acc); close(srv); close(ep); close(efd);
    char out[200];
    snprintf(out, sizeof out, "opt=%d bind=%d connect=%d accept>0=%d peer=%d idle=%d events=%d recv=%s/%zd eventfd=%llu so_error=%d gai=%d %s:%d fam=%d",
             r1, r2, r3, acc > 0, peer.sin_family, none, seen, buf, rd, (unsigned long long)v, err, gai, ip, gport, fam);
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
        {"aaudio", "()Ljava/lang/String;", (void*)aaudio},
        {"opensl", "()Ljava/lang/String;", (void*)opensl},
        {"net", "()Ljava/lang/String;", (void*)net},
    };
    if ((*env)->RegisterNatives(env, c, methods, 11) != 0) return JNI_ERR;
    __android_log_print(ANDROID_LOG_INFO, TAG, "JNI_OnLoad: %d methods, pi=%.3f, %s", 4, 3.14159, "registered");
    return JNI_VERSION_1_6;
}
