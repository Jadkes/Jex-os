/**
 * @file ac97.c
 * @brief Intel AC'97 (82801AA) PCM audio output driver.
 *
 * Purpose: play real 16-bit stereo PCM.  The PC speaker can only toggle a
 *          square wave, so sample playback (and therefore MP3) needs a
 *          codec with a DAC — this is that path.
 *
 * Design:
 *   - Two I/O BARs: BAR0 is the mixer (NAM), BAR1 the bus master (NABM).
 *     Registers are addressed by their AC'97 codec offset directly; QEMU
 *     models a direct-offset map, not the real chip's index/data pair.
 *   - The bus master walks a Buffer Descriptor List of 32 fixed buffers.
 *     CIV and LVI are 5-bit indices that wrap at 32, so the list must have
 *     32 entries: an 8-entry list makes the engine fetch descriptors 8-31
 *     out of bounds once it passes the first cycle.  Every descriptor we
 *     publish is valid, padded with silence when the producer is dry,
 *     because the engine halts at LVI and a halted list has to be fully
 *     reprogrammed.
 *   - No interrupt: QEMU assigns AC'97 the same PCI IRQ line as the
 *     RTL8139, and irq.c keeps one handler per line, so claiming it would
 *     steal the NIC's.  Completion is detected by watching CIV from the
 *     PIT tick instead — 100 Hz against the ~23 ms a 4 KB buffer takes to
 *     play, so a descriptor refill has more than a full cycle of slack.
 *   - Halting is the expensive case.  With mixing off the backend fills
 *     its whole period with silence as soon as the list stops, so the
 *     engine is kept fed ahead of the producer rather than restarted on
 *     every underrun: only a truly empty ring leaves it halted.
 *
 * Thread-safety:
 *   - ac97_write()  — task context, blocks on a full ring.
 *   - ac97_tick()   — IRQ context, same lock taken with IRQs off, so it
 *                     cannot deadlock against a preempted writer.
 */

#define pr_fmt(fmt) "[AC97] " fmt

#include "ac97.h"
#include "pci.h"
#include "ports.h"
#include "kheap.h"
#include "spinlock.h"
#include "timer.h"
#include "init.h"
#include "kernel/printk.h"

/* ---- Mixer (NAM, BAR0) register offsets ---- */
#define NAM_MASTER_VOLUME    0x02
#define NAM_PCM_OUT_VOLUME   0x18
#define NAM_POWERDOWN        0x26
#define NAM_EXT_AUDIO_CTRL   0x2A
#define NAM_PCM_DAC_RATE     0x2C
#define NAM_VENDOR_ID1       0x7C
#define NAM_VENDOR_ID2       0x7E

#define EACS_VRA             0x0001  /* variable rate audio */

/* ---- Bus master (NABM, BAR1) register offsets ---- */
#define NABM_PO_BDBAR        0x10
#define NABM_PO_CIV          0x14
#define NABM_PO_LVI          0x15
#define NABM_PO_SR           0x16
#define NABM_PO_CR           0x1B
#define NABM_GLOB_STA        0x30

#define SR_FIFOE             0x10
#define SR_BCIS              0x08
#define SR_LVBCI             0x04
#define SR_DCH               0x01

#define CR_RR                0x02
#define CR_RPBM              0x01

#define GS_S0CR              0x0100  /* codec 0 ready */

/* ---- Buffer geometry ----
 * 32 x 4 KB = 128 KB = 743 ms of stereo audio at 44.1 kHz, and the CIV/LVI
 * registers wrap at 32 entries, so the list length is not a free choice.
 * A 10 ms tick gives each descriptor ~23 ms of slack — well over a full
 * cycle before the engine could catch up with the refill. */
#define BD_COUNT             32
#define BD_SAMPLES           2048          /* 16-bit words: 4096 B = 1024 frames */
#define BD_BYTES             (BD_SAMPLES * 2)
#define BD_IOC               (1u << 31)
#define AUDIO_RING_BYTES     65536
#define COPY_SLICE_BYTES     2048

/* Largest duration whose rate * ms product still fits 32 bits — the frame
 * count is derived from that product, so overflow there is what bites. */
