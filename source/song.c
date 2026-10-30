/*
 * Music, ambience and spoken dialogue ("songs").
 *
 * On Android, BaseUE3JavaApp plays these with a MediaPlayer: JavaCallback_PlaySong(name, loop) opens
 * "UDKGame/Music/<name>.mp3" inside the expansion files (stored, uncompressed, in the .obb zips) and a 50 ms timer
 * fades between songs. All of the game's dialogue (vo_*.mp3), its music (mus_*, cin_*) and the room ambience
 * (sfx_amb_*) go through this path; only short sound effects use OpenSL ES.
 *
 * This file reimplements that player: one stream at a time, decoded with libmpg123 straight out of the .obb,
 * resampled to 48 kHz and mixed together with the sound effects, with the same volume and fade rules as the Java code:
 *
 *   gain = fade * base * soundClass * music
 *   PlaySong while a song plays  -> fade the current one out (fadeOut s), then start the new one, fading in (fadeIn s)
 *   StopSong                     -> fade out, then stop
 *   SetSongBaseVolume / SetSongFadeValues while a song plays only apply to the next song (as on Android)
 */
#include "utils/watchdog.h"
#include "song.h"
#include "movie.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <mpg123.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>

#include "dm_config.h"
#include "utils/logger.h"

/* ---- finding a file inside the .obb zips ----------------------------------------------------------------------- */

typedef struct { char *name; int obb; uint32_t offset, size; } zip_entry_t;

static const char *obb_paths[2] = { DM_OBB_PATCH, DM_OBB_MAIN };   /* the patch file wins, like on Android */
static zip_entry_t *entries;
static int entries_n;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

/* Index the stored "UDKGame/Music/" entries of one zip (central directory -> local header -> data offset). */
static void index_zip(int obb) {
    FILE *f = fopen(obb_paths[obb], "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    long tail = len < 0x10000 + 22 ? len : 0x10000 + 22;
    uint8_t *t = malloc(tail);
    uint8_t *dir = NULL;
    if (!t) goto out;
    fseek(f, len - tail, SEEK_SET);
    if (fread(t, 1, tail, f) != (size_t)tail) goto out;
    long e = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(t + i) == 0x06054b50) { e = i; break; }
    if (e < 0) { l_warn("[song] %s: no zip directory found", obb_paths[obb]); goto out; }
    uint32_t count = rd16(t + e + 10), dsize = rd32(t + e + 12), doff = rd32(t + e + 16);
    dir = malloc(dsize);
    if (!dir) goto out;
    fseek(f, doff, SEEK_SET);
    if (fread(dir, 1, dsize, f) != dsize) goto out;

    const char *prefix = "UDKGame/Music/";
    size_t plen = strlen(prefix);
    uint32_t p = 0;
    for (uint32_t i = 0; i < count && p + 46 <= dsize; i++) {
        if (rd32(dir + p) != 0x02014b50) break;
        uint16_t method = rd16(dir + p + 10);
        uint32_t csize = rd32(dir + p + 20);
        uint16_t nlen = rd16(dir + p + 28), xlen = rd16(dir + p + 30), clen = rd16(dir + p + 32);
        uint32_t lho = rd32(dir + p + 42);
        const char *name = (const char *)dir + p + 46;
        if (nlen > plen && memcmp(name, prefix, plen) == 0 && method == 0) {
            uint8_t lh[30];
            fseek(f, lho, SEEK_SET);
            if (fread(lh, 1, 30, f) == 30 && rd32(lh) == 0x04034b50) {
                zip_entry_t *ne = realloc(entries, (entries_n + 1) * sizeof(*entries));
                if (ne) {
                    entries = ne;
                    entries[entries_n].name = strndup(name + plen, nlen - plen);
                    entries[entries_n].obb = obb;
                    entries[entries_n].offset = lho + 30 + rd16(lh + 26) + rd16(lh + 28);
                    entries[entries_n].size = csize;
                    entries_n++;
                }
            }
        }
        p += 46 + nlen + xlen + clen;
    }
out:
    free(t);
    free(dir);
    fclose(f);
}

static const zip_entry_t *find_song(const char *name) {
    static int indexed;
    if (!indexed) {
        indexed = 1;
        index_zip(0);
        index_zip(1);
        l_info("[song] %d songs found in the expansion files", entries_n);
    }
    char want[160];
    snprintf(want, sizeof(want), "%s.mp3", name);
    for (int i = 0; i < entries_n; i++)
        if (strcasecmp(entries[i].name, want) == 0) return &entries[i];
    return NULL;
}

