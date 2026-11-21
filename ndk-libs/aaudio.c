// andropkg NDK layer — AAudio (libaaudio) output streams on Core Audio's AudioQueue.
//
// A stream is an AudioQueue with a few buffers of `framesPerBurst` frames. With a data callback
// the queue's thread asks the app for each buffer (as AAudio's callback thread does); without
// one, AAudioStream_write fills a ring buffer the queue drains. Input streams are not supported
// yet (AAUDIO_ERROR_UNAVAILABLE). Oboe uses AAudio on API 27+, so this is most NDK games' audio.
#include <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ndk_internal.h"

// ---- AAudio constants (aaudio/AAudio.h) ----------------------------------------------------
enum {
    AAUDIO_OK = 0,
    AAUDIO_ERROR_DISCONNECTED = -899, AAUDIO_ERROR_ILLEGAL_ARGUMENT = -898, AAUDIO_ERROR_INTERNAL = -896,
    AAUDIO_ERROR_INVALID_STATE = -895, AAUDIO_ERROR_INVALID_HANDLE = -892, AAUDIO_ERROR_UNIMPLEMENTED = -890,
    AAUDIO_ERROR_UNAVAILABLE = -889, AAUDIO_ERROR_NO_MEMORY = -887, AAUDIO_ERROR_NULL = -886,
    AAUDIO_ERROR_TIMEOUT = -885, AAUDIO_ERROR_WOULD_BLOCK = -884, AAUDIO_ERROR_INVALID_FORMAT = -883,
    AAUDIO_ERROR_OUT_OF_RANGE = -882, AAUDIO_ERROR_INVALID_RATE = -880,
};
enum { AAUDIO_FORMAT_UNSPECIFIED = 0, AAUDIO_FORMAT_PCM_I16 = 1, AAUDIO_FORMAT_PCM_FLOAT = 2,
       AAUDIO_FORMAT_PCM_I24_PACKED = 3, AAUDIO_FORMAT_PCM_I32 = 4 };
enum { AAUDIO_DIRECTION_OUTPUT = 0, AAUDIO_DIRECTION_INPUT = 1 };
enum {
    AAUDIO_STREAM_STATE_UNINITIALIZED = 0, AAUDIO_STREAM_STATE_UNKNOWN, AAUDIO_STREAM_STATE_OPEN,
    AAUDIO_STREAM_STATE_STARTING, AAUDIO_STREAM_STATE_STARTED, AAUDIO_STREAM_STATE_PAUSING,
    AAUDIO_STREAM_STATE_PAUSED, AAUDIO_STREAM_STATE_FLUSHING, AAUDIO_STREAM_STATE_FLUSHED,
    AAUDIO_STREAM_STATE_STOPPING, AAUDIO_STREAM_STATE_STOPPED, AAUDIO_STREAM_STATE_CLOSING,
    AAUDIO_STREAM_STATE_CLOSED, AAUDIO_STREAM_STATE_DISCONNECTED,
};
enum { AAUDIO_CALLBACK_RESULT_CONTINUE = 0, AAUDIO_CALLBACK_RESULT_STOP = 1 };
enum { AAUDIO_PERFORMANCE_MODE_NONE = 10, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12 };
enum { AAUDIO_SHARING_MODE_SHARED = 1 };
#define AAUDIO_UNSPECIFIED 0
#define DEFAULT_RATE 48000
#define BURST_FRAMES 256
#define NBUFFERS 3

typedef struct Stream Stream;
typedef int32_t (*DataCallback)(Stream *, void *user, void *audio, int32_t frames);
typedef void (*ErrorCallback)(Stream *, void *user, int32_t error);

typedef struct {
    int32_t direction, rate, channels, format, perf, sharing, capacity, cbFrames;
    int32_t usage, contentType, inputPreset, sessionId, deviceId, channelMask, capturePolicy, spatial;
    bool privacy, spatialized;
    DataCallback dataCb;
    void *dataUser;
    ErrorCallback errorCb;
    void *errorUser;
} Builder;

struct Stream {
    Builder cfg;
    AudioQueueRef queue;
    AudioQueueBufferRef bufs[NBUFFERS];
    int32_t burst, frameBytes, bufferSize;
    _Atomic int32_t state;
    _Atomic int64_t written, read;  // frames: given by the app / consumed by the queue
    _Atomic int32_t xruns;
    pthread_mutex_t lock;  // the ring (write mode)
    pthread_cond_t cond;
    uint8_t *ring;
    size_t ringSize, ringHead, ringFill;  // bytes
};

