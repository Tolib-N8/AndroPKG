// andropkg NDK layer — OpenSL ES (libOpenSLES): engine, output mix, PCM audio players fed by
// a (Android simple) buffer queue, on Core Audio's AudioQueue.
//
// Objects and interfaces are OpenSL's: a handle points at a vtable pointer inside our object.
// The app's buffers are copied into the queue's own as they are consumed; the buffer queue
// callback (the app enqueuing its next buffer) runs on the AudioQueue thread, as on Android it
// runs on AudioTrack's. URI/FD players, recorders, MIDI, 3D and effects are not supported
// (SL_RESULT_FEATURE_UNSUPPORTED).
//
// ABI note: Android (AAPCS64) callers don't extend 8/16-bit arguments; Darwin callees assume it.
// Parameters narrower than 32 bits (SLmillibel, SLpermille) are taken as 32-bit and truncated.
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "ndk_internal.h"

typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef uint32_t SLboolean;
typedef struct { uint32_t time_low; uint16_t time_mid, time_hi_and_version, clock_seq; uint8_t node[6]; } IID;
typedef const IID *SLInterfaceID;

enum {
    SL_RESULT_SUCCESS = 0, SL_RESULT_PRECONDITIONS_VIOLATED = 1, SL_RESULT_PARAMETER_INVALID = 2,
    SL_RESULT_MEMORY_FAILURE = 3, SL_RESULT_RESOURCE_ERROR = 4, SL_RESULT_BUFFER_INSUFFICIENT = 7,
    SL_RESULT_CONTENT_UNSUPPORTED = 9, SL_RESULT_FEATURE_UNSUPPORTED = 12, SL_RESULT_INTERNAL_ERROR = 13,
};
enum { SL_OBJECT_STATE_UNREALIZED = 1, SL_OBJECT_STATE_REALIZED = 2 };
enum { SL_PLAYSTATE_STOPPED = 1, SL_PLAYSTATE_PAUSED = 2, SL_PLAYSTATE_PLAYING = 3 };
enum { SL_DATALOCATOR_BUFFERQUEUE = 6, SL_DATALOCATOR_OUTPUTMIX = 4, SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE = 0x800007BD };
enum { SL_DATAFORMAT_PCM = 2, SL_ANDROID_DATAFORMAT_PCM_EX = 4 };
enum { SL_ANDROID_PCM_REPRESENTATION_FLOAT = 3, SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT = 2 };
enum { SL_PLAYEVENT_HEADATEND = 1 };
#define SL_MILLIBEL_MIN ((int16_t)-32768)

// ---- interface IDs -------------------------------------------------------------------------
// Apps only pass the library's own IDs back to it; each needs a distinct identity.
#define IIDS(X)                                                                                         \
    X(3DCOMMIT) X(3DDOPPLER) X(3DGROUPING) X(3DLOCATION) X(3DMACROSCOPIC) X(3DSOURCE)                   \
    X(ANDROIDACOUSTICECHOCANCELLATION) X(ANDROIDAUTOMATICGAINCONTROL) X(ANDROIDBUFFERQUEUESOURCE)       \
    X(ANDROIDCONFIGURATION) X(ANDROIDEFFECT) X(ANDROIDEFFECTCAPABILITIES) X(ANDROIDEFFECTSEND)          \
    X(ANDROIDNOISESUPPRESSION) X(ANDROIDSIMPLEBUFFERQUEUE) X(AUDIODECODERCAPABILITIES) X(AUDIOENCODER)  \
    X(AUDIOENCODERCAPABILITIES) X(AUDIOIODEVICECAPABILITIES) X(BASSBOOST) X(BUFFERQUEUE)                \
    X(DEVICEVOLUME) X(DYNAMICINTERFACEMANAGEMENT) X(DYNAMICSOURCE) X(EFFECTSEND) X(ENGINE)              \
    X(ENGINECAPABILITIES) X(ENVIRONMENTALREVERB) X(EQUALIZER) X(LED) X(METADATAEXTRACTION)              \
    X(METADATATRAVERSAL) X(MIDIMESSAGE) X(MIDIMUTESOLO) X(MIDITEMPO) X(MIDITIME) X(MUTESOLO) X(NULL)    \
    X(OBJECT) X(OUTPUTMIX) X(PITCH) X(PLAY) X(PLAYBACKRATE) X(PREFETCHSTATUS) X(PRESETREVERB)          \
    X(RATEPITCH) X(RECORD) X(SEEK) X(THREADSYNC) X(VIBRA) X(VIRTUALIZER) X(VISUALIZATION) X(VOLUME)     \
    X(ANDROIDBUFFERQUEUE)