/* ---- design ----------------------------------------------------------------------------------------------------
 * Three parties, so that file reads and MP3 decoding can never starve the audio output (that caused crackling):
 *   - the game thread (JavaCallback_*Song*) only changes the player state below, under `lock`, and never blocks;
 *   - the decoder thread owns the stream (file + libmpg123): it opens/closes it on request and keeps a ~1 s ring of
 *     48 kHz frames filled; it never holds `lock` while reading or decoding;
 *   - song_mix(), called by the sound-effect mixer thread (reimpl/opensl.c) for every block, advances the fades
 *     (the Java 50 ms timer) and adds the ring to the mix, applying the gain.
 * ------------------------------------------------------------------------------------------------------------- */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* player state (mirrors the Java fields) - guarded by `lock` */
static int active;                                   /* a song is playing (MediaPlayer `s` != null) */
static int paused;
static uint64_t started_us;
static char cur_name[128];                           /* the song playing now (for the log) */
static float base_vol = 1.f, next_base_vol = 1.f;    /* u, w */
static float class_vol = 1.f;                        /* v */
static float music_vol = 1.f;                        /* I */
static float fade_in = 1.f, next_fade_in = 1.f;      /* x, y */
static float fade_out = 1.f, next_fade_out = 1.f;    /* z, A */
static float fade_t;                                 /* E: > 0 fading in, < 0 fading out, 0 steady */
static char pending[128];                            /* t: song to start once the current one has faded out */
static int pending_loop;                             /* B */
static int pending_set;
static int pending_deferred;                         /* r54: held back until the current dialogue line ends */
static float fade_len;                               /* length of the fade-out in progress */
/* r58: a cutscene track (cin_*, music with the lines mixed in) keeps playing after the engine asks for music or ambience
 * (or a stop) until it goes quiet, at most TAIL_MAX_US: the trickster's choice scene asked for its combat music 1.5 s
 * before the last line in cin_defeat_trickster_choice_flmx was over. */
#define TAIL_MAX_US   2500000ull
#define TAIL_QUIET_MS 200
#define TAIL_PEAK     600                            /* below this (of 32767) the track counts as quiet */
static int pending_tail, tail_stop;                  /* waiting for the cutscene track to go quiet; then stop instead */
static uint64_t tail_since;
static volatile unsigned tail_quiet_ms;              /* consecutive quiet output, measured by the mixer */
static volatile int tail_done;                       /* set by the mixer, logged by the decoder thread: 1 quiet, 2 cap */
static volatile unsigned tail_done_ms;
static char tail_cin[128], tail_next[128];

/* requests to the decoder thread - guarded by `lock` */
static int req_seq;                                  /* bumped on every open/close request */
static char req_name[128];                           /* "" = close */
static int req_loop;
static int dec_finished;                             /* the decoder reached the end of a non-looping song */

/* ring of decoded 48 kHz stereo frames: single producer (decoder), single consumer (mixer). The Vita's Cortex-A9 cores
 * reorder memory accesses, so both sides use barriers around the index updates: without them the mixer could read
 * frames before the decoder's writes were visible (stale samples = crackle on the console, but not on a PC). */
#define OUT_RATE   48000
#define RING_FRAMES 65536                            /* ~1.4 s */
static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_w, ring_r;             /* frame counters (wrap freely) */
static volatile uint32_t flush_gen, flush_at;        /* consumer skips to flush_at when flush_gen changes */
static volatile uint64_t first_out_us;               /* r68: when the current song's first frames were mixed */
static uint64_t asked_us;                            /* r68: when the decoder was asked for it */
static volatile unsigned underruns, underrun_gap;     /* counted by the mixer, logged by the decoder thread (the
                                                       * mixer must never wait on a log write) */

/* r67: a song is starting and has less than 0.5 s buffered: the package prefetch (pkg_cache.c) keeps off the memory
 * card meanwhile. In r66 it was reading packages when "cin_intro_flmx" was asked for, and the song started ~1.5 s late. */
volatile int song_io_busy;

static void request_locked(const char *name, int loop) {
    if (name && *name) { song_io_busy = 1; asked_us = sceKernelGetProcessTimeWide(); }
    snprintf(req_name, sizeof(req_name), "%s", name ? name : "");
    req_loop = loop;
    req_seq++;
    dec_finished = 0;
}

