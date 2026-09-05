#pragma once
#include <stdint.h>

/*
 * msr.h — model-specific register access (SCRUM-32).
 *
 * The MSR counterpart of io.h's port I/O.  `rdmsr`/`wrmsr` are ring-0 only;
 * executing either from ring 3 raises #GP, which is the property SCRUM-56
 * will test rather than something callers here need to guard against.
 *
 * Both instructions split the 64-bit value across EDX:EAX and take the
 * register number in ECX, so the wrappers do the halving that every caller
 * would otherwise repeat.
 */

/* MSR numbers used by the syscall entry path (Intel SDM Vol. 4 §2.1). */
#define MSR_IA32_EFER   0xC0000080u  /* extended features; bit 0 = SCE     */
#define MSR_IA32_STAR   0xC0000081u  /* syscall/sysret segment selectors   */
#define MSR_IA32_LSTAR  0xC0000082u  /* 64-bit syscall entry RIP           */
#define MSR_IA32_FMASK  0xC0000084u  /* RFLAGS bits cleared on syscall     */

/* IA32_EFER bits.  LME is already set by src/boot.s before it enables
 * paging — anything writing EFER must read-modify-write to preserve it. */
#define EFER_SCE  (1ull << 0)   /* System Call Extensions: enables syscall */
#define EFER_LME  (1ull << 8)   /* Long Mode Enable                        */
#define EFER_LMA  (1ull << 10)  /* Long Mode Active (read-only status)     */

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile ("wrmsr"
                      :
                      : "c"(msr),
                        "a"((uint32_t)(value & 0xFFFFFFFFu)),
                        "d"((uint32_t)(value >> 32)));
}