enum {
#define X(n) IDX_##n,
    IIDS(X)
#undef X
    IID_COUNT
};
static const IID kIids[IID_COUNT] = {
#define X(n) [IDX_##n] = {0xa9d50000u + IDX_##n, 0x5a1d, 0x11e4, 0x8f0e, {'a', 'n', 'd', 'r', 'o', 'p'}},
    IIDS(X)
#undef X
};
#define X(n) NDK_EXPORT const SLInterfaceID bionic_SL_IID_##n = &kIids[IDX_##n];
IIDS(X)
#undef X

static bool iid_is(SLInterfaceID id, int idx) {
    return id && (id == &kIids[idx] || memcmp(id, &kIids[idx], sizeof(IID)) == 0);
}

// ---- objects -------------------------------------------------------------------------------
typedef struct Object Object;
typedef void (*SlotCallback)(void *caller, void *context);

typedef struct {  // a PCM buffer queue player
    AudioQueueRef queue;
    AudioQueueBufferRef bufs[3];
    uint32_t bufBytes, frameBytes, maxBuffers;
    pthread_mutex_t lock;
    struct { const void *data; uint32_t size; } fifo[64];
    uint32_t head, count, offset;  // the app's buffers; bytes of fifo[head] consumed
    uint32_t played;               // buffers consumed so far (the queue state's index)
    _Atomic uint32_t playState;
    SlotCallback bqCallback;
    void *bqContext;
    void (*playCallback)(void *caller, void *context, uint32_t event);
    void *playContext;
    uint32_t eventMask;
    int16_t volume;
    bool muted;
    uint64_t framesPlayed;
    uint32_t rate;
} Player;

enum { KIND_ENGINE, KIND_OUTPUTMIX, KIND_PLAYER };

struct Object {
    const void *objectItf;  // each interface handle is the address of its vtable pointer
    const void *engineItf, *playItf, *bqItf, *volumeItf, *outputMixItf, *effectSendItf, *configItf;
    int kind;
    uint32_t state;
    Player player;
};

#define FROM(field, self) ((Object *)((char *)(self) - offsetof(Object, field)))