static int is_vo(const char *n) { return n && !strncmp(n, "vo_", 3); }
/* r55: only room ambience and background music wait for a dialogue line. Cutscene music (cin_*) starts at once, as
 * on Android: r54 held "cin_witch_2nd_encounter_flmx" back ~8 s behind a trickster line. */
static int waits_for_vo(const char *n) { return n && (!strncmp(n, "sfx_amb_", 8) || !strncmp(n, "mus_", 4)); }
static int is_cin(const char *n) { return n && !strncmp(n, "cin_", 4); }
int dm_song_android_cuts;   /* DM_SONG_ANDROID_CUTS 1 in config.txt: exact Android behaviour (lines can be cut short) */

/* JavaCallback_PlaySong's runnable (class z), with the lock held. */
static void play_locked(const char *name, int loop) {
    if (!name || !*name || music_vol == 0.f) return;
    if (!active) {
        active = 1;
        paused = 0;
        started_us = sceKernelGetProcessTimeWide();
        snprintf(cur_name, sizeof(cur_name), "%s", name);
        fade_t = fade_in;
        request_locked(name, loop);   /* logged by the decoder thread when it opens the stream */
    } else {
        int new_vo = is_vo(name), cur_vo = is_vo(cur_name);
        snprintf(pending, sizeof(pending), "%s", name);
        pending_loop = loop;
        pending_set = 1;
        pending_deferred = 0;
        int was_tail = pending_tail;
        pending_tail = 0;
        tail_stop = 0;
        if (is_cin(cur_name) && waits_for_vo(name) && fade_t >= 0.f && !dm_song_android_cuts) {
            /* r58: let the cutscene's last line finish (see TAIL_MAX_US) */
            pending_tail = 1;
            if (!was_tail) { tail_since = sceKernelGetProcessTimeWide(); tail_quiet_ms = 0; }
            snprintf(tail_cin, sizeof(tail_cin), "%s", cur_name);
            snprintf(tail_next, sizeof(tail_next), "%s", name);
            return;
        }
        if (cur_vo && waits_for_vo(name) && fade_t >= 0.f && !dm_song_android_cuts) {
            /* r54: music or ambience asked for while a dialogue line plays: the engine times its lines itself and asks
             * for the room's ambience when it thinks the line is over, which here came 1-3 s too early and cut the end
             * of the sentence. Let the line finish; the ambience starts when it does. */
            pending_deferred = 1;
            return;
        }
        if (fade_t >= 0.f) {
            /* a dialogue line replacing ambience or music starts after a short fade, so it is not late against the
             * engine's own timing for it (the full fade-out delayed every line by that much) */
            fade_len = (new_vo && !cur_vo && !dm_song_android_cuts && fade_out > 0.3f) ? 0.3f : fade_out;
            fade_t = -fade_len;
        }
    }
}

static void stop_now_locked(void) {
    active = 0;
    fade_t = 0.f;
    request_locked("", 0);
}

/* The 50 ms timer (BaseUE3JavaApp.A): advance the fade and return the gain to apply now.
 * Like the Java timer it keeps running when no song is playing: if the current song ends by itself while it is fading
 * out (onCompletion only releases the MediaPlayer, it does not touch the fade), the queued song still starts when the
 * fade time is up. r16 stopped the timer with the song, and the voice line queued behind the fade was lost (after
 * reawakening, cin_awaken_flmx ended during the fade and vo_trickster_part1_03 never played). */
static float advance_locked(float dt) {
    float g = base_vol * class_vol;
    if (fade_t > 0.f) {
        fade_t -= dt;
        if (fade_t <= 0.f) fade_t = 0.f;
        else g *= 1.f - fade_t / fade_in;
    } else if (fade_t < 0.f) {
        fade_t += dt;
        if (fade_t >= 0.f) {
            stop_now_locked();
            base_vol = next_base_vol;
            fade_in = next_fade_in;
            fade_out = next_fade_out;
            if (pending_set) {
                pending_set = 0;
                char name[128];
                snprintf(name, sizeof(name), "%s", pending);
                pending[0] = 0;
                play_locked(name, pending_loop);
            }
            return 0.f;
        }
        g *= -fade_t / (fade_len > 0.f ? fade_len : fade_out);
    }
    return active ? g * music_vol : 0.f;
}

/* ---- decoder thread -------------------------------------------------------------------------------------------- */