#define TONE_MAX_MS          (0xFFFFFFFFu / AC97_RATE_HZ)

typedef struct {
    uint32_t addr;   /* physical (== virtual, kernel is identity mapped) */
    uint32_t len;    /* bit31 IOC, bits15:0 sample count */
} ac97_bd_t;

static uint16_t nam_base;
static uint16_t nabm_base;

static ac97_bd_t* bdl;
static uint8_t*   bd_buffers;         /* BD_COUNT * BD_BYTES, contiguous */
static uint8_t*   ring;               /* producer ring */

static uint32_t ring_head;
static uint32_t ring_tail;
static uint32_t ring_count;

static uint32_t last_civ;             /* last CIV read from the engine */
static uint32_t fill_idx;             /* next descriptor to (re)write */
static uint32_t free_desc;            /* descriptors reclaimed, not yet refilled */

static int        ac97_ready;
static spinlock_t audio_lock;

/* 256-entry sine, amplitude 8000 (~-12 dBFS).  A table keeps tone
 * generation free of both floating point and per-sample division. */
static const int16_t sine_table[256] = {
          0,    196,    393,    589,    784,    979,   1174,   1368,   1561,   1753,   1944,   2134,
       2322,   2509,   2695,   2879,   3061,   3242,   3420,   3597,   3771,   3943,   4113,   4280,
       4445,   4606,   4766,   4922,   5075,   5225,   5372,   5516,   5657,   5794,   5928,   6058,
       6184,   6307,   6426,   6541,   6652,   6759,   6862,   6961,   7055,   7146,   7232,   7314,
       7391,   7464,   7532,   7596,   7656,   7710,   7760,   7806,   7846,   7882,   7913,   7940,
       7961,   7978,   7990,   7998,   8000,   7998,   7990,   7978,   7961,   7940,   7913,   7882,
       7846,   7806,   7760,   7710,   7656,   7596,   7532,   7464,   7391,   7314,   7232,   7146,
       7055,   6961,   6862,   6759,   6652,   6541,   6426,   6307,   6184,   6058,   5928,   5794,
       5657,   5516,   5372,   5225,   5075,   4922,   4766,   4606,   4445,   4280,   4113,   3943,
       3771,   3597,   3420,   3242,   3061,   2879,   2695,   2509,   2322,   2134,   1944,   1753,
       1561,   1368,   1174,    979,    784,    589,    393,    196,      0,   -196,   -393,   -589,
       -784,   -979,  -1174,  -1368,  -1561,  -1753,  -1944,  -2134,  -2322,  -2509,  -2695,  -2879,
      -3061,  -3242,  -3420,  -3597,  -3771,  -3943,  -4113,  -4280,  -4445,  -4606,  -4766,  -4922,
      -5075,  -5225,  -5372,  -5516,  -5657,  -5794,  -5928,  -6058,  -6184,  -6307,  -6426,  -6541,
      -6652,  -6759,  -6862,  -6961,  -7055,  -7146,  -7232,  -7314,  -7391,  -7464,  -7532,  -7596,
      -7656,  -7710,  -7760,  -7806,  -7846,  -7882,  -7913,  -7940,  -7961,  -7978,  -7990,  -7998,
      -8000,  -7998,  -7990,  -7978,  -7961,  -7940,  -7913,  -7882,  -7846,  -7806,  -7760,  -7710,
      -7656,  -7596,  -7532,  -7464,  -7391,  -7314,  -7232,  -7146,  -7055,  -6961,  -6862,  -6759,
      -6652,  -6541,  -6426,  -6307,  -6184,  -6058,  -5928,  -5794,  -5657,  -5516,  -5372,  -5225,
      -5075,  -4922,  -4766,  -4606,  -4445,  -4280,  -4113,  -3943,  -3771,  -3597,  -3420,  -3242,
      -3061,  -2879,  -2695,  -2509,  -2322,  -2134,  -1944,  -1753,  -1561,  -1368,  -1174,   -979,
       -784,   -589,   -393,   -196,
};

static inline uint16_t nam_read(uint16_t reg)
{
    return inw((uint16_t)(nam_base + reg));
}

