/**
 * @file ac97.h
 * @brief Intel AC'97 (82801AA) PCM audio — 16-bit stereo line out.
 *
 * Purpose: give the kernel a sample-capable audio path.  The PC speaker
 *          (speaker.c) is a 1-bit square wave and cannot play recorded
 *          audio; this driver feeds real 16-bit PCM to the AC'97 codec,
 *          which is the prerequisite for MP3 playback.
 *
 * Design:  The AC'97 bus master consumes a list of 32 fixed-size buffers
 *          (CIV and LVI are 5-bit, so the list length is fixed at 32) and
 *          raises no interrupt we can own (QEMU puts it on the same PCI IRQ
 *          as the RTL8139, and irq.c holds one handler per line).  So the
 *          list is advanced from the PIT tick instead — see ac97_tick().
 *          Descriptors are published ahead of the engine and padded with
 *          silence only when the caller has nothing queued.
 *
 * Thread-safety: ac97_write() is task context only and blocks while the
 *          software ring is full.  ac97_tick() runs in IRQ context and
 *          takes the same lock with interrupts off, so neither can
 *          deadlock against the other.
 */

#ifndef AC97_H
#define AC97_H

#include <stdint.h>

/** PCI identity of the QEMU AC'97 controller (Intel 82801AA). */
#define AC97_PCI_VENDOR 0x8086
#define AC97_PCI_DEVICE 0x2415

/** Output format is fixed: signed 16-bit stereo at this rate. */
#define AC97_RATE_HZ    44100

/**
 * @brief Probe the AC'97 controller and start its bus master.
 *
 * Registered as a device initcall.  Call ac97_present() to find out
 * whether a controller was actually found.
 */
void ac97_init(void);

/** @brief 1 when a controller is initialised and accepting PCM. */
int ac97_present(void);

/** @brief Sample rate actually programmed, in Hz (0 when absent). */
uint32_t ac97_rate(void);

/**
 * @brief Queue interleaved signed 16-bit stereo PCM.
 *
 * Blocks while the software ring is full, so playback is paced at real
 * time rather than flooding memory.  A trailing odd byte is dropped:
 * the frame size is 4 bytes and partial frames have no meaning here.
 *
 * @param data  Interleaved L,R samples.
 * @param bytes Byte count; only even values are consumed.
 * @return 0 on success, -1 if no controller is present.
 */
int ac97_write(const void* data, uint32_t bytes);

/** @brief Bytes queued but not yet handed to the bus master. */
uint32_t ac97_buffered(void);

/**
 * @brief Advance the descriptor list.  Call from the timer tick.
 *
 * Cheap no-op when no controller is present.
 */
void ac97_tick(void);

/**
 * @brief Play a sine tone — proves the whole path end to end.
 *
 * @param freq_hz Tone frequency.
 * @param ms      Duration in milliseconds.
 * @return 0 on success, -1 if no controller is present.
 */
int ac97_test_tone(uint32_t freq_hz, uint32_t ms);

#endif /* AC97_H */