typedef struct {
    const zip_entry_t *entry;
    int loop;
    SceUID fd;              /* shared, never closed: see obb_fd() */
    uint32_t pos;           /* next byte of the mp3 to feed, relative to the entry */
    mpg123_handle *mh;
    long rate;
    uint32_t frames_out;    /* source frames decoded since the stream was opened */
    const char *why;        /* why the last refill() failed */
    int last_err, last_io;
} stream_t;

static stream_t st;
static int16_t src[4096 * 2 + 2];
static int src_n;
static uint32_t src_pos;    /* 16.16 into src */

/* One descriptor per .obb, opened the first time it is needed and kept open. Reads use sceIoPread with an explicit
 * offset, so there is no FILE buffer or seek position to go stale and nothing is opened or allocated while the
 * engine is busy loading a level (r14 lost two dialogue lines exactly then: the stream opened but read no audio). */
static SceUID obb_fd(int obb) {
    static SceUID fds[2] = { -1, -1 };
    if (fds[obb] < 0) fds[obb] = sceIoOpen(obb_paths[obb], SCE_O_RDONLY, 0);
    return fds[obb];
}

static void stream_close(void) {
    if (st.mh) { mpg123_close(st.mh); mpg123_delete(st.mh); }
    memset(&st, 0, sizeof(st));
    st.fd = -1;
    src_n = 0;
    src_pos = 0;
}

static int stream_rewind(void) {
    mpg123_close(st.mh);
    if (mpg123_open_feed(st.mh) != MPG123_OK) return 0;
    st.pos = 0;
    return 1;
}

static int stream_open(const char *name, int loop) {
    stream_close();
    const zip_entry_t *e = find_song(name);
    if (!e) { l_warn("[song] \"%s\" is not in the expansion files", name); return 0; }
    st.fd = obb_fd(e->obb);
    if (st.fd < 0) { l_error("[song] cannot open %s: 0x%08x", obb_paths[e->obb], (unsigned)st.fd); return 0; }
    int err = 0;
    st.mh = mpg123_new(NULL, &err);
    if (!st.mh) { l_error("[song] mpg123_new failed: %d (out of memory?)", err); return 0; }
    mpg123_param(st.mh, MPG123_FLAGS, MPG123_FORCE_STEREO | MPG123_QUIET, 0);
    mpg123_format_none(st.mh);
    const long *rates;
    size_t nrates;
    mpg123_rates(&rates, &nrates);
    for (size_t i = 0; i < nrates; i++) mpg123_format(st.mh, rates[i], MPG123_STEREO, MPG123_ENC_SIGNED_16);
    st.entry = e;
    if (!stream_rewind()) { l_error("[song] mpg123_open_feed failed"); stream_close(); return 0; }
    st.loop = loop;
    st.rate = 44100;
    l_info("[song] play \"%s\"%s", name, loop ? " (loop)" : "");
    return 1;
}

/* Decode the next chunk into src (keeping the last frame for interpolation).
 * 1 = got frames, 0 = end of the song, -1 = failed (st.why says why). */
static int refill(void) {
    int keep = 0;
    if (src_n > 0) {
        src[0] = src[(src_n - 1) * 2];
        src[1] = src[(src_n - 1) * 2 + 1];
        src_pos -= (uint32_t)(src_n - 1) << 16;
        keep = 1;
    }
    src_n = keep;
    for (int tries = 0; tries < 512 && src_n <= keep; tries++) {
        size_t done = 0;
        int r = mpg123_read(st.mh, (unsigned char *)(src + src_n * 2), (4096 - src_n) * 4, &done);
        src_n += (int)(done / 4);
        if (r == MPG123_NEW_FORMAT) {
            int ch, enc;
            mpg123_getformat(st.mh, &st.rate, &ch, &enc);
        } else if (r == MPG123_NEED_MORE) {
            if (st.pos >= st.entry->size) {
                if (src_n > keep) break;
                if (st.loop && st.frames_out && stream_rewind()) continue;
                return 0;
            }
            static unsigned char in[16384];   /* decoder thread only */
            uint32_t want = st.entry->size - st.pos;
            if (want > sizeof(in)) want = sizeof(in);
            int got = sceIoPread(st.fd, in, want, (SceOff)st.entry->offset + st.pos);
            if (got <= 0) { st.why = "read failed"; st.last_io = got; return -1; }
            int fr = mpg123_feed(st.mh, in, (size_t)got);
            if (fr != MPG123_OK) { st.why = "mpg123_feed failed (out of memory?)"; st.last_err = fr; return -1; }
            st.pos += (uint32_t)got;
        } else if (r == MPG123_DONE) {
            if (src_n > keep) break;
            if (st.loop && st.frames_out && stream_rewind()) continue;
            return 0;
        } else if (r != MPG123_OK) {
            st.why = "decode error";
            st.last_err = r;
            return -1;
        }
    }
    if (src_n <= keep) { st.why = "no audio frames found"; return -1; }
    st.frames_out += (uint32_t)(src_n - keep);
    return 1;
}

