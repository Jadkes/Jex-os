/**
 * @file test_mp3.c
 * @brief Decode the sample MP3s shipped in the disk image.
 *
 * Purpose: the audio path has two halves that each look healthy while the
 *          pair is broken -- the codec plays whatever PCM it is handed,
 *          and the decoder produces PCM whether or not it is correct.  So
 *          decode a real file from the image and check the properties
 *          that only come out right when the frame parser, the bit
 *          reservoir and the synthesis filter bank all work: sample rate,
 *          channel count, duration and signal level.
 *
 * Design: the sink accumulates |sample| instead of writing to the codec,
 *          so the check needs no audio device and runs on the headless
 *          test flavor.  Two assets cover both layouts: a stereo file and
 *          a mono file whose duplicated-channel path is otherwise only
 *          reachable by ear.  The Makefile generates them into
 *          build/assets/ and packs them at the image root; a host without
 *          ffmpeg ships no assets, and the tests report a skip rather than
 *          failing a machine that cannot make one.
 *
 *          The decode itself runs on a forked kernel task, never on the
 *          caller's stack: minimp3's frame is 16.6 KB and the bootstrap
 *          stack that runs the suite is only 16 KB.  A kernel task stack
 *          is KSTACK_SIZE (32 KB), which is why mp3.h requires that much.
 */

#include "test_suite.h"
#include "kernel/completion.h"
#include "mp3.h"
#include "task.h"
#include "fs.h"
#include <stdint.h>
#include <string.h>

#define MP3_TEST_STEREO  "/test.mp3"
#define MP3_TEST_MONO    "/test-mono.mp3"

#define MP3_EXPECT_HZ    44100u

/* The encoder writes a Xing/LAME header with its own delay and padding,
 * so the decoded stream runs a few tens of milliseconds past the four
 * seconds of source.  An eighth of a second of slack still catches a
 * parser that loses frames. */
#define MP3_EXPECT_FRAMES (4u * MP3_EXPECT_HZ)
#define MP3_FRAMES_SLACK  (MP3_EXPECT_HZ / 8)

/* A 0.35-amplitude sine averages a seventh of full scale after encoding.
 * Silence averages zero and a clipping decoder would sit near the
 * ceiling; the band only has to prove "audio, not silence or static". */
#define MP3_MIN_MEAN_ABS 1000u
#define MP3_MAX_MEAN_ABS 20000u

static uint64_t pcm_energy;
static uint32_t pcm_frames;
static uint32_t pcm_channel_mismatches;

/* Decode result, passed from the worker task back to the test task.  The
 * completion is the release edge for these writes. */
static const char* decode_path;
static mp3_info_t  decode_info;
static int         decode_rc;
static completion_t decode_done;

/* The sink always receives interleaved stereo at the codec rate. */
static int collect(const int16_t* pcm, uint32_t bytes)
{
    uint32_t frames = bytes / 4;

    if (bytes % 4)
        return -1;

    for (uint32_t i = 0; i < frames; i++) {
        int32_t left  = pcm[2 * i];
        int32_t right = pcm[2 * i + 1];

        if (left != right)
            pcm_channel_mismatches++;
        pcm_energy += (uint64_t)(left < 0 ? -left : left);
        pcm_energy += (uint64_t)(right < 0 ? -right : right);
    }
    pcm_frames += frames;
    return 0;
}

static void decode_worker(void* arg)
{
    (void)arg;
    decode_rc = mp3_decode_file(decode_path, collect, NULL, &decode_info);
    completion_signal(&decode_done);
}

/**
 * Decode @p path on a kernel task and copy its result into @p info.
 *
 * @return the decoder's return code, or -1 if the worker could not start.
 */
static int decode_on_task(const char* path, mp3_info_t* info)
{
    int id;

    completion_init(&decode_done);
    memset(&decode_info, 0, sizeof(decode_info));
    decode_rc = -1;
    decode_path = path;
    pcm_energy = 0;
    pcm_frames = 0;
    pcm_channel_mismatches = 0;

    id = kernel_fork_on(decode_worker, NULL, 0);
    if (id <= 0)
        return -1;

    completion_wait(&decode_done);

    *info = decode_info;
    return decode_rc;
}

static int asset_present(const char* path)
{
    int fd = fs_open(path, 0);

    if (fd < 0)
        return 0;
    fs_close(fd);
    return 1;
}

/* Compare the mean |sample| against a bound without a 64-bit division:
 * the kernel links no libgcc, so __udivdi3 does not exist.  Scaling the
 * bound up instead is the same comparison. */
static int mean_is_above(uint64_t bound)
{
    return pcm_energy > bound * (uint64_t)pcm_frames * 2;
}

static int length_is_sane(uint32_t frames)
{
    return frames + MP3_FRAMES_SLACK > MP3_EXPECT_FRAMES &&
           frames < MP3_EXPECT_FRAMES + MP3_FRAMES_SLACK;
}

static void mp3_stereo_file(void)
{
    mp3_info_t info;

    if (!asset_present(MP3_TEST_STEREO)) {
        test_skip("mp3_decode_stereo (no asset in this image)");
        return;
    }

    TEST_ASSERT(decode_on_task(MP3_TEST_STEREO, &info) == 0,
                "mp3: stereo decode failed");
    TEST_ASSERT(info.sample_rate == MP3_EXPECT_HZ, "mp3: wrong sample rate");
    TEST_ASSERT(info.channels == 2, "mp3: stereo file reported as mono");
    TEST_ASSERT(length_is_sane(info.frames), "mp3: decoded length is not ~4 s");
    TEST_ASSERT(pcm_frames == info.frames, "mp3: sink saw a different frame count");
    TEST_ASSERT(mean_is_above(MP3_MIN_MEAN_ABS), "mp3: output is silent");
    TEST_ASSERT(!mean_is_above(MP3_MAX_MEAN_ABS), "mp3: output is clipped to noise");
}

static void mp3_mono_file(void)
{
    mp3_info_t info;

    if (!asset_present(MP3_TEST_MONO)) {
        test_skip("mp3_decode_mono (no asset in this image)");
        return;
    }

    TEST_ASSERT(decode_on_task(MP3_TEST_MONO, &info) == 0,
                "mp3: mono decode failed");
    TEST_ASSERT(info.sample_rate == MP3_EXPECT_HZ, "mp3: mono sample rate");
    TEST_ASSERT(info.channels == 1, "mp3: mono file not reported as mono");
    TEST_ASSERT(length_is_sane(info.frames), "mp3: mono length is not ~4 s");
    TEST_ASSERT(pcm_frames == info.frames, "mp3: mono sink frame count");
    TEST_ASSERT(pcm_channel_mismatches == 0,
                "mp3: mono stream was not duplicated to both channels");
    TEST_ASSERT(mean_is_above(MP3_MIN_MEAN_ABS), "mp3: mono output is silent");
}

void register_mp3_tests(void)
{
    test_register("mp3_decode_stereo",
                  "Decode the stereo sample: rate, length, level",
                  mp3_stereo_file);
    test_register("mp3_decode_mono",
                  "Mono sample decodes and is duplicated to both channels",
                  mp3_mono_file);
}
