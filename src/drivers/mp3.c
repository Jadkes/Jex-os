/**
 * @file mp3.c
 * @brief MPEG-1 Layer III decoding into the AC'97 PCM path.
 *
 * Purpose: play compressed audio files.  Layer III is the interesting one
 *          — it is what ".mp3" means — and minimp3 (CC0, vendored at
 *          third_party/minimp3.h) decodes it in about 700 lines of real
 *          code instead of the ~25 k lines a fixed-point decoder needs.
 *
 * Design:
 *   - This is the only translation unit built with the x87 enabled.  The
 *     kernel proper is compiled with -mno-80387 so that an accidental
 *     float cannot silently corrupt a preempted task's registers; minimp3
 *     is float-based, so it gets an explicit per-object -m80387 in the
 *     Makefile and keeps its FPU use in one place.  It is safe only
 *     because fpu.c now saves and restores a per-task image on every
 *     switch, and because nothing in interrupt context touches x87.
 *   - The decoder state is 6.7 KB and the working set another ~42 KB, so
 *     it lives on the heap: an 8 KB kernel stack cannot hold it.
 *   - Decoding is streaming, not whole-file: a 32 KB window holds more
 *     than one maximum-size frame, and leftover bytes are moved to the
 *     front before every refill so a frame may straddle two reads.
 *   - Output is interleaved 16-bit stereo at the codec's fixed 44.1 kHz.
 *     Mono is duplicated into both channels; anything else is refused
 *     rather than played at the wrong pitch.  Resampling and VBR
 *     gapless metadata are deliberately out of scope for this cut.
 *
 * Thread-safety: task context only.  Playback takes a flag (not a held
 *          lock — the main loop blocks on the audio ring, and holding a
 *          spinlock across a sleep would stop the tick that drains it).
 *          A second caller gets -EBUSY.
 */

#define pr_fmt(fmt) "[MP3] " fmt

#include "mp3.h"
#include "ac97.h"
#include "fs.h"
#include "kheap.h"
#include "spinlock.h"
#include "timer.h"
#include "jexos/errno.h"
#include "kernel/printk.h"

/* Layer I and II support would add a few KB for formats nobody asks for;
 * dropping it also makes it honest to call the result an MP3 decoder. */
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
/* minimp3 pulls <stdlib.h>/<string.h> for three prototypes; under
 * -ffreestanding those resolve to the host headers and only the symbols
 * reach the link, where kheap.c provides memcpy/memmove/memset. */
#include "minimp3.h"

/* A maximum-size MPEG-1 frame is 1441 bytes; a 32 KB window therefore
 * always holds several frames plus the tail of the previous read, which
 * is what makes a straddling frame impossible to lose. */
#define MP3_WINDOW_BYTES (32u * 1024u)
/* Refills are a quarter of the window: frequent enough that a decode
 * never waits on a big read, small enough that the disk is not hit for
 * every 26 ms frame. */
#define MP3_READ_BYTES   (8u * 1024u)

/* The ring holds 64 KB (~0.7 s), so a controller still draining after
 * five seconds is not draining at all and the caller should get control
 * back rather than hang. */
#define MP3_DRAIN_TIMEOUT_MS 5000

static spinlock_t player_lock = SPINLOCK_INIT;
static volatile int player_busy;

/* Claim the single playback slot.  The critical section covers only the
 * flag: the decode loop below blocks on the audio ring for real time and
 * must never run with this lock held. */
static int player_claim(void)
{
    uint32_t flags;
    int busy;

    spin_lock_irqsave(&player_lock, &flags);
    busy = player_busy;
    if (!busy)
        player_busy = 1;
    spin_unlock_irqrestore(&player_lock, flags);

    return busy ? -EBUSY : 0;
}

static void player_release(void)
{
    uint32_t flags;

    spin_lock_irqsave(&player_lock, &flags);
    player_busy = 0;
    spin_unlock_irqrestore(&player_lock, flags);
}

/* Wait for everything already handed to the ring to reach the codec, so
 * a following file starts on silence instead of on this one's tail. */
static void drain_output(void)
{
    uint32_t start = get_ticks();

    while (ac97_buffered() && (get_ticks() - start) < MP3_DRAIN_TIMEOUT_MS)
        sleep(20);
}

