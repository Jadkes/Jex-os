/**
 * @file mp3.h
 * @brief MPEG-1 Layer III playback into the AC'97 PCM path.
 *
 * Purpose: turn a JexFS file into audio.  The decoder is minimp3 (CC0,
 *          third_party/minimp3.h) — see mp3.c for why it is the only
 *          translation unit built with the FPU enabled.
 *
 * Thread-safety: task context only.  One playback at a time; a second
 *          caller gets -EBUSY rather than interleaving two streams into
 *          the same audio ring.
 *
 * Stack:   the decoder's frame is about 17 KB, so callers must have at
 *          least 24 KB of stack headroom.  A kernel task stack (32 KB) is
 *          fine; the 16 KB bootstrap stack is not.
 */

#ifndef MP3_H
#define MP3_H

#include <stdint.h>

/** @struct mp3_info
 *  @brief Stream properties, filled in once the first frame is decoded. */
typedef struct {
    uint32_t sample_rate;   /**< Hz; only 44100 is playable today. */
    uint32_t bitrate_kbps;  /**< Nominal frame bitrate. */
    uint32_t frames;        /**< Sample frames decoded (per channel). */
    uint8_t  channels;      /**< 1 or 2 as stored in the file. */
} mp3_info_t;

/**
 * @brief Sink for decoded PCM, already converted to the codec format.
 *
 * Always interleaved signed 16-bit stereo at 44100 Hz.  Return 0 to keep
 * going, or a negative errno to abort the decode.
 */
typedef int (*mp3_sink_t)(const int16_t* pcm, uint32_t bytes);

/**
 * @brief Decode a whole file, handing the PCM to @p sink.
 *
 * Same streaming and limits as mp3_play_file(), but without requiring an
 * audio device — that is what lets the self-test check a real file's
 * sample rate, length and level on a headless machine.
 *
 * Needs about 24 KB of stack headroom (minimp3's frame is ~17 KB), so do
 * not call it from the bootstrap stack or any small-stack context.
 *
 * @param path File to decode.
 * @param sink Destination for each decoded frame; may be NULL to measure
 *             a file without producing audio.
 * @param stop Polled once per decoded frame; returning nonzero aborts.
 *             May be NULL.
 * @param info Optional; receives the stream properties.
 *
 * @return 0 on success, -ENOENT if the file will not open, -ENOMEM if the
 *         working set (about 48 KB) will not allocate, -EBUSY if a decode
 *         is running, -EINVAL for a sample rate or channel layout the
 *         codec path does not handle or a file with no decodable frame,
 *         -EIO if the file or the sink fails, -EINTR when @p stop asked
 *         to abort.
 */
int mp3_decode_file(const char* path, mp3_sink_t sink, int (*stop)(void),
                    mp3_info_t* info);

/**
 * @brief Decode a whole file to the AC'97 output.
 *
 * Blocks until the file has been handed to the audio ring and the ring
 * has drained, so the caller's next line prints while the last samples
 * are still audible only by a fraction of a second.
 *
 * @param path File to play.
 * @param stop Polled once per decoded frame; returning nonzero aborts.
 *             May be NULL.
 * @param info Optional; receives the stream properties.
 *
 * @return 0 on success, -ENODEV without an AC'97 controller, otherwise
 *         the codes mp3_decode_file() returns.
 */
int mp3_play_file(const char* path, int (*stop)(void), mp3_info_t* info);

#endif /* MP3_H */