static inline void nam_write(uint16_t reg, uint16_t value)
{
    outw((uint16_t)(nam_base + reg), value);
}

static inline uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/* ports.h has no "memory" clobber on the out* helpers, so plain stores to
 * the descriptor list can be sunk past the register write that publishes
 * it.  This barrier makes the order explicit: list first, then start. */
static inline void dma_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* Pull bytes out of the producer ring.  Returns the count actually taken;
 * the caller zero-fills the remainder so a dry source plays silence rather
 * than replaying whatever the descriptor held before. */
static uint32_t ring_take(uint8_t* dst, uint32_t want)
{
    uint32_t taken = 0;

    while (taken < want && ring_count > 0) {
        uint32_t chunk = AUDIO_RING_BYTES - ring_tail;
        uint32_t room = want - taken;

        if (chunk > ring_count) chunk = ring_count;
        if (chunk > room)       chunk = room;

        memcpy(dst + taken, ring + ring_tail, chunk);
        ring_tail = (ring_tail + chunk) % AUDIO_RING_BYTES;
        ring_count -= chunk;
        taken += chunk;
    }
    return taken;
}

/*
 * Fill the next descriptor from the ring.  With @p pad clear an empty ring
 * leaves the slot untouched and returns 0, so engine_reload() can stop at
 * the last slot that really has audio instead of queueing silence ahead of
 * a producer that is still decoding.
 */
static uint32_t descriptor_fill(int pad)
{
    uint32_t index = fill_idx;
    uint8_t* buffer = bd_buffers + index * BD_BYTES;
    uint32_t taken = ring_take(buffer, BD_BYTES);

    if (taken < BD_BYTES) {
        if (!pad && taken == 0)
            return 0;
        memset(buffer + taken, 0, BD_BYTES - taken);
    }

    bdl[index].addr = (uint32_t)buffer;
    bdl[index].len  = BD_IOC | BD_SAMPLES;

    fill_idx = (index + 1) % BD_COUNT;
    return taken;
}

/*
 * Reprogram the list from slot 0 and start the engine.  CR_RR puts civ,
 * piv and lvi in a known state instead of inheriting stale pointers from
 * before the halt.  Returns 0 when the ring is empty and the engine is
 * left halted; the tick restarts it on the next write.
 */
static int engine_reload(void)
{
    fill_idx = 0;

    uint32_t filled = 0;
    while (filled < BD_COUNT && descriptor_fill(0) > 0)
        filled++;

    last_civ = 0;
    free_desc = 0;

    if (!filled)
        return 0;

    outb((uint16_t)(nabm_base + NABM_PO_CR), CR_RR);
    dma_barrier();
    outl((uint16_t)(nabm_base + NABM_PO_BDBAR), (uint32_t)bdl & ~3u);
    outb((uint16_t)(nabm_base + NABM_PO_LVI), (uint8_t)(filled - 1));
    outb((uint16_t)(nabm_base + NABM_PO_CR), CR_RPBM);
    return 1;
}

static void pump_locked(void)
{
    if (!ac97_ready)
        return;

    if (inw((uint16_t)(nabm_base + NABM_PO_SR)) & SR_DCH) {
        /* The engine caught up with LVI.  With mixing off the backend
         * fills the rest of its period with silence the moment the list
         * stops, so do not restart until there is audio to play. */
        if (!ring_count)
            return;
        outw((uint16_t)(nabm_base + NABM_PO_SR), SR_FIFOE | SR_BCIS | SR_LVBCI);
        engine_reload();
        return;
    }

    uint32_t civ = inb((uint16_t)(nabm_base + NABM_PO_CIV));
    uint32_t advanced = (civ + BD_COUNT - last_civ) % BD_COUNT;

    if (advanced) {
        last_civ = civ;
        free_desc += advanced;
        if (free_desc > BD_COUNT)
            free_desc = BD_COUNT;
    }

    if (!free_desc)
        return;

    while (free_desc) {
        descriptor_fill(1);
        free_desc--;
    }

    outb((uint16_t)(nabm_base + NABM_PO_LVI),
         (uint8_t)((fill_idx + BD_COUNT - 1) % BD_COUNT));
}