static int32_t bytes_per_sample(int32_t fmt) {
    switch (fmt) {
    case AAUDIO_FORMAT_PCM_I16: return 2;
    case AAUDIO_FORMAT_PCM_I24_PACKED: return 3;
    default: return 4;
    }
}

// ---- builder -------------------------------------------------------------------------------
NDK_EXPORT int32_t bionic_AAudio_createStreamBuilder(Builder **out) {
    if (!out) return AAUDIO_ERROR_NULL;
    Builder *b = calloc(1, sizeof *b);
    if (!b) return AAUDIO_ERROR_NO_MEMORY;
    b->perf = AAUDIO_PERFORMANCE_MODE_NONE;
    b->sharing = AAUDIO_SHARING_MODE_SHARED;
    b->usage = 1;        // MEDIA
    b->contentType = 2;  // MUSIC
    b->inputPreset = 1;  // GENERIC
    b->sessionId = -1;   // NONE
    b->capturePolicy = 1;
    b->spatial = 1;
    *out = b;
    return AAUDIO_OK;
}
NDK_EXPORT int32_t bionic_AAudioStreamBuilder_delete(Builder *b) { free(b); return AAUDIO_OK; }

#define SETTER(name, field, type) \
    NDK_EXPORT void bionic_AAudioStreamBuilder_##name(Builder *b, type v) { if (b) b->field = v; }
SETTER(setDirection, direction, int32_t)
SETTER(setSampleRate, rate, int32_t)
SETTER(setChannelCount, channels, int32_t)
SETTER(setSamplesPerFrame, channels, int32_t)
SETTER(setFormat, format, int32_t)
SETTER(setPerformanceMode, perf, int32_t)
SETTER(setSharingMode, sharing, int32_t)
SETTER(setBufferCapacityInFrames, capacity, int32_t)
SETTER(setFramesPerDataCallback, cbFrames, int32_t)
SETTER(setUsage, usage, int32_t)
SETTER(setContentType, contentType, int32_t)
SETTER(setInputPreset, inputPreset, int32_t)
SETTER(setSessionId, sessionId, int32_t)
SETTER(setDeviceId, deviceId, int32_t)
SETTER(setChannelMask, channelMask, uint32_t)
SETTER(setAllowedCapturePolicy, capturePolicy, int32_t)
SETTER(setSpatializationBehavior, spatial, int32_t)
// bool arguments: an AAPCS64 caller leaves the bits above the low byte unspecified.
NDK_EXPORT void bionic_AAudioStreamBuilder_setPrivacySensitive(Builder *b, uint32_t v) { if (b) b->privacy = (v & 0xff) != 0; }
NDK_EXPORT void bionic_AAudioStreamBuilder_setIsContentSpatialized(Builder *b, uint32_t v) {
    if (b) b->spatialized = (v & 0xff) != 0;
}
NDK_EXPORT void bionic_AAudioStreamBuilder_setPackageName(Builder *b, const char *s) {}
NDK_EXPORT void bionic_AAudioStreamBuilder_setAttributionTag(Builder *b, const char *s) {}
NDK_EXPORT void bionic_AAudioStreamBuilder_setDataCallback(Builder *b, DataCallback cb, void *user) {
    if (b) b->dataCb = cb, b->dataUser = user;
}
NDK_EXPORT void bionic_AAudioStreamBuilder_setErrorCallback(Builder *b, ErrorCallback cb, void *user) {
    if (b) b->errorCb = cb, b->errorUser = user;
}

// Channel masks (AAUDIO_CHANNEL_*): the channel count is the number of bits.
static int32_t mask_channels(uint32_t mask) { return __builtin_popcount(mask & 0x00ffffff); }

// ---- the queue -----------------------------------------------------------------------------
static void stop_async(Stream *s) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        if (atomic_load(&s->state) == AAUDIO_STREAM_STATE_STARTED ||
            atomic_load(&s->state) == AAUDIO_STREAM_STATE_STOPPING) {
            AudioQueueStop(s->queue, true);
            atomic_store(&s->state, AAUDIO_STREAM_STATE_STOPPED);
        }
    });
}