static int decoder_thread(SceSize args, void *argp) {
    int seen_seq = 0;
    int retries = 0;
    unsigned logged_underruns = 0;
    char cur[128] = "";
    int cur_loop = 0;
    /* r69: list the songs now, at start. r68: done at the first PlaySong, the scan of the expansion files took 555 ms,
     * and the intro's cutscene track (all of the trickster's first lines) started that much behind its subtitles. */
    find_song("");
    while (1) {
        pthread_mutex_lock(&lock);
        int seq = req_seq;
        char name[128];
        int loop = req_loop;
        snprintf(name, sizeof(name), "%s", req_name);
        pthread_mutex_unlock(&lock);

        if (seq != seen_seq) {           /* open or close the stream, and drop what is buffered */
            seen_seq = seq;
            retries = 0;
            snprintf(cur, sizeof(cur), "%s", name);
            cur_loop = loop;
            stream_close();
            flush_at = ring_w;
            __sync_synchronize();
            flush_gen++;
            if (name[0] && !stream_open(name, loop)) {
                pthread_mutex_lock(&lock);
                if (req_seq == seq) dec_finished = 1;
                pthread_mutex_unlock(&lock);
                song_io_busy = 0;
            }
            if (!name[0]) song_io_busy = 0;
        }

        if (first_out_us) {   /* r68: how long from the request to the first sound */
            uint64_t f = first_out_us;
            first_out_us = 0;
            static int nf;
            if (nf++ < 300) l_info("[song] \"%s\" audible now: %u ms after it was asked for", cur, asked_us && f > asked_us ? (unsigned)((f - asked_us) / 1000) : 0);
        }
        if (tail_done) {
            int how = tail_done;
            tail_done = 0;
            l_info("[song] \"%s\" %s %u ms after the engine asked for %s%s%s", tail_cin,
                   how == 1 ? "went quiet" : "was still going (limit reached)", tail_done_ms,
                   tail_next[0] ? "\"" : "a stop", tail_next[0] ? tail_next : "", tail_next[0] ? "\"" : "");
        }
        unsigned u = underruns;
        if (u != logged_underruns) {
            for (unsigned k = logged_underruns + 1; k <= u; k++)
                if (k <= 20 || k % 64 == 0) {
                    l_warn("[song] decoder fell behind: gap of %u frames (%u so far)", underrun_gap, u);
                    break;
                }
            logged_underruns = u;
        }

        uint32_t used = ring_w - ring_r;
        uint32_t room = used >= RING_FRAMES ? 0 : RING_FRAMES - used;   /* r64: the lead-in silence can put it over until
                                                                         * the mixer has skipped the flushed part */
        __sync_synchronize();   /* acquire: the consumer has finished reading what it gave back before we overwrite it */
        if (!st.mh || room < 1024) {
            song_io_busy = 0;   /* r67: nothing to read, or the ring is full */
            sceKernelDelayThread(5 * 1000);
            continue;
        }

        /* resample src (any rate) into the ring at 48 kHz */
        uint32_t step = (uint32_t)(((uint64_t)st.rate << 16) / OUT_RATE);
        uint32_t w = ring_w;
        for (uint32_t n = 0; n < room - 16; n++) {
            if ((src_pos >> 16) + 1 >= (uint32_t)src_n) {
                int r = refill();
                if (r <= 0) {
                    uint32_t got = st.frames_out;
                    if (r < 0)
                        l_warn("[song] \"%s\": %s after %u of %u bytes, %u frames (mpg123 %d%s%s, read 0x%08x)",
                               st.entry->name, st.why, (unsigned)st.pos, (unsigned)st.entry->size, (unsigned)got,
                               st.last_err, st.last_err ? ": " : "", st.last_err ? mpg123_plain_strerror(st.last_err) : "",
                               (unsigned)st.last_io);
                    /* A song that produced no audio at all did not really play: try again (3 times, 0.6 s in all)
                     * before telling the game it ended, or the line is skipped and only its subtitle shows. */
                    if (got == 0 && retries < 3) {
                        retries++;
                        l_warn("[song] \"%s\" gave no audio, retrying (%d)", st.entry->name, retries);
                        sceKernelDelayThread(retries * 100 * 1000);
                        if (stream_open(cur, cur_loop)) break;
                    } else {
                        unsigned td = (unsigned)(sceKernelGetProcessTimeWide() / 100000);
                        l_info("[song] t=%u.%u s \"%s\" decoded to the end (%u frames = %u ms; playback ends up to 1.4 s later)",
                               td / 10, td % 10, st.entry->name, (unsigned)got,
                               st.rate ? (unsigned)((uint64_t)got * 1000 / st.rate) : 0);
                    }
                    stream_close();
                    pthread_mutex_lock(&lock);
                    if (req_seq == seen_seq) dec_finished = 1;
                    pthread_mutex_unlock(&lock);
                    break;
                }
                if (n >= 4096) break;   /* hand over regularly so requests are noticed quickly */
            }
            uint32_t k = src_pos >> 16;
            int fr = (int)(src_pos & 0xffff) >> 1;   /* 15 bits: (b - a) * fraction must fit in 32 bits; with 16
                                                      * it overflowed on steep edges and clicked */
            int16_t *o = ring + (w % RING_FRAMES) * 2;
            for (int c = 0; c < 2; c++) {
                int a = src[k * 2 + c], b = src[(k + 1) * 2 + c];
                o[c] = (int16_t)(a + (((b - a) * fr) >> 15));
            }
            src_pos += step;
            w++;
        }
        __sync_synchronize();
        ring_w = w;
        if (song_io_busy && (ring_w - ring_r >= OUT_RATE / 2 || !st.mh)) song_io_busy = 0;   /* r67: 0.5 s buffered */
    }
    return 0;
}