int ac97_present(void)
{
    return ac97_ready;
}

uint32_t ac97_rate(void)
{
    return ac97_ready ? (uint32_t)nam_read(NAM_PCM_DAC_RATE) : 0;
}

uint32_t ac97_buffered(void)
{
    uint32_t flags;
    uint32_t count;

    spin_lock_irqsave(&audio_lock, &flags);
    count = ring_count;
    spin_unlock_irqrestore(&audio_lock, flags);
    return count;
}

void ac97_tick(void)
{
    uint32_t flags;

    if (!ac97_ready)
        return;

    spin_lock_irqsave(&audio_lock, &flags);
    pump_locked();
    spin_unlock_irqrestore(&audio_lock, flags);
}

int ac97_write(const void* data, uint32_t bytes)
{
    const uint8_t* src = (const uint8_t*)data;

    if (!ac97_ready)
        return -1;

    /* The codec consumes whole 16-bit frames; a dangling byte cannot be
     * expressed in the descriptor length field. */
    bytes &= ~1u;

    while (bytes) {
        uint32_t flags;
        uint32_t chunk;

        /* Space is measured and consumed under one lock: splitting the two
         * lets a second writer fill the gap and push ring_count past the
         * buffer size. */
        spin_lock_irqsave(&audio_lock, &flags);
        uint32_t space = AUDIO_RING_BYTES - ring_count;

        chunk = bytes < space ? bytes : space;
        if (chunk > COPY_SLICE_BYTES) chunk = COPY_SLICE_BYTES;
        chunk &= ~1u;

        if (!chunk) {
            spin_unlock_irqrestore(&audio_lock, flags);
            /* Real-time pacing: the engine drains at the sample rate, so
             * a short sleep here is exactly the backpressure we want. */
            sleep(1);
            continue;
        }

        uint32_t head_room = AUDIO_RING_BYTES - ring_head;
        if (head_room >= chunk) {
            memcpy(ring + ring_head, src, chunk);
        } else {
            memcpy(ring + ring_head, src, head_room);
            memcpy(ring, src + head_room, chunk - head_room);
        }
        ring_head = (ring_head + chunk) % AUDIO_RING_BYTES;
        ring_count += chunk;
        spin_unlock_irqrestore(&audio_lock, flags);

        src += chunk;
        bytes -= chunk;
    }
    return 0;
}

int ac97_test_tone(uint32_t freq_hz, uint32_t ms)
{
    int16_t block[256 * 2];
    uint32_t frames;
    uint32_t phase = 0;
    uint32_t step;

    if (!ac97_ready || !freq_hz || !ms)
        return -1;
    if (ms > TONE_MAX_MS)
        ms = TONE_MAX_MS;

    /* Above Nyquist a table walk aliases into an unrelated pitch, and the
     * phase step would overflow its 16-bit shift for very large inputs. */
    if (freq_hz > AC97_RATE_HZ / 2)
        freq_hz = AC97_RATE_HZ / 2;

    frames = (AC97_RATE_HZ * ms) / 1000;
    /* 16-bit phase accumulator: one cycle is 0x10000, so the increment is
     * the frequency as a fraction of the sample rate.  No per-sample
     * division and no floating point. */
    step = (freq_hz << 16) / AC97_RATE_HZ;

    while (frames) {
        uint32_t count = frames < 256 ? frames : 256;

        for (uint32_t i = 0; i < count; i++) {
            int16_t sample = sine_table[(phase >> 8) & 0xFF];
            block[i * 2 + 0] = sample;
            block[i * 2 + 1] = sample;
            phase += step;
        }
        if (ac97_write(block, count * 4) != 0)
            return -1;
        frames -= count;
    }
    return 0;
}

