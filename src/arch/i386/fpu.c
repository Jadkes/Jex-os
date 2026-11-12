/*
 * fpu.c - per-task x87/SSE state save and restore
 *
 * Purpose: give each task its own floating-point state.  Without this the
 * x87 unit and MXCSR are shared hardware: two tasks using float interleave
 * their registers and read each other's numbers, with no fault to notice.
 *
 * Design: eager FXSAVE/FXRSTOR at switch time (FNSAVE/FRSTOR on CPUs
 * without CPUID.FXSR).  The alternative, lazy switching via CR0.TS and a
 * #NM handler, buys little here -- one 512-byte copy per switch at 100 Hz
 * -- and costs a trap path that has to answer what a kernel task does when
 * it faults on its own FPU inside an ISR.  See fpu.h for the full rationale.
 *
 * Thread-safety: save/restore are called from task_switch() with interrupts
 * disabled, so no other context can touch the unit between the two calls.
 * Allocation and free happen in task-create/reap paths on one CPU.
 */

#define pr_fmt(fmt) "[FPU] " fmt
#include "kernel/printk.h"
#include "fpu.h"
#include "task.h"
#include "kheap.h"
#include "init.h"

/* CPUID leaf 1, EDX */
#define CPUID_EDX_FPU   (1u << 0)
#define CPUID_EDX_FXSR  (1u << 24)
#define CPUID_EDX_SSE   (1u << 25)

/* CR0 */
#define CR0_MP          (1u << 1)   /* WAIT/FWAIT honour CR0.TS */
#define CR0_EM          (1u << 2)   /* no FPU: x87 instructions raise #NM */
#define CR0_TS          (1u << 3)   /* task switched: first x87 use traps */
#define CR0_NE          (1u << 5)   /* native x87 errors raise #MF, not IRQ13 */

/* CR4 */
#define CR4_OSFXSR      (1u << 9)   /* FXSAVE/FXRSTOR and SSE state */
#define CR4_OSXMMEXCPT  (1u << 10)  /* unmasked SSE exceptions raise #XF */

#define FXSAVE_BYTES    512
#define FNSAVE_BYTES    108

static int      have_fxsr;          /* CPUID.FXSR: FXSAVE/FXRSTOR usable */
static int      have_sse;           /* CPUID.SSE: MXCSR holds real state */
static uint32_t fpu_state_bytes;    /* bytes meaningful in a saved image */

/* Pristine image captured after fninit: every new task starts here, so a
 * task never inherits whatever happened to be in the unit when it was
 * created.  16-byte aligned because fxsave requires it. */
static uint8_t fpu_template[FXSAVE_BYTES] __attribute__((aligned(16)));

static void cpuid_leaf(uint32_t leaf, uint32_t* eax, uint32_t* ebx,
                       uint32_t* ecx, uint32_t* edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf));
}

/*
 * fpu_area - 16-byte-aligned view of a task's image.
 *
 * FXSAVE faults on a misaligned buffer and kmalloc makes no alignment
 * promise, so the raw pointer is stored and aligned on use rather than
 * adding an aligned allocator for one caller.
 */
static uint8_t* fpu_area(const struct task* t)
{
    uintptr_t base = (uintptr_t)t->fpu_state;
    return (uint8_t*)((base + 15u) & ~(uintptr_t)15u);
}

static void fxsave_to(void* p)
{
    __asm__ volatile("fxsave (%0)" : : "r"(p) : "memory");
}

static void fxrstor_from(const void* p)
{
    __asm__ volatile("fxrstor (%0)" : : "r"(p) : "memory");
}

static void fnsave_to(void* p)
{
    __asm__ volatile("fnsave (%0)" : : "r"(p) : "memory");
}

static void frstor_from(const void* p)
{
    __asm__ volatile("frstor (%0)" : : "r"(p) : "memory");
}

static void save_image(void* p)
{
    if (have_fxsr)
        fxsave_to(p);
    else
        fnsave_to(p);
}

static void restore_image(const void* p)
{
    if (have_fxsr)
        fxrstor_from(p);
    else
        frstor_from(p);
}

/*
 * Enable the FPU on the calling CPU.  CR0 and CR4 are per-CPU registers,
 * so every core runs this before its first task switch.
 */
static void cpu_enable_fpu(void)
{
    uint32_t cr0;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    if (have_fxsr) {
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= CR4_OSFXSR;
        if (have_sse)
            cr4 |= CR4_OSXMMEXCPT;
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    }
}

void fpu_init_bsp(void)
{
    uint32_t eax, ebx, ecx, edx;

    cpuid_leaf(1, &eax, &ebx, &ecx, &edx);

    if (!(edx & CPUID_EDX_FPU)) {
        /* No unit to save.  Leaving the size at zero makes every
         * task_fpu_init() a no-op instead of failing every fork. */
        pr_warning("no FPU reported by CPUID; float unsupported\n");
        return;
    }

    have_fxsr = (edx & CPUID_EDX_FXSR) != 0;
    have_sse  = (edx & CPUID_EDX_SSE) != 0;
    fpu_state_bytes = have_fxsr ? FXSAVE_BYTES : FNSAVE_BYTES;

    cpu_enable_fpu();

    __asm__ volatile("fninit" ::: "memory");
    save_image(fpu_template);

    /* fnsave leaves the unit initialised, so the snapshot above is both
     * the template and the running state. */
    pr_info("%s per-task state, %u bytes/task%s\n",
            have_fxsr ? "FXSAVE" : "FNSAVE", fpu_state_bytes,
            have_sse ? ", SSE enabled" : "");
}

int fpu_available(void)
{
    return fpu_state_bytes != 0;
}

void fpu_init_cpu(void)
{
    if (!fpu_state_bytes)
        return;

    cpu_enable_fpu();
    __asm__ volatile("fninit" ::: "memory");
}

int task_fpu_init(struct task* t)
{
    t->fpu_state = NULL;

    if (!fpu_state_bytes)
        return 0;

    uint8_t* base = (uint8_t*)kmalloc(FPU_STATE_SLOT);
    if (!base)
        return -1;

    t->fpu_state = base;
    memcpy(fpu_area(t), fpu_template, fpu_state_bytes);
    return 0;
}

void task_fpu_free(struct task* t)
{
    if (!t)
        return;
    kfree(t->fpu_state);
    t->fpu_state = NULL;
}

void fpu_save(struct task* t)
{
    if (!t || !t->fpu_state)
        return;
    save_image(fpu_area(t));
}

void fpu_restore(struct task* t)
{
    if (!t || !t->fpu_state)
        return;
    restore_image(fpu_area(t));
}

early_init(fpu_init_bsp);