/* ---- output: mixed into the sound-effect mix (reimpl/opensl.c) ----------------------------------------------- */

/* Called by the OpenSL ES mixer thread for every block: advance the fades (the Java 50 ms timer) and add the song
 * to `acc` (48 kHz stereo, int32). Songs and sound effects share one port, so the final limiter sees their sum. */
void song_mix(int32_t *acc, int frames) {
    static uint32_t seen_gen;
    static float gain;
    static int had_data;     /* the current song has produced audio (gaps before that are a slow start, not a stall) */
    float dt = (float)frames / OUT_RATE;

    pthread_mutex_lock(&lock);
    float target = advance_locked(dt);
    int playing = active && !paused;
    int finished = dec_finished;
    uint64_t since = sceKernelGetProcessTimeWide() - started_us;
    if (finished && ring_w == ring_r && flush_gen == seen_gen) {   /* MediaPlayer.onCompletion: released */
        active = 0;            /* the fade timer is left alone, as in Java: a queued song still starts after a fade-out */
        dec_finished = 0;
        playing = 0;
        if (pending_tail) {   /* r58: the cutscene track ended by itself while its tail was protected */
            pending_tail = 0;
            if (tail_stop) { tail_stop = 0; pending_set = 0; pending[0] = 0; }
            else if (pending_set) pending_deferred = 1;   /* start it right away, below */
        }
        if (pending_set && pending_deferred) {   /* r54: the ambience held back behind a dialogue line */
            pending_set = 0;
            pending_deferred = 0;
            char nm[128];
            snprintf(nm, sizeof(nm), "%s", pending);
            pending[0] = 0;
            fade_t = 0.f;
            play_locked(nm, pending_loop);
        }
    }
    pthread_mutex_unlock(&lock);

    if (flush_gen != seen_gen) {
        seen_gen = flush_gen;
        __sync_synchronize();
        ring_r = flush_at;
        had_data = 0;
    }
    if (!playing) { gain = 0.f; return; }
    if (movie_playing()) target = 0.f;   /* r61: music and dialogue are silent while a movie plays (they keep going) */

    uint32_t avail = ring_w - ring_r;
    __sync_synchronize();   /* acquire: see the decoder's frames, not stale ones, before reading them (ARM reorders loads) */
    uint32_t n = avail < (uint32_t)frames ? avail : (uint32_t)frames;
    if (n && !had_data) first_out_us = sceKernelGetProcessTimeWide();   /* r68: logged by the decoder thread */
    if (n) had_data = 1;
    if (n < (uint32_t)frames && !finished && had_data && since > 500 * 1000) {
        underrun_gap = (unsigned)(frames - n);
        underruns++;
    }
    int peak = 0;
    for (uint32_t i = 0; i < n; i++) {
        float g = gain + (target - gain) * ((float)i / frames);   /* ramp: no clicks on volume steps */
        const int16_t *src_f = ring + ((ring_r + i) % RING_FRAMES) * 2;
        acc[i * 2] += (int32_t)(src_f[0] * g);
        acc[i * 2 + 1] += (int32_t)(src_f[1] * g);
        int a0 = src_f[0] < 0 ? -src_f[0] : src_f[0], a1 = src_f[1] < 0 ? -src_f[1] : src_f[1];
        if (a0 > peak) peak = a0;
        if (a1 > peak) peak = a1;
    }
    __sync_synchronize();
    ring_r += n;
    gain = target;

    if (pending_tail) {   /* r58: has the cutscene track gone quiet? (read without the lock: only a hint) */
        unsigned ms = (unsigned)frames * 1000u / OUT_RATE;
        tail_quiet_ms = (n && peak < TAIL_PEAK) ? tail_quiet_ms + ms : 0;
        pthread_mutex_lock(&lock);
        uint64_t waited = sceKernelGetProcessTimeWide() - tail_since;
        if (pending_tail && (tail_quiet_ms >= TAIL_QUIET_MS || waited >= TAIL_MAX_US)) {
            tail_done = tail_quiet_ms >= TAIL_QUIET_MS ? 1 : 2;
            tail_done_ms = (unsigned)(waited / 1000);
            pending_tail = 0;
            if (tail_stop) { tail_stop = 0; pending_set = 0; pending[0] = 0; }
            if (fade_t >= 0.f) { fade_len = fade_out; fade_t = -fade_len; }   /* now as the engine asked */
        }
        pthread_mutex_unlock(&lock);
    }
}