// ---- SLObjectItf -------------------------------------------------------------------------------
static SLresult obj_Realize(const void *self, SLboolean async);
static SLresult obj_Resume(const void *self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(const void *self, SLuint32 *state) {
    if (!state) return SL_RESULT_PARAMETER_INVALID;
    *state = FROM(objectItf, self)->state;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_GetInterface(const void *self, SLInterfaceID iid, void *out);
static SLresult obj_RegisterCallback(const void *self, void *cb, void *ctx) { return SL_RESULT_SUCCESS; }
static void obj_AbortAsyncOperation(const void *self) {}
static void obj_Destroy(const void *self);
static SLresult obj_SetPriority(const void *self, int32_t p, SLboolean pre) { return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(const void *self, int32_t *p, SLboolean *pre) {
    if (p) *p = 0;
    if (pre) *pre = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_SetLossOfControlInterfaces(const void *self, int16_t n, SLInterfaceID *ids, SLboolean e) {
    return SL_RESULT_SUCCESS;
}
static const struct {
    void *f[10];
} kObjectVt = {{obj_Realize, obj_Resume, obj_GetState, obj_GetInterface, obj_RegisterCallback, obj_AbortAsyncOperation,
                obj_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLossOfControlInterfaces}};

static Object *new_object(int kind) {
    Object *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->objectItf = &kObjectVt;
    o->kind = kind;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    return o;
}

// The interfaces an object kind has (for GetInterface and "required" checks).
static void *interface_of(Object *o, SLInterfaceID iid) {
    if (iid_is(iid, IDX_OBJECT)) return &o->objectItf;
    switch (o->kind) {
    case KIND_ENGINE:
        if (iid_is(iid, IDX_ENGINE)) return &o->engineItf;
        break;
    case KIND_OUTPUTMIX:
        if (iid_is(iid, IDX_OUTPUTMIX)) return &o->outputMixItf;
        break;
    case KIND_PLAYER:
        if (iid_is(iid, IDX_PLAY)) return &o->playItf;
        if (iid_is(iid, IDX_BUFFERQUEUE) || iid_is(iid, IDX_ANDROIDSIMPLEBUFFERQUEUE)) return &o->bqItf;
        if (iid_is(iid, IDX_VOLUME)) return &o->volumeItf;
        if (iid_is(iid, IDX_EFFECTSEND)) return &o->effectSendItf;
        if (iid_is(iid, IDX_ANDROIDCONFIGURATION)) return &o->configItf;
        break;
    }
    return NULL;
}

static SLresult check_required(Object *o, SLuint32 n, const SLInterfaceID *ids, const SLboolean *req) {
    for (SLuint32 i = 0; i < n; i++)
        if (req && req[i] && !interface_of(o, ids[i])) return SL_RESULT_FEATURE_UNSUPPORTED;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_GetInterface(const void *self, SLInterfaceID iid, void *out) {
    Object *o = FROM(objectItf, self);
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    if (o->state != SL_OBJECT_STATE_REALIZED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    void *itf = interface_of(o, iid);
    *(void **)out = itf;
    return itf ? SL_RESULT_SUCCESS : SL_RESULT_FEATURE_UNSUPPORTED;
}

// ---- the player's queue ----------------------------------------------------------------------
static void player_fill(Object *o, AudioQueueBufferRef buf) {
    Player *p = &o->player;
    uint8_t *dst = buf->mAudioData;
    uint32_t want = p->bufBytes, got = 0;
    SlotCallback cbs[64];
    void *self = &o->bqItf;
    int ncb = 0;
    pthread_mutex_lock(&p->lock);
    if (atomic_load(&p->playState) == SL_PLAYSTATE_PLAYING) {
        while (got < want && p->count > 0) {
            uint32_t n = p->fifo[p->head].size - p->offset;
            if (n > want - got) n = want - got;
            memcpy(dst + got, (const uint8_t *)p->fifo[p->head].data + p->offset, n);
            got += n;
            p->offset += n;
            if (p->offset >= p->fifo[p->head].size) {  // the app's buffer is done: it may reuse it
                p->head = (p->head + 1) % 64;
                p->count--;
                p->offset = 0;
                p->played++;
                if (p->bqCallback && ncb < 64) cbs[ncb++] = p->bqCallback;
            }
        }
    }
    p->framesPlayed += got / p->frameBytes;
    void *ctx = p->bqContext;
    pthread_mutex_unlock(&p->lock);
    if (got < want) memset(dst + got, 0, want - got);
    buf->mAudioDataByteSize = want;
    for (int i = 0; i < ncb; i++) cbs[i](self, ctx);  // outside the lock: it enqueues
}

static void player_output(void *user, AudioQueueRef q, AudioQueueBufferRef buf) {
    Object *o = user;
    if (o->state != SL_OBJECT_STATE_REALIZED) return;
    player_fill(o, buf);
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

// ---- SLPlayItf ---------------------------------------------------------------------------------
static SLresult play_SetPlayState(const void *self, SLuint32 state) {
    Object *o = FROM(playItf, self);
    Player *p = &o->player;
    uint32_t old = atomic_exchange(&p->playState, state);
    if (state == old) return SL_RESULT_SUCCESS;
    if (state == SL_PLAYSTATE_PLAYING) {
        if (old == SL_PLAYSTATE_STOPPED) {
            for (int i = 0; i < 3; i++) {
                player_fill(o, p->bufs[i]);
                AudioQueueEnqueueBuffer(p->queue, p->bufs[i], 0, NULL);
            }
        }
        if (AudioQueueStart(p->queue, NULL) != noErr) return SL_RESULT_RESOURCE_ERROR;
    } else if (state == SL_PLAYSTATE_PAUSED) {
        AudioQueuePause(p->queue);
    } else if (state == SL_PLAYSTATE_STOPPED) {
        AudioQueueStop(p->queue, true);
        pthread_mutex_lock(&p->lock);
        p->framesPlayed = 0;
        p->offset = 0;  // (the queued buffers stay: Clear() drops them)
        pthread_mutex_unlock(&p->lock);
    } else {
        atomic_store(&p->playState, old);
        return SL_RESULT_PARAMETER_INVALID;
    }
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(const void *self, SLuint32 *state) {
    if (!state) return SL_RESULT_PARAMETER_INVALID;
    *state = atomic_load(&FROM(playItf, self)->player.playState);
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetDuration(const void *self, SLuint32 *ms) {
    if (ms) *ms = 0xFFFFFFFFu;  // SL_TIME_UNKNOWN
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetPosition(const void *self, SLuint32 *ms) {
    Player *p = &FROM(playItf, self)->player;
    if (ms) *ms = (SLuint32)(p->framesPlayed * 1000 / (p->rate ? p->rate : 48000));
    return SL_RESULT_SUCCESS;
}
static SLresult play_RegisterCallback(const void *self, void *cb, void *ctx) {
    Player *p = &FROM(playItf, self)->player;
    p->playCallback = cb;
    p->playContext = ctx;
    return SL_RESULT_SUCCESS;
}
static SLresult play_SetCallbackEventsMask(const void *self, SLuint32 m) {
    FROM(playItf, self)->player.eventMask = m;
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetCallbackEventsMask(const void *self, SLuint32 *m) {
    if (m) *m = FROM(playItf, self)->player.eventMask;
    return SL_RESULT_SUCCESS;
}
static SLresult play_SetMs(const void *self, SLuint32 ms) { return SL_RESULT_SUCCESS; }
static SLresult play_Clear(const void *self) { return SL_RESULT_SUCCESS; }
static SLresult play_GetMs(const void *self, SLuint32 *ms) {
    if (ms) *ms = 0;
    return SL_RESULT_SUCCESS;
}
static const struct { void *f[12]; } kPlayVt = {{play_SetPlayState, play_GetPlayState, play_GetDuration, play_GetPosition,
                                                 play_RegisterCallback, play_SetCallbackEventsMask,
                                                 play_GetCallbackEventsMask, play_SetMs, play_Clear, play_GetMs,
                                                 play_SetMs, play_GetMs}};

// ---- SLBufferQueueItf / SLAndroidSimpleBufferQueueItf --------------------------------------
static SLresult bq_Enqueue(const void *self, const void *data, SLuint32 size) {
    Player *p = &FROM(bqItf, self)->player;
    if (!data || size == 0) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&p->lock);
    if (p->count >= p->maxBuffers) {
        pthread_mutex_unlock(&p->lock);
        return SL_RESULT_BUFFER_INSUFFICIENT;
    }
    uint32_t tail = (p->head + p->count) % 64;
    p->fifo[tail].data = data;
    p->fifo[tail].size = size;
    p->count++;
    pthread_mutex_unlock(&p->lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(const void *self) {
    Player *p = &FROM(bqItf, self)->player;
    pthread_mutex_lock(&p->lock);
    p->count = p->head = p->offset = 0;
    pthread_mutex_unlock(&p->lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(const void *self, SLuint32 *state /* {count, index} */) {
    Player *p = &FROM(bqItf, self)->player;
    if (!state) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&p->lock);
    state[0] = p->count;
    state[1] = p->played;
    pthread_mutex_unlock(&p->lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(const void *self, SlotCallback cb, void *ctx) {
    Player *p = &FROM(bqItf, self)->player;
    if (atomic_load(&p->playState) != SL_PLAYSTATE_STOPPED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    pthread_mutex_lock(&p->lock);
    p->bqCallback = cb;
    p->bqContext = ctx;
    pthread_mutex_unlock(&p->lock);
    return SL_RESULT_SUCCESS;
}
static const struct { void *f[4]; } kBqVt = {{bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback}};

// ---- SLVolumeItf -------------------------------------------------------------------------------
static void apply_volume(Player *p) {
    float gain = p->muted || p->volume <= -9600 ? 0.f : powf(10.f, p->volume / 2000.f);
    if (p->queue) AudioQueueSetParameter(p->queue, kAudioQueueParam_Volume, gain);
}
static SLresult vol_SetVolumeLevel(const void *self, uint32_t level) {
    Player *p = &FROM(volumeItf, self)->player;
    p->volume = (int16_t)level;
    if (p->volume > 0) p->volume = 0;
    apply_volume(p);
    return SL_RESULT_SUCCESS;
}
static SLresult vol_GetVolumeLevel(const void *self, int16_t *level) {
    if (level) *level = FROM(volumeItf, self)->player.volume;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMaxVolumeLevel(const void *self, int16_t *max) {
    if (max) *max = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_SetMute(const void *self, SLboolean mute) {
    Player *p = &FROM(volumeItf, self)->player;
    p->muted = mute != 0;
    apply_volume(p);
    return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMute(const void *self, SLboolean *mute) {
    if (mute) *mute = FROM(volumeItf, self)->player.muted;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_EnableStereoPosition(const void *self, SLboolean e) { return SL_RESULT_SUCCESS; }
static SLresult vol_IsEnabledStereoPosition(const void *self, SLboolean *e) {
    if (e) *e = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_SetStereoPosition(const void *self, uint32_t permille) { return SL_RESULT_SUCCESS; }
static SLresult vol_GetStereoPosition(const void *self, int16_t *permille) {
    if (permille) *permille = 0;
    return SL_RESULT_SUCCESS;
}
static const struct { void *f[9]; } kVolumeVt = {{vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel,
                                                  vol_SetMute, vol_GetMute, vol_EnableStereoPosition,
                                                  vol_IsEnabledStereoPosition, vol_SetStereoPosition,
                                                  vol_GetStereoPosition}};

// ---- SLOutputMixItf, SLEffectSendItf, SLAndroidConfigurationItf (accepted, no effect) --------
static SLresult ok_any(void) { return SL_RESULT_SUCCESS; }
static SLresult mix_GetDestinationOutputDeviceIDs(const void *self, int32_t *n, SLuint32 *ids) {
    if (n) {
        if (ids && *n > 0) ids[0] = 1;
        *n = 1;
    }
    return SL_RESULT_SUCCESS;
}
static const struct { void *f[3]; } kOutputMixVt = {{mix_GetDestinationOutputDeviceIDs, ok_any, ok_any}};
static SLresult effect_IsEnabled(const void *self, const void *aux, SLboolean *e) {
    if (e) *e = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult effect_GetLevel(const void *self, int16_t *level) {
    if (level) *level = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult effect_GetSendLevel(const void *self, const void *aux, int16_t *level) {
    if (level) *level = 0;
    return SL_RESULT_SUCCESS;
}
static const struct { void *f[6]; } kEffectSendVt = {{ok_any, effect_IsEnabled, ok_any, effect_GetLevel, ok_any,
                                                      effect_GetSendLevel}};
static SLresult config_Get(const void *self, const char *key, SLuint32 *size, void *value) {
    return SL_RESULT_PARAMETER_INVALID;
}
static SLresult config_Acquire(const void *self, SLuint32 type, void *out) { return SL_RESULT_FEATURE_UNSUPPORTED; }
static const struct { void *f[4]; } kConfigVt = {{ok_any, config_Get, config_Acquire, ok_any}};

// ---- realize / destroy -----------------------------------------------------------------------
static SLresult obj_Realize(const void *self, SLboolean async) {
    Object *o = FROM(objectItf, self);
    if (o->state == SL_OBJECT_STATE_REALIZED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    o->state = SL_OBJECT_STATE_REALIZED;
    return SL_RESULT_SUCCESS;
}

static void obj_Destroy(const void *self) {
    Object *o = FROM(objectItf, self);
    if (o->kind == KIND_PLAYER && o->player.queue) {
        o->state = SL_OBJECT_STATE_UNREALIZED;
        AudioQueueStop(o->player.queue, true);
        AudioQueueDispose(o->player.queue, true);
        pthread_mutex_destroy(&o->player.lock);
    }
    free(o);
}

// ---- SLEngineItf -------------------------------------------------------------------------------
typedef struct { SLuint32 locatorType; SLuint32 numBuffers; } LocatorBQ;
typedef struct {
    SLuint32 formatType, numChannels, samplesPerSec, bitsPerSample, containerSize, channelMask, endianness,
        representation;
} FormatPCM;
typedef struct { void *pLocator; void *pFormat; } DataSource;

static SLresult engine_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult engine_CreateOutputMix(const void *self, const void **out, SLuint32 n, const SLInterfaceID *ids,
                                       const SLboolean *req) {
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    Object *o = new_object(KIND_OUTPUTMIX);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->outputMixItf = &kOutputMixVt;
    SLresult r = check_required(o, n, ids, req);
    if (r != SL_RESULT_SUCCESS) {
        free(o);
        return r;
    }
    *out = &o->objectItf;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_CreateAudioPlayer(const void *self, const void **out, const DataSource *src,
                                         const DataSource *sink, SLuint32 n, const SLInterfaceID *ids,
                                         const SLboolean *req) {
    if (!out || !src || !src->pLocator || !src->pFormat) return SL_RESULT_PARAMETER_INVALID;
    const LocatorBQ *loc = src->pLocator;
    if (loc->locatorType != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE && loc->locatorType != SL_DATALOCATOR_BUFFERQUEUE) {
        ndk_log(NDK_LOG_WARN, "OpenSL ES: audio player source locator 0x%x not supported", loc->locatorType);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    const FormatPCM *fmt = src->pFormat;
    if (fmt->formatType != SL_DATAFORMAT_PCM && fmt->formatType != SL_ANDROID_DATAFORMAT_PCM_EX)
        return SL_RESULT_CONTENT_UNSUPPORTED;
    bool isFloat = fmt->formatType == SL_ANDROID_DATAFORMAT_PCM_EX && fmt->representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT;
    uint32_t bits = fmt->bitsPerSample, channels = fmt->numChannels, rate = fmt->samplesPerSec / 1000;  // milliHz
    if (channels < 1 || channels > 8 || rate == 0 || (bits != 8 && bits != 16 && bits != 32 && !(isFloat && bits == 32)))
        return SL_RESULT_CONTENT_UNSUPPORTED;

    Object *o = new_object(KIND_PLAYER);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->playItf = &kPlayVt;
    o->bqItf = &kBqVt;
    o->volumeItf = &kVolumeVt;
    o->effectSendItf = &kEffectSendVt;
    o->configItf = &kConfigVt;
    SLresult r = check_required(o, n, ids, req);
    if (r != SL_RESULT_SUCCESS) {
        free(o);
        return r;
    }
    Player *p = &o->player;
    pthread_mutex_init(&p->lock, NULL);
    atomic_store(&p->playState, SL_PLAYSTATE_STOPPED);
    p->maxBuffers = loc->numBuffers > 0 && loc->numBuffers <= 64 ? loc->numBuffers : 64;
    p->frameBytes = channels * bits / 8;
    p->rate = rate;
    p->bufBytes = 1024 * p->frameBytes;
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsPacked |
                     (isFloat ? kAudioFormatFlagIsFloat : bits == 8 ? 0 : kAudioFormatFlagIsSignedInteger);
    f.mBitsPerChannel = bits;
    f.mChannelsPerFrame = channels;
    f.mBytesPerFrame = p->frameBytes;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = p->frameBytes;
    if (AudioQueueNewOutput(&f, player_output, o, NULL, NULL, 0, &p->queue) != noErr) {
        free(o);
        return SL_RESULT_RESOURCE_ERROR;
    }
    for (int i = 0; i < 3; i++) AudioQueueAllocateBuffer(p->queue, p->bufBytes, &p->bufs[i]);
    ndk_log(NDK_LOG_INFO, "OpenSL ES: player %u Hz x%u, %u-bit%s, %u buffers", rate, channels, bits,
            isFloat ? " float" : "", p->maxBuffers);
    *out = &o->objectItf;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_QueryNumSupportedInterfaces(const void *self, SLuint32 objectId, SLuint32 *n) {
    if (n) *n = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult engine_QueryNumSupportedExtensions(const void *self, SLuint32 *n) {
    if (n) *n = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult engine_IsExtensionSupported(const void *self, const char *name, SLboolean *supported) {
    if (supported) *supported = 0;
    return SL_RESULT_SUCCESS;
}
static const struct { void *f[15]; } kEngineVt = {{
    engine_unsupported,         // CreateLEDDevice
    engine_unsupported,         // CreateVibraDevice
    engine_CreateAudioPlayer,   //
    engine_unsupported,         // CreateAudioRecorder
    engine_unsupported,         // CreateMidiPlayer
    engine_unsupported,         // CreateListener
    engine_unsupported,         // Create3DGroup
    engine_CreateOutputMix,     //
    engine_unsupported,         // CreateMetadataExtractor
    engine_unsupported,         // CreateExtensionObject
    engine_QueryNumSupportedInterfaces,
    engine_unsupported,         // QuerySupportedInterfaces
    engine_QueryNumSupportedExtensions,
    engine_unsupported,         // QuerySupportedExtension
    engine_IsExtensionSupported,
}};

NDK_EXPORT SLresult bionic_slCreateEngine(const void **out, SLuint32 numOptions, const void *options, SLuint32 n,
                                          const SLInterfaceID *ids, const SLboolean *req) {
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    Object *o = new_object(KIND_ENGINE);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->engineItf = &kEngineVt;
    SLresult r = check_required(o, n, ids, req);
    if (r != SL_RESULT_SUCCESS) {
        free(o);
        return r;
    }
    *out = &o->objectItf;
    return SL_RESULT_SUCCESS;
}
NDK_EXPORT SLresult bionic_slQueryNumSupportedEngineInterfaces(SLuint32 *n) {
    if (!n) return SL_RESULT_PARAMETER_INVALID;
    *n = 2;
    return SL_RESULT_SUCCESS;
}
NDK_EXPORT SLresult bionic_slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID *iid) {
    if (!iid || index > 1) return SL_RESULT_PARAMETER_INVALID;
    *iid = &kIids[index == 0 ? IDX_OBJECT : IDX_ENGINE];
    return SL_RESULT_SUCCESS;
}
