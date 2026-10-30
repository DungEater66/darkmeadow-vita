/*
 * A small OpenSL ES implementation: exactly what Unreal Engine 3's UOpenSLAudioDevice uses, and nothing more.
 *
 * Why not VitaSDK's libOpenSLES (Android's "wilhelm")? On the console its mixer thread did not start, and every
 * stop/destroy of a player waits for that mixer to acknowledge, so the game thread hung (first after "tap to start",
 * then in CreateAudioPlayer while the loader was standing in for the mixer). Here nothing ever waits for the mixer:
 * state changes take effect immediately and buffer-queue callbacks run with no lock held.
 *
 * What the engine does (from the disassembly of FOpenSLSoundSource / UOpenSLAudioDevice in 1.4.6.4):
 *   slCreateEngine -> Realize -> GetInterface(SL_IID_ENGINE) -> CreateOutputMix -> Realize
 *   per sound:  CreateAudioPlayer(src = buffer queue + PCM 16-bit mono/stereo, sink = output mix,
 *                                 interfaces = BUFFERQUEUE + VOLUME) -> Realize -> GetInterface(PLAY, VOLUME, BUFFERQUEUE)
 *               Enqueue(whole sound) [looping sounds: RegisterCallback, the callback enqueues the same buffer again]
 *               SetPlayState(PLAYING / PAUSED / STOPPED), GetPlayState (== STOPPED means finished),
 *               GetMaxVolumeLevel + SetVolumeLevel (millibels), Destroy
 * A one-shot player (no buffer callback) whose queue runs dry switches to SL_PLAYSTATE_STOPPED. The engine polls
 * GetPlayState in FOpenSLSoundSource::IsSourceFinished and frees the source when it reads STOPPED; otherwise each
 * finished sound keeps its source until the engine's own time-out (about 2.5x its length), and quick repeats such as
 * sword swipes run out of sources after 5 or 6 and go silent. Looping sounds re-enqueue from their callback, so their
 * queue does not run dry.
 *
 * Everything - these sound effects and the music/dialogue from song.c - is mixed to 48 kHz stereo on one audio port
 * by one thread, then scaled by DM_AUDIO_MASTER and soft-limited.
 */
#include "audio/opensl.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "audio/song.h"
#include "game_info.h"
#include "util/logger.h"
#include "util/watchdog.h"

/* ---- OpenSL ES ABI (Khronos OpenSL ES 1.0.1 + the Android buffer queue, which has the same layout) ---------------- */

typedef uint32_t SLresult;
typedef void *vfn;   /* vtable entries: the engine only calls through them */

#define SL_RESULT_SUCCESS              0
#define SL_RESULT_PARAMETER_INVALID    2
#define SL_RESULT_MEMORY_FAILURE       3
#define SL_RESULT_BUFFER_INSUFFICIENT  7
#define SL_RESULT_CONTENT_UNSUPPORTED  9
#define SL_RESULT_FEATURE_UNSUPPORTED 12

#define SL_OBJECT_STATE_UNREALIZED 1
#define SL_OBJECT_STATE_REALIZED   2

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_DATALOCATOR_OUTPUTMIX               0x00000004
#define SL_DATALOCATOR_BUFFERQUEUE             0x00000006
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BD
#define SL_DATAFORMAT_PCM 2

typedef struct { uint32_t time_low; uint16_t time_mid, time_hi_and_version, clock_seq; uint8_t node[6]; } SLInterfaceID_;
typedef const SLInterfaceID_ *SLInterfaceID;
typedef struct { uint32_t locatorType; uint32_t numBuffers; } SLDataLocator_BufferQueue;
typedef struct { uint32_t formatType, numChannels, samplesPerSec, bitsPerSample, containerSize, channelMask, endianness; } SLDataFormat_PCM;
typedef struct { void *pLocator; void *pFormat; } SLDataSource;
typedef struct { uint32_t count; uint32_t playIndex; } SLBufferQueueState;