static int ac97_probe(pci_device_t* dev)
{
    uint32_t command;
    int ready;

    /* Both BARs must decode I/O space.  Masking an MMIO address down to
     * 16 bits would leave the driver writing into unrelated port numbers. */
    if (!(dev->bar0 & 1u) || !(dev->bar1 & 1u)) {
        pr_err("BARs are not I/O space (bar0=0x%x bar1=0x%x)\n",
               dev->bar0, dev->bar1);
        return -1;
    }

    nam_base  = (uint16_t)(dev->bar0 & ~0x3u);
    nabm_base = (uint16_t)(dev->bar1 & ~0x3u);

    if (!nam_base || !nabm_base) {
        pr_err("BAR not assigned (nam=0x%x nabm=0x%x)\n",
               nam_base, nabm_base);
        return -1;
    }

    /* I/O space plus bus mastering: the codec fetches the descriptor list
     * and sample buffers over PCI DMA. */
    command = pci_config_read_dword(dev->bus, dev->device, dev->function, 0x04);
    command |= (1u << 0) | (1u << 2);
    pci_config_write_dword(dev->bus, dev->device, dev->function, 0x04, command);

    /* Codec ready is bit 8 of the global status.  Real codecs can take a
     * while after link reset, so bound the wait instead of hanging on a
     * device that never answers. */
    ready = 0;
    for (int i = 0; i < 100000; i++) {
        if (inl((uint16_t)(nabm_base + NABM_GLOB_STA)) & GS_S0CR) {
            ready = 1;
            break;
        }
    }
    if (!ready) {
        pr_err("codec never reported ready\n");
        return -1;
    }

    bdl        = (ac97_bd_t*)kmalloc(sizeof(ac97_bd_t) * BD_COUNT + 16);
    bd_buffers = (uint8_t*)kmalloc(BD_COUNT * BD_BYTES);
    ring       = (uint8_t*)kmalloc(AUDIO_RING_BYTES);
    if (!bdl || !bd_buffers || !ring) {
        pr_err("out of memory\n");
        kfree(bdl);
        kfree(bd_buffers);
        kfree(ring);
        return -1;
    }

    /* BDBAR is masked to 4 bytes by the hardware; align further so nobody
     * later assumes that mask is sufficient.  Align before zeroing so the
     * cleared range covers exactly the memory the engine will read. */
    bdl = (ac97_bd_t*)align_up((uint32_t)bdl, 16);
    memset(bdl, 0, sizeof(ac97_bd_t) * BD_COUNT);
    memset(bd_buffers, 0, BD_COUNT * BD_BYTES);
    memset(ring, 0, AUDIO_RING_BYTES);

    ring_head = 0;
    ring_tail = 0;
    ring_count = 0;
    last_civ = 0;
    fill_idx = 0;
    free_desc = 0;
    spin_init(&audio_lock);

    /* QEMU resets the mixer with master and PCM-out muted, which is the
     * classic "driver runs but is silent" trap.  0x0000 is 0 dB. */
    nam_write(NAM_MASTER_VOLUME, 0x0000);
    nam_write(NAM_PCM_OUT_VOLUME, 0x0000);
    nam_write(NAM_POWERDOWN, 0x0000);

    /* Variable-rate audio must be enabled before the rate register
     * accepts anything other than the 48 kHz default. */
    nam_write(NAM_EXT_AUDIO_CTRL,
              (uint16_t)(nam_read(NAM_EXT_AUDIO_CTRL) | EACS_VRA));
    nam_write(NAM_PCM_DAC_RATE, AC97_RATE_HZ);

    ac97_ready = 1;
    engine_reload();

    pr_info("8086:2415 io=0x%x/0x%x codec=%04x:%04x %u Hz\n",
            nam_base, nabm_base,
            nam_read(NAM_VENDOR_ID1), nam_read(NAM_VENDOR_ID2),
            (uint32_t)nam_read(NAM_PCM_DAC_RATE));
    return 0;
}

static const struct pci_device_id ac97_ids[] = {
    { AC97_PCI_VENDOR, AC97_PCI_DEVICE, 0 },
    { 0, 0, 0 },
};

static struct pci_driver ac97_driver = {
    .name     = "ac97",
    .id_table = ac97_ids,
    .probe    = ac97_probe,
    .remove   = 0,
    .next     = 0,
};

void ac97_init(void)
{
    /* Registration probes config space directly, so this does not depend
     * on init_pci() having run first — initcall order is link order, and
     * ac97.c can land either side of pci.c. */
    pci_register_driver(&ac97_driver);
}

device_init(ac97_init);