int mp3_decode_file(const char* path, mp3_sink_t sink, int (*stop)(void),
                    mp3_info_t* info)
{
    mp3dec_t* decoder;
    uint8_t* window;
    mp3d_sample_t* samples;
    int16_t* stereo;
    mp3dec_frame_info_t frame;
    const int16_t* payload;
    uint32_t avail = 0;
    uint32_t frames = 0;
    int fd;
    int eof = 0;
    int started = 0;
    int rc;

    if (info)
        memset(info, 0, sizeof(*info));
    if (!path || !*path)
        return -EINVAL;

    rc = player_claim();
    if (rc)
        return rc;

    fd = fs_open(path, 0);
    if (fd < 0) {
        rc = -ENOENT;
        goto release;
    }

    decoder = kmalloc(sizeof(*decoder));
    window  = kmalloc(MP3_WINDOW_BYTES);
    samples = kmalloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t));
    stereo  = kmalloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    if (!decoder || !window || !samples || !stereo) {
        rc = -ENOMEM;
        goto free;
    }

    memset(&frame, 0, sizeof(frame));
    mp3dec_init(decoder);
    rc = 0;

    for (;;) {
        int decoded;

        if (!eof && avail < MP3_WINDOW_BYTES) {
            uint32_t want = MP3_WINDOW_BYTES - avail;
            int got;

            if (want > MP3_READ_BYTES)
                want = MP3_READ_BYTES;
            got = fs_read(fd, window + avail, want);
            if (got <= 0)
                eof = 1;
            else
                avail += (uint32_t)got;
        }

        if (!avail)
            break;              /* end of file and nothing queued */

        decoded = mp3dec_decode_frame(decoder, window, (int)avail,
                                      samples, &frame);

        /* frame_bytes is everything the decoder looked at, including any
         * junk it skipped while searching for a sync word. */
        if (frame.frame_bytes > 0) {
            avail -= (uint32_t)frame.frame_bytes;
            if (avail)
                memmove(window, window + frame.frame_bytes, avail);
        }

        if (decoded <= 0) {
            if (frame.frame_bytes == 0) {
                if (avail == MP3_WINDOW_BYTES) {
                    /* A frame cannot be larger than the window, so this
                     * data never syncs.  Drop a byte and rescan inside
                     * it, otherwise the loop spins forever. */
                    avail--;
                    memmove(window, window + 1, avail);
                } else if (eof) {
                    break;      /* truncated last frame; nothing to add */
                }
            }
            continue;
        }

        if (!started) {
            if (frame.hz != (int)AC97_RATE_HZ ||
                (frame.channels != 1 && frame.channels != 2)) {
                pr_err("%s: %d Hz / %d channel layout unsupported\n",
                       path, frame.hz, frame.channels);
                rc = -EINVAL;
                break;
            }
            started = 1;
            if (info) {
                info->sample_rate = (uint32_t)frame.hz;
                info->channels = (uint8_t)frame.channels;
            }
            pr_debug("%s: %d Hz, %d ch, %d kbit/s\n", path, frame.hz,
                     frame.channels, frame.bitrate_kbps);
        }

        /* The first frame can be an encoder-delay header whose nominal
         * rate differs from the stream (LAME writes 56 kbit/s ahead of a
         * 32 kbit/s mono stream).  Report the latest audio frame instead,
         * which is steady-state CBR and the last frame for VBR. */
        if (info)
            info->bitrate_kbps = (uint32_t)frame.bitrate_kbps;

        if (frame.channels == 1) {
            for (int i = 0; i < decoded; i++) {
                stereo[2 * i]     = samples[i];
                stereo[2 * i + 1] = samples[i];
            }
            payload = stereo;
        } else {
            payload = samples;
        }

        if (sink && sink(payload, (uint32_t)decoded * 4) != 0) {
            rc = -EIO;
            break;
        }

        frames += (uint32_t)decoded;
        if (stop && stop()) {
            rc = -EINTR;
            break;
        }
    }

    /* Nothing was decoded at all: either the file is not MP3 or every
     * frame was broken, and silence is not success. */
    if (!rc && !started)
        rc = -EINVAL;
    if (info)
        info->frames = frames;

free:
    kfree(decoder);
    kfree(window);
    kfree(samples);
    kfree(stereo);
    fs_close(fd);
release:
    player_release();
    return rc;
}

/* Every decoded frame goes straight to the ring, which applies the
 * real-time backpressure by blocking once it is full. */
static int ac97_sink(const int16_t* pcm, uint32_t bytes)
{
    return ac97_write(pcm, bytes) == 0 ? 0 : -EIO;
}

int mp3_play_file(const char* path, int (*stop)(void), mp3_info_t* info)
{
    int rc;

    if (!ac97_present())
        return -ENODEV;

    rc = mp3_decode_file(path, ac97_sink, stop, info);

    /* Let the queued tail reach the codec before returning: otherwise a
     * following file would be appended behind it, and the caller's "done"
     * would describe audio that is still on its way out. */
    if (rc == 0 || rc == -EINTR)
        drain_output();

    return rc;
}