/* ---- the JavaCallback_* entry points ---------------------------------------------------------------------------- */

void song_init(void) {
    mpg123_init();   /* needed by older libmpg123, harmless on newer ones */
    SceUID d = sceKernelCreateThread("dm_song_decoder", decoder_thread, 0x60, 0x10000, 0, 0, NULL);
    if (d < 0) { l_error("[song] cannot start the music/dialogue decoder: 0x%08x", (unsigned)d); return; }
    sceKernelStartThread(d, 0, NULL);
    watchdog_register_thread("dm_song_decoder", d);
}

/* Every request from the engine is logged with the player state, so a line that "did not play" can be traced:
 * ignored (music volume 0), queued behind a fade-out, replaced in the queue by a later request, or paused. */
static unsigned now_ds(void) { return (unsigned)(sceKernelGetProcessTimeWide() / 100000); }   /* tenths of a second */

void song_play(const char *name, int loop) {
    char msg[300];
    int warn = 0, info = 0;
    pthread_mutex_lock(&lock);
    unsigned t = now_ds(), played = active ? (unsigned)((sceKernelGetProcessTimeWide() - started_us) / 1000) : 0;
    if (music_vol == 0.f) {
        warn = 1;
        snprintf(msg, sizeof(msg), "[song] PlaySong(\"%s\") ignored: music volume is 0", name ? name : "");
    } else if (active && pending_set)
        snprintf(msg, sizeof(msg), "[song] t=%u.%u s PlaySong(\"%s\"%s) replaces the queued \"%s\" (current song fading out)",
                 t / 10, t % 10, name, loop ? ", loop" : "", pending);
    else if (active) {
        /* r53: dialogue cut off near its end - which request ends the current song, and how far it had got */
        info = 1;
        snprintf(msg, sizeof(msg), "[song] t=%u.%u s PlaySong(\"%s\"%s) ends \"%s\" after %u ms (fading out over %.1f s)%s",
                 t / 10, t % 10, name, loop ? ", loop" : "", cur_name, played, (double)fade_out, paused ? " (paused)" : "");
    } else {
        info = 1;
        snprintf(msg, sizeof(msg), "[song] t=%u.%u s PlaySong(\"%s\"%s)", t / 10, t % 10, name, loop ? ", loop" : "");
    }
    play_locked(name, loop);
    if (pending_tail && !warn)
        snprintf(msg, sizeof(msg), "[song] t=%u.%u s PlaySong(\"%s\"%s) waits for the end of the line in \"%s\" (%u ms played; "
                 "at most %u s)", t / 10, t % 10, name, loop ? ", loop" : "", cur_name, played, (unsigned)(TAIL_MAX_US / 1000000));
    if (pending_set && pending_deferred && !warn)
        snprintf(msg, sizeof(msg), "[song] t=%u.%u s PlaySong(\"%s\"%s) waits for \"%s\" to finish (%u ms played so far)",
                 t / 10, t % 10, name, loop ? ", loop" : "", cur_name, played);
    pthread_mutex_unlock(&lock);
    /* logged after unlocking: a log write may wait for the memory card, and the mixer needs `lock` every block */
    if (warn) l_warn("%s", msg); else if (info) l_info("%s", msg); else l_debug("%s", msg);
}