static void fill(Stream *s, AudioQueueBufferRef buf) {
    int32_t frames = s->burst;
    size_t bytes = (size_t)frames * (size_t)s->frameBytes;
    if (s->cfg.dataCb && atomic_load(&s->state) == AAUDIO_STREAM_STATE_STARTED) {
        int32_t r = s->cfg.dataCb(s, s->cfg.dataUser, buf->mAudioData, frames);
        atomic_fetch_add(&s->written, frames);
        if (r == AAUDIO_CALLBACK_RESULT_STOP) {
            atomic_store(&s->state, AAUDIO_STREAM_STATE_STOPPING);
            stop_async(s);
        }
    } else if (!s->cfg.dataCb) {
        pthread_mutex_lock(&s->lock);
        size_t n = s->ringFill < bytes ? s->ringFill : bytes;
        for (size_t done = 0; done < n;) {  // the ring wraps
            size_t chunk = s->ringSize - s->ringHead;
            if (chunk > n - done) chunk = n - done;
            memcpy((uint8_t *)buf->mAudioData + done, s->ring + s->ringHead, chunk);
            s->ringHead = (s->ringHead + chunk) % s->ringSize;
            done += chunk;
        }
        s->ringFill -= n;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        if (n < bytes) {
            memset((uint8_t *)buf->mAudioData + n, 0, bytes - n);
            if (atomic_load(&s->state) == AAUDIO_STREAM_STATE_STARTED && atomic_load(&s->written) > 0)
                atomic_fetch_add(&s->xruns, 1);
        }
    } else {
        memset(buf->mAudioData, 0, bytes);
    }
    buf->mAudioDataByteSize = (UInt32)bytes;
    atomic_fetch_add(&s->read, frames);
}