/* Our interface IDs. The engine looks them up by name with dlsym (see dynlib.c) and passes the pointers back. */
static const SLInterfaceID_ iid_engine = { 0x8d97c260, 0xddd4, 0x11db, 0x958f, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ iid_play   = { 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ iid_volume = { 0x09e8ede0, 0xddde, 0x11db, 0xb4f6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ iid_bq     = { 0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ iid_abq    = { 0x198e4940, 0xc5d7, 0x11df, 0xa2a6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };

const void *const dm_SL_IID_ENGINE = &iid_engine;
const void *const dm_SL_IID_PLAY = &iid_play;
const void *const dm_SL_IID_VOLUME = &iid_volume;
const void *const dm_SL_IID_BUFFERQUEUE = &iid_bq;

static int iid_is(SLInterfaceID a, const SLInterfaceID_ *b) {
    return a == b || (a && memcmp(a, b, sizeof(*b)) == 0);
}

/* ---- objects ------------------------------------------------------------------------------------------------- */

struct sl_object;
typedef struct { const vfn *vtbl; struct sl_object *owner; } itf_t;   /* what an SL*Itf points to */

enum { KIND_ENGINE, KIND_OUTPUTMIX, KIND_PLAYER };

#define QUEUE_MAX 16
typedef struct { const int16_t *data; uint32_t frames; } qbuf_t;

typedef struct sl_object {
    itf_t obj;                   /* SLObjectItf */
    itf_t engine, play, bq, vol; /* the other interfaces (only the ones that apply to `kind` are handed out) */
    int kind;
    int state;

    /* players */
    int channels;
    uint32_t rate;
    int play_state;
    qbuf_t queue[QUEUE_MAX];
    int q_head, q_count;
    uint64_t pos;                /* 48.16 fixed point: frames into queue[q_head]. (It was 16.16 in 32 bits, which wrapped
                                  * after 65536 frames - 1.5 s at 44.1 kHz - and replayed longer sounds from the start:
                                  * the "plays three times" and the pulsing buzz of long looping sounds.) */
    float gain;                  /* from the millibel volume level */
    float cur_gain;              /* gain actually applied, ramps towards `gain` */
    int16_t level_mb;
    int mute;
    void (*callback)(void *caller, void *ctx);
    void *callback_ctx;
    unsigned id;                 /* for the trace */
    volatile int in_callback;
    int destroyed;               /* Destroy() was called from inside its own callback: the mixer frees it */
    int drained;                 /* the mixer stopped it because its data ran out (for the trace) */
    int slot;
} sl_object;

#define MAX_PLAYERS 128
static sl_object *players[MAX_PLAYERS];
static SceKernelLwMutexWork mix_lock;
static SceUID mix_thread_id = -1;
static volatile unsigned mixed_blocks;
static int lock_ready;

static void lock(void)   { sceKernelLockLwMutex(&mix_lock, 1, NULL); }
static void unlock(void) { sceKernelUnlockLwMutex(&mix_lock, 1); }

static SLresult unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

/* The first few hundred player operations are logged, with a timestamp and a short player id, to see exactly what the
 * engine asks for (e.g. why a sound would be heard several times). Not from the mixer thread itself (looping sounds
 * re-enqueue from their callback there): a log call can wait on the memory card, and the mixer must never wait. */
#define TRACE_MAX 400
static volatile int trace_n;
static unsigned next_player_id = 1;
#define TRACE_LONG(o, fmt, ...) do { \
    if (sceKernelGetThreadId() != mix_thread_id) \
        l_debug("[opensl] %6u ms  #%u " fmt, (unsigned)(sceKernelGetProcessTimeWide() / 1000), (o)->id, ##__VA_ARGS__); \
} while (0)
#define TRACE(o, fmt, ...) do { \
    if (trace_n < TRACE_MAX && sceKernelGetThreadId() != mix_thread_id && \
        __sync_fetch_and_add(&trace_n, 1) < TRACE_MAX) \
        l_debug("[opensl] %6u ms  #%u " fmt, (unsigned)(sceKernelGetProcessTimeWide() / 1000), (o)->id, ##__VA_ARGS__); \
} while (0)

/* SLObjectItf */
static SLresult obj_Realize(itf_t *self, int async) { self->owner->state = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(itf_t *self, int async)  { return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(itf_t *self, uint32_t *st) { if (!st) return SL_RESULT_PARAMETER_INVALID; *st = self->owner->state; return SL_RESULT_SUCCESS; }
static SLresult obj_GetInterface(itf_t *self, SLInterfaceID iid, void *out);
static SLresult obj_RegisterCallback(itf_t *self, void *cb, void *ctx) { return SL_RESULT_SUCCESS; }
static void obj_AbortAsyncOperation(itf_t *self) {}
static void obj_Destroy(itf_t *self);
static SLresult obj_SetPriority(itf_t *self, int p, int pre) { return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(itf_t *self, int *p, int *pre) { if (p) *p = 0; if (pre) *pre = 0; return SL_RESULT_SUCCESS; }
static const vfn obj_vtbl[] = {
    obj_Realize, obj_Resume, obj_GetState, obj_GetInterface, obj_RegisterCallback, obj_AbortAsyncOperation,
    obj_Destroy, obj_SetPriority, obj_GetPriority, unsupported /* SetLossOfControlInterfaces */
};

static sl_object *object_new(int kind);

/* SLEngineItf */
static SLresult eng_CreateAudioPlayer(itf_t *self, itf_t **out, SLDataSource *src, SLDataSource *snk,
                                      uint32_t n, const SLInterfaceID *ids, const uint32_t *req) {
    if (!out || !src || !src->pLocator || !src->pFormat) return SL_RESULT_PARAMETER_INVALID;
    const SLDataLocator_BufferQueue *loc = src->pLocator;
    const SLDataFormat_PCM *fmt = src->pFormat;
    if ((loc->locatorType != SL_DATALOCATOR_BUFFERQUEUE && loc->locatorType != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE) ||
        fmt->formatType != SL_DATAFORMAT_PCM || fmt->bitsPerSample != 16 || fmt->numChannels < 1 || fmt->numChannels > 2) {
        static int warned;
        if (!warned++) l_warn("[opensl] unsupported audio player (locator 0x%x, format %u, %u ch, %u bit)",
                              (unsigned)loc->locatorType, (unsigned)fmt->formatType, (unsigned)fmt->numChannels,
                              (unsigned)fmt->bitsPerSample);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    for (uint32_t i = 0; i < n; i++) {
        int known = iid_is(ids[i], &iid_play) || iid_is(ids[i], &iid_volume) || iid_is(ids[i], &iid_bq) ||
                    iid_is(ids[i], &iid_abq);
        if (!known && req && req[i]) return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    sl_object *o = object_new(KIND_PLAYER);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->channels = (int)fmt->numChannels;
    o->rate = fmt->samplesPerSec / 1000;   /* OpenSL gives milliHertz */
    if (o->rate < 1000) o->rate = 22050;
    o->play_state = SL_PLAYSTATE_STOPPED;
    o->gain = o->cur_gain = 1.f;
    o->id = next_player_id++;
    TRACE(o, "create: %d ch, %u Hz", o->channels, (unsigned)o->rate);

    lock();
    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) if (!players[i]) { slot = i; break; }
    if (slot >= 0) { players[slot] = o; o->slot = slot; }
    unlock();
    if (slot < 0) {
        static int warned;
        if (!warned++) l_warn("[opensl] more than %d audio players at once; refusing new ones", MAX_PLAYERS);
        free(o);
        return SL_RESULT_MEMORY_FAILURE;
    }
    *out = &o->obj;
    return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(itf_t *self, itf_t **out, uint32_t n, const SLInterfaceID *ids, const uint32_t *req) {
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    sl_object *o = object_new(KIND_OUTPUTMIX);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    *out = &o->obj;
    return SL_RESULT_SUCCESS;
}

static const vfn engine_vtbl[] = {
    unsupported, unsupported, eng_CreateAudioPlayer, unsupported, unsupported, unsupported, unsupported,
    eng_CreateOutputMix, unsupported, unsupported, unsupported, unsupported, unsupported, unsupported, unsupported
};

/* SLPlayItf */
static SLresult play_SetPlayState(itf_t *self, uint32_t st) {
    sl_object *o = self->owner;
    if (st < SL_PLAYSTATE_STOPPED || st > SL_PLAYSTATE_PLAYING) return SL_RESULT_PARAMETER_INVALID;
    static const char *const names[] __attribute__((unused)) = { "?", "STOPPED", "PAUSED", "PLAYING" };
    TRACE(o, "%s (was %s, %d buffers queued)", names[st], names[o->play_state & 3], o->q_count);
    lock();
    o->play_state = (int)st;
    if (st == SL_PLAYSTATE_STOPPED) { o->q_head = o->q_count = 0; o->pos = 0; }   /* stopping clears the queue */
    unlock();
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(itf_t *self, uint32_t *st) {
    if (!st) return SL_RESULT_PARAMETER_INVALID;
    sl_object *o = self->owner;
    *st = (uint32_t)o->play_state;
    if (o->drained == 1 && o->play_state == SL_PLAYSTATE_STOPPED) { o->drained = 2; TRACE(o, "played to the end: STOPPED"); }
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetDuration(itf_t *self, uint32_t *ms) { if (ms) *ms = 0xFFFFFFFF; return SL_RESULT_SUCCESS; }
static SLresult play_GetPosition(itf_t *self, uint32_t *ms) { if (ms) *ms = 0; return SL_RESULT_SUCCESS; }
static SLresult ok(void) { return SL_RESULT_SUCCESS; }
static const vfn play_vtbl[] = {
    play_SetPlayState, play_GetPlayState, play_GetDuration, play_GetPosition,
    ok /* RegisterCallback */, ok /* SetCallbackEventsMask */, unsupported /* GetCallbackEventsMask */,
    ok /* SetMarkerPosition */, ok /* ClearMarkerPosition */, unsupported /* GetMarkerPosition */,
    ok /* SetPositionUpdatePeriod */, unsupported /* GetPositionUpdatePeriod */
};

/* SLBufferQueueItf / SLAndroidSimpleBufferQueueItf */
static SLresult bq_Enqueue(itf_t *self, const void *buf, uint32_t size) {
    sl_object *o = self->owner;
    if (!buf || !size) return SL_RESULT_PARAMETER_INVALID;
    lock();
    if (o->q_count >= QUEUE_MAX) { unlock(); return SL_RESULT_BUFFER_INSUFFICIENT; }
    qbuf_t *q = &o->queue[(o->q_head + o->q_count) % QUEUE_MAX];
    q->data = buf;
    q->frames = size / (2u * (uint32_t)o->channels);
    o->q_count++;
    unlock();
    unsigned ms = (unsigned)(q->frames * 1000ull / (o->rate ? o->rate : 1));
    if (ms >= 1500 && !o->in_callback && trace_n >= TRACE_MAX)   /* past the trace limit, still show long sounds */
        TRACE_LONG(o, "enqueue %u bytes = %u ms (%d ch, %u Hz)", size, ms, o->channels, (unsigned)o->rate);
    else
        TRACE(o, "enqueue %u bytes = %u ms%s", size, ms, o->in_callback ? " (from its callback: loop)" : "");
    return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(itf_t *self) {
    sl_object *o = self->owner;
    lock();
    o->q_head = o->q_count = 0;
    o->pos = 0;
    unlock();
    return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(itf_t *self, SLBufferQueueState *st) {
    if (!st) return SL_RESULT_PARAMETER_INVALID;
    st->count = (uint32_t)self->owner->q_count;
    st->playIndex = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(itf_t *self, void (*cb)(void *, void *), void *ctx) {
    sl_object *o = self->owner;
    TRACE(o, "register buffer callback %s", cb ? "(looping sound)" : "(none)");
    lock();
    o->callback = cb;
    o->callback_ctx = ctx;
    unlock();
    return SL_RESULT_SUCCESS;
}
static const vfn bq_vtbl[] = { bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback };

/* SLVolumeItf */
static SLresult vol_SetVolumeLevel(itf_t *self, int16_t mb) {
    sl_object *o = self->owner;
    o->level_mb = mb;
    o->gain = mb <= -9600 ? 0.f : powf(10.f, (float)mb / 2000.f);
    return SL_RESULT_SUCCESS;
}
static SLresult vol_GetVolumeLevel(itf_t *self, int16_t *mb) { if (!mb) return SL_RESULT_PARAMETER_INVALID; *mb = self->owner->level_mb; return SL_RESULT_SUCCESS; }
static SLresult vol_GetMaxVolumeLevel(itf_t *self, int16_t *mb) { if (!mb) return SL_RESULT_PARAMETER_INVALID; *mb = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_SetMute(itf_t *self, uint32_t m) { self->owner->mute = m != 0; return SL_RESULT_SUCCESS; }
static SLresult vol_GetMute(itf_t *self, uint32_t *m) { if (m) *m = (uint32_t)self->owner->mute; return SL_RESULT_SUCCESS; }
static const vfn vol_vtbl[] = {
    vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel, vol_SetMute, vol_GetMute,
    ok /* EnableStereoPosition */, unsupported /* IsEnabledStereoPosition */, ok /* SetStereoPosition */,
    unsupported /* GetStereoPosition */
};

static sl_object *object_new(int kind) {
    sl_object *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->kind = kind;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    o->obj = (itf_t){ obj_vtbl, o };
    o->engine = (itf_t){ engine_vtbl, o };
    o->play = (itf_t){ play_vtbl, o };
    o->bq = (itf_t){ bq_vtbl, o };
    o->vol = (itf_t){ vol_vtbl, o };
    o->slot = -1;
    return o;
}

static SLresult obj_GetInterface(itf_t *self, SLInterfaceID iid, void *out) {
    sl_object *o = self->owner;
    itf_t **p = out;
    if (!p) return SL_RESULT_PARAMETER_INVALID;
    itf_t *r = NULL;
    if (o->kind == KIND_ENGINE && iid_is(iid, &iid_engine)) r = &o->engine;
    else if (o->kind == KIND_PLAYER) {
        if (iid_is(iid, &iid_play)) r = &o->play;
        else if (iid_is(iid, &iid_volume)) r = &o->vol;
        else if (iid_is(iid, &iid_bq) || iid_is(iid, &iid_abq)) r = &o->bq;
    }
    if (!r) return SL_RESULT_FEATURE_UNSUPPORTED;
    *p = r;
    return SL_RESULT_SUCCESS;
}

static void obj_Destroy(itf_t *self) {
    sl_object *o = self->owner;
    if (o->kind == KIND_PLAYER) {
        TRACE(o, "destroy");
        lock();
        if (o->slot >= 0) players[o->slot] = NULL;
        o->slot = -1;
        o->callback = NULL;
        if (o->in_callback && sceKernelGetThreadId() == mix_thread_id) {
            o->destroyed = 1;   /* called from its own callback: the mixer frees it when the callback returns */
            unlock();
            return;
        }
        /* the mixer may be inside this player's callback right now (unlocked): wait for it */
        while (o->in_callback) {
            unlock();
            sceKernelDelayThread(500);
            lock();
        }
        unlock();
    }
    free(o);
}

/* ---- mixer ---------------------------------------------------------------------------------------------------- */

#define OUT_RATE   48000
#define OUT_FRAMES 512

static int32_t acc[OUT_FRAMES * 2];

/* Mix as much of one player as fits in the block. Called with the lock held; drops it around buffer callbacks. */
static void mix_player(sl_object *o) {
    uint32_t step = (o->rate << 16) / OUT_RATE;
    float target = o->mute ? 0.f : o->gain;
    float g = o->cur_gain, dg = (target - g) / OUT_FRAMES;   /* ramp volume changes over the block: no zipper noise */
    int i = 0;
    while (i < OUT_FRAMES && o->play_state == SL_PLAYSTATE_PLAYING) {
        if (o->q_count == 0) {
            /* out of data: a one-shot is finished (see the top of this file); a looping sound waits for its callback */
            if (!o->callback) { o->play_state = SL_PLAYSTATE_STOPPED; o->pos = 0; o->drained = 1; }
            break;
        }
        qbuf_t *q = &o->queue[o->q_head];
        const int16_t *d = q->data;
        uint32_t last = q->frames - 1;
        while (i < OUT_FRAMES && (o->pos >> 16) < q->frames) {
            /* linear interpolation between neighbouring frames (nearest-neighbour upsampling of 22 kHz sounds
             * to 48 kHz is audibly gritty) */
            uint32_t k = (uint32_t)(o->pos >> 16), k1 = k < last ? k + 1 : k;
            int fr = (int)(o->pos & 0xffff) >> 1;   /* 15 bits: (difference * fraction) must fit in 32 bits */
            int l, r;
            if (o->channels == 2) {
                l = d[k * 2] + (((d[k1 * 2] - d[k * 2]) * fr) >> 15);
                r = d[k * 2 + 1] + (((d[k1 * 2 + 1] - d[k * 2 + 1]) * fr) >> 15);
            } else {
                l = r = d[k] + (((d[k1] - d[k]) * fr) >> 15);
            }
            acc[i * 2] += (int32_t)(l * g);
            acc[i * 2 + 1] += (int32_t)(r * g);
            g += dg;
            o->pos += step;
            i++;
        }
        if ((o->pos >> 16) >= q->frames) {
            /* buffer done: pop it and tell the engine (looping sounds enqueue the same buffer again) */
            o->pos -= (uint64_t)q->frames << 16;
            if (o->pos >> 16) o->pos = 0;
            o->q_head = (o->q_head + 1) % QUEUE_MAX;
            o->q_count--;
            void (*cb)(void *, void *) = o->callback;
            if (cb) {
                o->in_callback = 1;
                unlock();
                cb(&o->bq, o->callback_ctx);
                lock();
                o->in_callback = 0;
                if (o->destroyed) { free(o); return; }
                if (o->slot < 0) return;   /* destroyed by another thread meanwhile (it waits for us to unlock) */
            }
        }
    }
    o->cur_gain = target;
}

/* Master gain and limiter. On Android the device volume is applied to every track before the system mixes them, so
 * full-scale dialogue plus sound effects rarely overflow; on the Vita the volume buttons act after our mix, so the
 * mix itself has to leave headroom. Above 3/4 of full scale a soft knee bends peaks under full scale instead of
 * clipping them (hard clipping is what sounded "blown out" and crackly: the dialogue files already peak at 0 dBFS). */
static int16_t limit(int32_t v) {
    float x = (float)v * (DM_AUDIO_MASTER / 32768.f);
    float ax = x < 0 ? -x : x;
    if (ax > 0.75f) {
        float y = 0.75f + 0.25f * tanhf((ax - 0.75f) / 0.25f);
        x = x < 0 ? -y : y;
    }
    return (int16_t)(x * 32767.f);
}

static int mix_thread(SceSize args, void *argp) {
    /* Two output buffers, used alternately. sceAudioOutOutput() returns as soon as the PREVIOUS buffer has been
     * played and keeps reading the one just passed while we mix the next block - mixing into the same buffer again
     * overwrote samples that were still playing: crackle and a buzz on the console (emulators copy the buffer at
     * once, so Vita3K sounded clean). */
    static int16_t out[2][OUT_FRAMES * 2] __attribute__((aligned(64)));
    int which = 0;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_FRAMES, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, OUT_FRAMES, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        l_error("[opensl] no audio port (0x%08x): sound effects will be silent, but the game keeps running", (unsigned)port);
    else {
        int vol[2] = { 32768, 32768 };
        sceAudioOutSetVolume(port, 3, vol);
        l_info("[opensl] audio mixer ready (port %d, %d Hz, master %.2f)", port, OUT_RATE, (double)DM_AUDIO_MASTER);
    }
    while (1) {
        memset(acc, 0, sizeof(acc));
        lock();
        for (int s = 0; s < MAX_PLAYERS; s++) {
            sl_object *o = players[s];
            if (o && o->play_state == SL_PLAYSTATE_PLAYING) mix_player(o);
        }
        unlock();
        song_mix(acc, OUT_FRAMES);   /* music, ambience and dialogue (song.c) */
        int16_t *o = out[which];
        which ^= 1;
        for (int i = 0; i < OUT_FRAMES * 2; i++) o[i] = limit(acc[i]);
        mixed_blocks++;
        if (port < 0 || sceAudioOutOutput(port, o) < 0)
            sceKernelDelayThread(OUT_FRAMES * 1000000 / OUT_RATE);   /* keep time even without a port */
    }
    return 0;
}

/* ---- entry points ---------------------------------------------------------------------------------------------- */

void opensl_init(void) {
    if (lock_ready) return;
    sceKernelCreateLwMutex(&mix_lock, "dm_opensl", 2 /* recursive */, 0, NULL);
    lock_ready = 1;
    mix_thread_id = sceKernelCreateThread("dm_opensl_mixer", mix_thread, 0x40, 0x4000, 0, 0, NULL);
    if (mix_thread_id >= 0) { sceKernelStartThread(mix_thread_id, 0, NULL); watchdog_register_thread("dm_opensl_mixer", mix_thread_id); }
    else l_error("[opensl] cannot start the mixer thread: 0x%08x", (unsigned)mix_thread_id);
}

uint32_t dm_slCreateEngine(void *pEngine, uint32_t nopt, const void *opts, uint32_t n, const void *ids,
                           const uint32_t *req) {
    itf_t **out = pEngine;
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    opensl_init();
    sl_object *o = object_new(KIND_ENGINE);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    *out = &o->obj;
    l_info("[opensl] engine created (loader's own OpenSL ES)");
    return SL_RESULT_SUCCESS;
}

void opensl_report(void) {
    int n = 0, playing = 0, starved = 0;
    lock();
    for (int s = 0; s < MAX_PLAYERS; s++) {
        sl_object *o = players[s];
        if (!o) continue;
        n++;
        if (o->play_state == SL_PLAYSTATE_PLAYING) { playing++; if (!o->q_count) starved++; }
    }
    unlock();
    l_warn("[opensl] %d players (%d playing, %d of them finished their data), %u blocks mixed", n, playing, starved,
           mixed_blocks);
}