/* r59: is fight music or fight ambience playing (or queued)? A hint for the physical-control profiles (padinput.c). */
int song_battle_music(void) {
    pthread_mutex_lock(&lock);
    const char *n = (pending_set && pending[0]) ? pending : (active ? cur_name : "");
    int r = !strncmp(n, "sfx_amb_battle", 14) || !strncmp(n, "mus_combat", 10);
    pthread_mutex_unlock(&lock);
    return r;
}

/* r61: is a cutscene track (cin_*) playing? A hint for hiding the controls' cursor during cutscenes (padinput.c). */
int song_cin_playing(void) {
    pthread_mutex_lock(&lock);
    int r = active && !strncmp(cur_name, "cin_", 4) && fade_t >= 0.f;
    pthread_mutex_unlock(&lock);
    return r;
}

void song_stop(void) {
    pthread_mutex_lock(&lock);
    int was_active = active, had_pending = pending_set;
    unsigned t = now_ds(), played = active ? (unsigned)((sceKernelGetProcessTimeWide() - started_us) / 1000) : 0;
    char cn[128];
    snprintf(cn, sizeof(cn), "%s", cur_name);
    int tail = active && pending_tail;
    if (tail) {
        /* r58: a stop while a cutscene line is protected: it applies when the line is over */
        tail_stop = 1;
        pending_set = 0;
        pending[0] = 0;
        tail_next[0] = 0;
    } else if (active) {
        if (fade_t >= 0.f) { fade_len = fade_out; fade_t = -fade_len; }
        pending_set = 0;
        pending_deferred = 0;
    }
    pthread_mutex_unlock(&lock);
    if (tail) l_info("[song] t=%u.%u s StopSong waits for the end of the line in \"%s\" (%u ms played); drops the queued song",
                     t / 10, t % 10, cn, played);
    else if (was_active) l_info("[song] t=%u.%u s StopSong ends \"%s\" after %u ms%s", t / 10, t % 10, cn, played,
                           had_pending ? "; drops the queued song" : "");
    else l_debug("[song] StopSong (nothing playing)");
}

/* On Android PlaySong is posted to the UI thread, while PauseSong/ResumeSong act at once on the MediaPlayer that
 * exists at that moment. So a PauseSong that follows a PlaySong immediately pauses the *previous* song, not the new
 * one. Reproduce that: a pause arriving within 100 ms of a song starting does not apply to it. */
void song_pause(int p) {
    pthread_mutex_lock(&lock);
    int young = p && active && sceKernelGetProcessTimeWide() - started_us < 100 * 1000;
    if (!young)
        paused = p;
    pthread_mutex_unlock(&lock);
    l_debug("[song] %s%s", p ? "PauseSong" : "ResumeSong", young ? " (ignored: the song started less than 0.1 s ago)" : "");
}

void song_set_base_volume(float v) {
    if (v > 1.f) v = 1.f;
    pthread_mutex_lock(&lock);
    if (active) next_base_vol = v; else base_vol = v;
    pthread_mutex_unlock(&lock);
}

void song_set_fades(float in, float out) {
    if (in < 0.001f) in = 0.001f;
    if (out < 0.001f) out = 0.001f;
    pthread_mutex_lock(&lock);
    if (active) { next_fade_in = in; next_fade_out = out; }
    else { fade_in = in; fade_out = out; }
    pthread_mutex_unlock(&lock);
}

void song_set_class_volume(float v) {
    pthread_mutex_lock(&lock);
    class_vol = v > 1.f ? 1.f : v;
    pthread_mutex_unlock(&lock);
}

void song_set_music_volume(float v) {
    pthread_mutex_lock(&lock);
    int changed = v != music_vol;
    music_vol = v;
    pthread_mutex_unlock(&lock);
    if (changed) l_debug("[song] music volume %.2f", (double)v);
}