static void output_cb(void *user, AudioQueueRef q, AudioQueueBufferRef buf) {
    Stream *s = user;
    int32_t st = atomic_load(&s->state);
    if (st == AAUDIO_STREAM_STATE_CLOSING || st == AAUDIO_STREAM_STATE_CLOSED) return;
    fill(s, buf);
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

NDK_EXPORT int32_t bionic_AAudioStreamBuilder_openStream(Builder *b, Stream **out) {
    if (!b || !out) return AAUDIO_ERROR_NULL;
    if (b->direction == AAUDIO_DIRECTION_INPUT) return AAUDIO_ERROR_UNAVAILABLE;  // no capture yet
    Stream *s = calloc(1, sizeof *s);
    if (!s) return AAUDIO_ERROR_NO_MEMORY;
    s->cfg = *b;
    if (s->cfg.rate == AAUDIO_UNSPECIFIED) s->cfg.rate = DEFAULT_RATE;
    if (s->cfg.channels == AAUDIO_UNSPECIFIED)
        s->cfg.channels = s->cfg.channelMask ? mask_channels((uint32_t)s->cfg.channelMask) : 2;
    if (s->cfg.format == AAUDIO_FORMAT_UNSPECIFIED) s->cfg.format = AAUDIO_FORMAT_PCM_FLOAT;
    if (s->cfg.format == AAUDIO_FORMAT_PCM_I24_PACKED) {
        free(s);
        return AAUDIO_ERROR_INVALID_FORMAT;
    }
    if (s->cfg.channels < 1 || s->cfg.channels > 8) {
        free(s);
        return AAUDIO_ERROR_OUT_OF_RANGE;
    }
    s->burst = s->cfg.cbFrames > 0 ? s->cfg.cbFrames : BURST_FRAMES;
    s->frameBytes = s->cfg.channels * bytes_per_sample(s->cfg.format);
    s->bufferSize = s->burst * 2;
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = s->cfg.rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = s->cfg.format == AAUDIO_FORMAT_PCM_FLOAT ? kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                                                               : kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = (UInt32)bytes_per_sample(s->cfg.format) * 8;
    f.mChannelsPerFrame = (UInt32)s->cfg.channels;
    f.mBytesPerFrame = (UInt32)s->frameBytes;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = (UInt32)s->frameBytes;
    OSStatus e = AudioQueueNewOutput(&f, output_cb, s, NULL, NULL, 0, &s->queue);
    if (e != noErr) {
        ndk_log(NDK_LOG_WARN, "AAudio: AudioQueueNewOutput: %d", (int)e);
        free(s);
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    for (int i = 0; i < NBUFFERS; i++)
        AudioQueueAllocateBuffer(s->queue, (UInt32)(s->burst * s->frameBytes), &s->bufs[i]);
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    if (!s->cfg.dataCb) {  // write mode: a ring of the buffer capacity
        int32_t cap = s->cfg.capacity > s->burst * 4 ? s->cfg.capacity : s->burst * 8;
        s->cfg.capacity = cap;
        s->ringSize = (size_t)cap * (size_t)s->frameBytes;
        s->ring = malloc(s->ringSize);
    } else if (s->cfg.capacity < s->burst * NBUFFERS) {
        s->cfg.capacity = s->burst * NBUFFERS;
    }
    atomic_store(&s->state, AAUDIO_STREAM_STATE_OPEN);
    ndk_log(NDK_LOG_INFO, "AAudio: stream %d Hz x%d fmt=%d %s, burst %d", s->cfg.rate, s->cfg.channels,
            s->cfg.format, s->cfg.dataCb ? "callback" : "write", s->burst);
    *out = s;
    return AAUDIO_OK;
}

// ---- control -------------------------------------------------------------------------------
NDK_EXPORT int32_t bionic_AAudioStream_requestStart(Stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    int32_t st = atomic_load(&s->state);
    if (st == AAUDIO_STREAM_STATE_STARTED) return AAUDIO_OK;
    if (st == AAUDIO_STREAM_STATE_CLOSED || st == AAUDIO_STREAM_STATE_CLOSING) return AAUDIO_ERROR_INVALID_STATE;
    atomic_store(&s->state, AAUDIO_STREAM_STATE_STARTED);
    if (st != AAUDIO_STREAM_STATE_PAUSED) {  // (prime the queue; a paused one still holds its buffers)
        for (int i = 0; i < NBUFFERS; i++) {
            fill(s, s->bufs[i]);
            AudioQueueEnqueueBuffer(s->queue, s->bufs[i], 0, NULL);
        }
    }
    OSStatus e = AudioQueueStart(s->queue, NULL);
    if (e != noErr) {
        ndk_log(NDK_LOG_WARN, "AAudio: AudioQueueStart: %d", (int)e);
        atomic_store(&s->state, st);
        return AAUDIO_ERROR_INTERNAL;
    }
    return AAUDIO_OK;
}
NDK_EXPORT int32_t bionic_AAudioStream_requestPause(Stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    if (s->cfg.direction == AAUDIO_DIRECTION_INPUT) return AAUDIO_ERROR_UNIMPLEMENTED;
    AudioQueuePause(s->queue);
    atomic_store(&s->state, AAUDIO_STREAM_STATE_PAUSED);
    return AAUDIO_OK;
}
NDK_EXPORT int32_t bionic_AAudioStream_requestFlush(Stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    int32_t st = atomic_load(&s->state);
    if (st != AAUDIO_STREAM_STATE_PAUSED && st != AAUDIO_STREAM_STATE_OPEN && st != AAUDIO_STREAM_STATE_STOPPED)
        return AAUDIO_ERROR_INVALID_STATE;
    AudioQueueReset(s->queue);
    pthread_mutex_lock(&s->lock);
    s->ringFill = s->ringHead = 0;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    atomic_store(&s->state, AAUDIO_STREAM_STATE_FLUSHED);
    return AAUDIO_OK;
}
NDK_EXPORT int32_t bionic_AAudioStream_requestStop(Stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    atomic_store(&s->state, AAUDIO_STREAM_STATE_STOPPING);
    AudioQueueStop(s->queue, true);
    pthread_mutex_lock(&s->lock);
    s->ringFill = s->ringHead = 0;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    atomic_store(&s->state, AAUDIO_STREAM_STATE_STOPPED);
    return AAUDIO_OK;
}
NDK_EXPORT int32_t bionic_AAudioStream_release(Stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    atomic_store(&s->state, AAUDIO_STREAM_STATE_CLOSING);
    return AAUDIO_OK;
}
NDK_EXPORT int32_t bionic_AAudioStream_close(Stream *s) {
    if (!s) return AAUDIO_ERROR_NULL;
    atomic_store(&s->state, AAUDIO_STREAM_STATE_CLOSING);
    AudioQueueStop(s->queue, true);
    AudioQueueDispose(s->queue, true);
    pthread_mutex_lock(&s->lock);
    atomic_store(&s->state, AAUDIO_STREAM_STATE_CLOSED);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    free(s->ring);
    free(s);
    return AAUDIO_OK;
}

static int64_t now_ns(clockid_t c) {
    struct timespec ts;
    clock_gettime(c, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

NDK_EXPORT int32_t bionic_AAudioStream_waitForStateChange(Stream *s, int32_t input, int32_t *next, int64_t timeout) {
    if (!s) return AAUDIO_ERROR_NULL;
    int64_t end = now_ns(CLOCK_MONOTONIC) + timeout;
    int32_t st;
    while ((st = atomic_load(&s->state)) == input) {
        if (now_ns(CLOCK_MONOTONIC) >= end) {
            if (next) *next = st;
            return AAUDIO_ERROR_TIMEOUT;
        }
        struct timespec d = {0, 1000000};
        nanosleep(&d, NULL);
    }
    if (next) *next = st;
    return AAUDIO_OK;
}

// ---- data ----------------------------------------------------------------------------------
NDK_EXPORT int32_t bionic_AAudioStream_write(Stream *s, const void *data, int32_t frames, int64_t timeoutNanos) {
    if (!s || !data) return AAUDIO_ERROR_NULL;
    if (s->cfg.dataCb) return AAUDIO_ERROR_INVALID_STATE;
    if (frames <= 0) return frames == 0 ? 0 : AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    int64_t end = now_ns(CLOCK_REALTIME) + timeoutNanos;
    size_t total = (size_t)frames * (size_t)s->frameBytes, done = 0;
    pthread_mutex_lock(&s->lock);
    while (done < total && atomic_load(&s->state) != AAUDIO_STREAM_STATE_CLOSED) {
        size_t space = s->ringSize - s->ringFill;
        if (space < (size_t)s->frameBytes) {  // full: wait for the queue to drain some
            if (timeoutNanos <= 0) break;
            struct timespec until = {(time_t)(end / 1000000000LL), (long)(end % 1000000000LL)};
            if (pthread_cond_timedwait(&s->cond, &s->lock, &until) != 0) break;
            continue;
        }
        size_t n = total - done < space ? total - done : space;
        n -= n % (size_t)s->frameBytes;
        size_t tail = (s->ringHead + s->ringFill) % s->ringSize;
        size_t chunk = s->ringSize - tail < n ? s->ringSize - tail : n;
        memcpy(s->ring + tail, (const uint8_t *)data + done, chunk);
        memcpy(s->ring, (const uint8_t *)data + done + chunk, n - chunk);
        s->ringFill += n;
        done += n;
    }
    pthread_mutex_unlock(&s->lock);
    int32_t wrote = (int32_t)(done / (size_t)s->frameBytes);
    atomic_fetch_add(&s->written, wrote);
    return wrote;
}
NDK_EXPORT int32_t bionic_AAudioStream_read(Stream *s, void *data, int32_t frames, int64_t timeout) {
    return AAUDIO_ERROR_UNIMPLEMENTED;  // (no input streams)
}

NDK_EXPORT int32_t bionic_AAudioStream_getTimestamp(Stream *s, int32_t clock, int64_t *frame, int64_t *time) {
    if (!s) return AAUDIO_ERROR_NULL;
    if (atomic_load(&s->state) != AAUDIO_STREAM_STATE_STARTED) return AAUDIO_ERROR_INVALID_STATE;
    // The frames handed to the queue, minus what it still holds (about one buffer in flight).
    int64_t played = atomic_load(&s->read) - (int64_t)s->burst * NBUFFERS;
    if (frame) *frame = played > 0 ? played : 0;
    // Linux clock ids: 0 REALTIME, 1 MONOTONIC, 4 MONOTONIC_RAW, 7 BOOTTIME.
    if (time) *time = now_ns(clock == 0 ? CLOCK_REALTIME : clock == 4 ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC);
    return AAUDIO_OK;
}

// ---- getters -------------------------------------------------------------------------------
#define GETTER(name, expr) \
    NDK_EXPORT int32_t bionic_AAudioStream_##name(Stream *s) { return s ? (int32_t)(expr) : AAUDIO_ERROR_NULL; }
GETTER(getSampleRate, s->cfg.rate)
GETTER(getHardwareSampleRate, s->cfg.rate)
GETTER(getChannelCount, s->cfg.channels)
GETTER(getHardwareChannelCount, s->cfg.channels)
GETTER(getSamplesPerFrame, s->cfg.channels)
GETTER(getFormat, s->cfg.format)
GETTER(getHardwareFormat, s->cfg.format)
GETTER(getState, atomic_load(&s->state))
GETTER(getDirection, s->cfg.direction)
GETTER(getPerformanceMode, s->cfg.perf)
GETTER(getSharingMode, s->cfg.sharing)
GETTER(getFramesPerBurst, s->burst)
GETTER(getFramesPerDataCallback, s->cfg.dataCb ? s->burst : AAUDIO_UNSPECIFIED)
GETTER(getBufferSizeInFrames, s->bufferSize)
GETTER(getBufferCapacityInFrames, s->cfg.capacity)
GETTER(getXRunCount, atomic_load(&s->xruns))
GETTER(getDeviceId, 1)
GETTER(getUsage, s->cfg.usage)
GETTER(getContentType, s->cfg.contentType)
GETTER(getInputPreset, s->cfg.inputPreset)
GETTER(getSessionId, s->cfg.sessionId)
GETTER(getAllowedCapturePolicy, s->cfg.capturePolicy)
GETTER(getSpatializationBehavior, s->cfg.spatial)
NDK_EXPORT uint32_t bionic_AAudioStream_getChannelMask(Stream *s) {
    return s ? (s->cfg.channels == 1 ? 0x1u : 0x3u) : 0;  // MONO / STEREO
}
NDK_EXPORT int64_t bionic_AAudioStream_getFramesWritten(Stream *s) { return s ? atomic_load(&s->written) : 0; }
NDK_EXPORT int64_t bionic_AAudioStream_getFramesRead(Stream *s) { return s ? atomic_load(&s->read) : 0; }
NDK_EXPORT bool bionic_AAudioStream_isMMapUsed(Stream *s) { return false; }
NDK_EXPORT bool bionic_AAudioStream_isPrivacySensitive(Stream *s) { return s && s->cfg.privacy; }
NDK_EXPORT bool bionic_AAudioStream_isContentSpatialized(Stream *s) { return s && s->cfg.spatialized; }
NDK_EXPORT int32_t bionic_AAudioStream_setBufferSizeInFrames(Stream *s, int32_t frames) {
    if (!s) return AAUDIO_ERROR_NULL;
    if (frames <= 0 || frames > s->cfg.capacity) frames = s->cfg.capacity;
    s->bufferSize = frames;
    return frames;
}
NDK_EXPORT int32_t bionic_AAudio_getMMapPolicy(void) { return 1; }  // NEVER
NDK_EXPORT int32_t bionic_AAudio_setMMapPolicy(int32_t p) { return AAUDIO_OK; }

NDK_EXPORT const char *bionic_AAudio_convertResultToText(int32_t r) {
    switch (r) {
    case AAUDIO_OK: return "AAUDIO_OK";
    case AAUDIO_ERROR_DISCONNECTED: return "AAUDIO_ERROR_DISCONNECTED";
    case AAUDIO_ERROR_ILLEGAL_ARGUMENT: return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case AAUDIO_ERROR_INTERNAL: return "AAUDIO_ERROR_INTERNAL";
    case AAUDIO_ERROR_INVALID_STATE: return "AAUDIO_ERROR_INVALID_STATE";
    case AAUDIO_ERROR_INVALID_HANDLE: return "AAUDIO_ERROR_INVALID_HANDLE";
    case AAUDIO_ERROR_UNIMPLEMENTED: return "AAUDIO_ERROR_UNIMPLEMENTED";
    case AAUDIO_ERROR_UNAVAILABLE: return "AAUDIO_ERROR_UNAVAILABLE";
    case AAUDIO_ERROR_NO_MEMORY: return "AAUDIO_ERROR_NO_MEMORY";
    case AAUDIO_ERROR_NULL: return "AAUDIO_ERROR_NULL";
    case AAUDIO_ERROR_TIMEOUT: return "AAUDIO_ERROR_TIMEOUT";
    case AAUDIO_ERROR_WOULD_BLOCK: return "AAUDIO_ERROR_WOULD_BLOCK";
    case AAUDIO_ERROR_INVALID_FORMAT: return "AAUDIO_ERROR_INVALID_FORMAT";
    case AAUDIO_ERROR_OUT_OF_RANGE: return "AAUDIO_ERROR_OUT_OF_RANGE";
    case AAUDIO_ERROR_INVALID_RATE: return "AAUDIO_ERROR_INVALID_RATE";
    }
    return "Unrecognized AAudio error";
}
NDK_EXPORT const char *bionic_AAudio_convertStreamStateToText(int32_t st) {
    static const char *const names[] = {"UNINITIALIZED", "UNKNOWN", "OPEN", "STARTING", "STARTED", "PAUSING",
                                        "PAUSED", "FLUSHING", "FLUSHED", "STOPPING", "STOPPED", "CLOSING",
                                        "CLOSED", "DISCONNECTED"};
    return st >= 0 && st < (int32_t)(sizeof names / sizeof *names) ? names[st] : "Unrecognized AAudio state";
}
