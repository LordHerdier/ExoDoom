# ExoDoom Syscall Interface Specification

Derived from static analysis of
[doomgeneric](https://github.com/ozkl/doomgeneric) source code.

**Document version:** 1.2 — Updated 20 Apr 2026 to reflect x86_64 migration

---

## Table of Contents

1. [The doomgeneric platform interface](#1-the-doomgeneric-platform-interface)
2. [Complete libc dependency audit](#2-complete-libc-dependency-audit)
   - [2.1 string.h](#21-stringh--all-required-no-shortcuts)
   - [2.2 stdlib.h](#22-stdlibh--malloc-is-critical-path)
   - [2.3 stdio.h](#23-stdioh--the-hardest-category)
   - [2.4 math.h](#24-mathh--almost-entirely-avoidable)
   - [2.5 ctype.h, stdarg.h, assert.h, errno.h, limits.h](#25-ctypeh-stdargh-asserth-errnoh-limitsh)
   - [2.6 POSIX / platform functions to stub](#26-posix--platform-functions-to-stub)
3. [Exokernel syscall specification](#3-exokernel-syscall-specification)
   - [3.1 Calling convention](#31-syscall-calling-convention)
   - [3.2 Syscall table](#32-syscall-table)
   - [3.2a Error codes](#32a-error-codes-scrum-57)
   - [3.3 Secure binding & resource ownership](#33-secure-binding--resource-ownership)
   - [3.4 Entry path](#34-entry-path-scrum-32)
   - [3.4a FPU/SSE state across kernel entry](#34a-fpusse-state-across-kernel-entry-scrum-177)
   - [3.5 Framebuffer binding](#35-framebuffer-binding-scrum-154)
   - [3.5a Disk binding](#35a-disk-binding-scrum-188)
   - [3.6 Revocation & repossession](#36-revocation--repossession-scrum-156)
   - [3.7 Address-space mapping](#37-address-space-mapping-scrum-35)
4. [Architectural decision: file I/O strategy](#4-architectural-decision-file-io-strategy)
5. [Memory allocation pattern](#5-memory-allocation-pattern)
6. [Sound architecture](#6-sound-architecture)

---

doomgeneric requires exactly **6 functions** to be implemented by the platform.
These are the **only** entry points between the Doom engine and the host
environment. Everything else (rendering, game logic, AI, physics) is handled
internally by the engine.

| Function            | Signature                                         | What it does                                                                                                           |
| ------------------- | ------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `DG_Init`           | `void DG_Init(void)`                              | Called once after `DG_ScreenBuffer` is allocated (640×400×4 bytes). Initialize framebuffer mapping and input.          |
| `DG_DrawFrame`      | `void DG_DrawFrame(void)`                         | Called by `I_FinishUpdate` after Doom writes a frame to `DG_ScreenBuffer`. Blit that buffer to the actual framebuffer. |
| `DG_SleepMs`        | `void DG_SleepMs(uint32_t ms)`                    | Sleep for `ms` milliseconds. Busy-wait on PIT tick counter is fine.                                                    |
| `DG_GetTicksMs`     | `uint32_t DG_GetTicksMs(void)`                    | Return monotonic milliseconds since boot. Used for game timing (35 tics/sec) and frame pacing.                         |
| `DG_GetKey`         | `int DG_GetKey(int* pressed, unsigned char* key)` | Dequeue next key event. Returns 1 if event available (sets `pressed=0/1` and `key=doom keycode`), 0 if queue empty.    |
| `DG_SetWindowTitle` | `void DG_SetWindowTitle(const char* title)`       | No-op on bare metal. Can print to serial for debug.                                                                    |

> **Important:** `DG_ScreenBuffer` is a 640×400 pixel RGBA8888 buffer
> (`pixel_t = uint32_t`). Doom's internal rendering is 8-bit paletted;
> `I_FinishUpdate` in `i_video.c` converts palette indices to RGBA and writes to
> `DG_ScreenBuffer` before calling `DG_DrawFrame`. Your `DG_DrawFrame` only
> needs to copy/scale this buffer to the hardware framebuffer.

> **Mouse input** is NOT part of the `DG_*` interface. The mouse code in
> `i_input.c` is commented out (it references SDL events). To add mouse support,
> patch `I_GetEvent()` to post `ev_mouse` events with `data2=dx`, `data3=-dy` by
> polling the mouse driver via syscall.

---

## 2. Complete libc dependency audit

> ⚠️ **Superseded by [`docs/libc_audit.md`](libc_audit.md) (SCRUM-72).** This
> section is kept for its per-family design notes, but **its status column and
> call counts are no longer authoritative.** They came from static grep over
> "doomgeneric's 82 core source files", which counts text rather than reachable
> calls and covers a file set wider than what `src/doom/` actually vendors (79
> `.c` files). The SCRUM-72 audit instead derives the list from the linker —
> `nm -u` over the compiled objects minus what Doom defines itself — which
> cannot count a call inside `#if ORIGCODE`, miss one made through a macro, or
> include a file that was never vendored.
>
> Concrete corrections that follow from that:
>
> - **`strerror` is listed below with 4 calls. It has zero.** Nothing under
>   `src/doom/` calls it, and no object references it. It does not need
>   implementing.
> - **`memcmp` and `memmove` are not referenced either** — both are implemented
>   and correct, but Doom never reaches them (`memmove`'s two call sites are in
>   a dehacked branch GCC proves dead, since `DEH_String(x)` is `#define`d to
>   `(x)`).
> - **`strcasecmp`/`strncasecmp` are marked Todo below; both have been
>   implemented** in `src/string.c` since SCRUM-11's follow-up.
> - Call counts differ throughout — e.g. `strlen` is 56 occurrences in the
>   vendored tree, not 64; `strdup` is 9, not 16.
>
> Use `docs/libc_audit.md` and its `libc_audit.csv` for status. Use this
> section for the "why" notes per family.

The following is a list of standard C library functions called by
doomgeneric, with call counts from static grep analysis. This was the original
basis for what the freestanding libc shim must provide.

### 2.1 `string.h` — all required, no shortcuts

> ✅ **Sprint 1 (SCRUM-11):** `memcpy`, `memset`, `memmove`, `strcmp`, `strlen`,
> `strncpy`, `strcat`, `strchr`, `memcmp` are implemented in `src/string.c` and
> merged. Remaining functions (`strdup`, `strcasecmp`, `strncasecmp`, `strncmp`,
> `strrchr`, `strerror`, `strstr`) are needed for doomgeneric but not yet
> implemented.

| Function      | Calls | Status  | Notes                                                                                                            |
| ------------- | ----- | ------- | ---------------------------------------------------------------------------------------------------------------- |
| `strlen`      | 64    | ✅ Done | Used everywhere.                                                                                                 |
| `memset`      | 50    | ✅ Done | Used for clearing buffers, zero-init.                                                                            |
| `memcpy`      | 45    | ✅ Done | Used for framebuffer blitting, WAD data copying. Performance-sensitive — consider 32-bit aligned implementation. |
| `strdup`      | 16    | ⬜ Todo | Allocates + copies. Depends on `malloc` + `strlen` + `memcpy`.                                                   |
| `strcmp`      | 16    | ✅ Done | Standard string compare.                                                                                         |
| `strcasecmp`  | 9     | ⬜ Todo | Case-insensitive compare. **NOT in C standard (POSIX).** Used heavily in WAD/config parsing.                     |
| `memcmp`      | 8     | ✅ Done | Byte comparison.                                                                                                 |
| `strncasecmp` | 7     | ⬜ Todo | Case-insensitive compare with length limit. Also POSIX, not C standard.                                          |
| `strchr`      | 7     | ✅ Done | Find character in string.                                                                                        |
| `strncmp`     | 6     | ⬜ Todo | Compare with length limit.                                                                                       |
| `strrchr`     | 5     | ⬜ Todo | Find last occurrence of character.                                                                               |
| `strerror`    | 4     | ⬜ Todo | Returns string for errno value. Can return a static `"unknown error"` string.                                    |
| `strstr`      | 3     | ⬜ Todo | Find substring. Used in config parsing.                                                                          |
| `strncpy`     | 3     | ✅ Done | Copy with length limit.                                                                                          |
| `memmove`     | 3     | ✅ Done | Overlapping copy.                                                                                                |

### 2.2 `stdlib.h` — malloc is critical path

> **Key insight:** Doom calls `malloc` exactly ONCE in `I_ZoneBase()` to
> allocate a 6 MiB zone, then manages memory internally via `Z_Malloc`. However,
> there are ~21 additional direct `malloc` calls scattered across WAD discovery,
> config parsing, string handling, and the screen buffer allocation. The libc
> `malloc` must work but does not need to be high-performance.

> ✅ **Sprint 3 (SCRUM-30):** `malloc`, `free`, `realloc`, `atoi`, `abs`,
> `rand`/`srand` and `qsort` are implemented in `src/stdlib.c` and merged.
> `malloc`/`free`/`realloc` allocate no pool of their own — which allocator
> they forward to depends on which side of the kernel/LibOS build split
> compiles them (SCRUM-51): `kmalloc`/`kfree`/`krealloc` (`src/memory.c` →
> `src/heap.c`, SCRUM-25) under `-DEXO_KERNEL` (the kernel itself, and every
> `tests/kernel/*.c` suite), `libos_heap_alloc`/`_free`/`_realloc`
> (`src/libos_heap.c`, SCRUM-38) otherwise — the ring-3 LibOS link target
> `tests/kernel/libc_shim_probe/` builds. Remaining: `exit`, `abort`,
> `atexit`, `getenv`, `system`, `calloc`, `atof`.

| Function  | Calls    | Status  | Notes                                                                                         |
| --------- | -------- | ------- | --------------------------------------------------------------------------------------------- |
| `free`    | 153      | ✅ Done | Most calls are `Z_Free` (internal zone). ~20 are direct libc `free()`.                        |
| `exit`    | 31       | ⬜ Todo | Called on fatal errors. Implement as halt loop. Pairs with `exo_exit` (#20).                  |
| `malloc`  | 21       | ✅ Done | One 6 MiB zone alloc + ~20 small allocs (strings, paths, structs).                            |
| `abs`     | 30       | ✅ Done | Integer absolute value. `abs(INT_MIN)` negates in unsigned arithmetic rather than being UB.   |
| `atoi`    | 14       | ✅ Done | String to integer. Used for config/command-line parsing. Saturates instead of wrapping.       |
| `atof`    | 2        | ⬜ Todo | String to float. Used only for mouse acceleration config. Can return `1.0` as stub.           |
| `atexit`  | 4        | ⬜ Todo | Register cleanup functions. Implement as linked list (Doom already does this via `I_AtExit`). |
| `getenv`  | 3        | ⬜ Todo | Returns `DOOMWADPATH`/`DOOMWADDIR`. Return `NULL` — WAD is a multiboot module.                |
| `system`  | 13       | ⬜ Todo | All behind `#ifdef` guards (Zenity error boxes). Stub as `return -1`.                         |
| `realloc` | 3        | ✅ Done | Resize allocation. `heap_realloc` grows in place when the next block is free.                 |
| `calloc`  | 2        | ⬜ Todo | `malloc` + `memset(0)`. Trivial wrapper.                                                      |
| `abort`   | 2        | ⬜ Todo | Abnormal termination. Implement as halt loop.                                                 |
| `qsort`   | 0 direct | ✅ Done | Not called directly but may be pulled in. Median-of-three quicksort, three-way partition.     |
| `rand`    | 0 direct | ✅ Done | Not in the vendored core either, but part of the same header. C-standard reference LCG.       |
| `srand`   | 0 direct | ✅ Done | Seeds `rand`; the sequence for a given seed is fixed and asserted in the test suite.          |

Call counts above are from the original 82-file doomgeneric analysis. For
reference, the same grep over the vendored subset in `src/doom/` (175 files)
finds `abs` 30, `malloc` 18, `free` 15, `atoi` 9, `realloc` 1, `calloc` 1, and
no direct `rand`/`srand`/`qsort` at all.

**Implementation notes (SCRUM-30).** Three choices in `src/stdlib.c` are worth
knowing before changing it:

- **`qsort` cannot copy its pivot.** Element size is a runtime value and there
  is no scratch buffer, so the pivot is compared in place: median-of-three
  parks it at `lo` and only `[lo+1, hi]` is partitioned, which means no swap
  can move it out from under the comparisons.
- **`qsort` recurses into the smaller partition only** and loops on the larger,
  bounding stack depth at O(log n). The kernel stack is 16 KiB; a quicksort
  recursing on both sides would overrun it on a sorted input long before
  finishing. Measured depth at n = 1,000,000 is 18 against a log₂ n of 20.
- **`qsort` partitions three ways, and that is not an optimisation**
  (SCRUM-171). The original two-way Hoare partition scanned with
  `cmp(i, lo) <= 0` / `cmp(j, lo) >= 0`, so both pointers ran over keys equal
  to the pivot instead of stopping on them; every partition of an input with
  few distinct values then split n-1/0, which is O(n²). The Bentley–McIlroy
  partition in place of it stops each scan on an equal key, parks it at the
  end that scan came from, and rotates both equal runs into the middle with
  two block swaps, where they are already in final position and are excluded
  from both recursions. Comparison counts at n = 200,000:

  | Input shape  | Two-way (before) | Three-way (after) | Change    |
  | ------------ | ---------------: | ----------------: | --------- |
  | sorted       |        3,167,247 |         3,167,247 | —         |
  | reverse      |        3,167,247 |         3,167,247 | —         |
  | random       |        4,020,100 |         3,211,413 | 1.25× fewer |
  | ten distinct |    1,704,561,308 |           680,505 | 2,505× fewer |
  | two distinct |   15,064,643,549 |           300,153 | 50,190× fewer |
  | all-equal    |   20,000,299,963 |           200,001 | 100,001× fewer |

  All-equal input is now a single O(n) partition pass; k distinct values cost
  O(n log k). `tests/kernel/test_stdlib_k.c` guards this with comparison-count
  budgets rather than wall-clock times, since the count is a property of the
  algorithm and is identical on the host and under QEMU.
- **`rand` is pinned to `uint32_t`.** The C standard's reference LCG is
  specified over a 32-bit accumulator; a 64-bit one silently produces a
  different sequence. Seed 1 must yield 16838, 5758, 10113, …

### 2.3 `stdio.h` — the hardest category

`stdio.h` is the most complex dependency. Doom uses `printf` extensively for
debug output, and uses real `FILE*` I/O for three distinct purposes: (1) reading
WAD files from disk, (2) reading/writing config files, and (3) reading/writing
save games. Since the WAD is a multiboot module (already in memory), **option
A** is to replace `w_file_stdc.c` with a memory-mapped reader. Config and save
files need either a ramdisk-backed or ATA-backed `FILE*` implementation.

| Function    | Calls | Notes                                                                                              |
| ----------- | ----- | -------------------------------------------------------------------------------------------------- |
| `printf`    | 143   | Redirect to serial output via `exo_serial_write` syscall. Most output is init/debug messages.      |
| `fprintf`   | 92    | Mostly `fprintf(stderr, ...)`. Redirect to serial. A few write to `FILE*` for config save.         |
| `fopen`     | 21    | Opens WAD, config, save files. Must return a `FILE*` struct backed by ramdisk or ATA.              |
| `fclose`    | 21    | Close file handle. Free `FILE*` struct.                                                            |
| `fwrite`    | 16    | Write config and save game data.                                                                   |
| `fread`     | 14    | Read WAD data and save games.                                                                      |
| `remove`    | 13    | Delete files (old save games). Implement for ramdisk; no-op is acceptable initially.               |
| `sscanf`    | 8     | Parse formatted strings. Used in config parsing (`%d`, `%f`, `%x` formats). Must implement.        |
| `puts`      | 6     | Print string + newline to stdout. Route to serial.                                                 |
| `ftell`     | 6     | Return current file position.                                                                      |
| `fflush`    | 6     | Flush buffered output. No-op for unbuffered serial. Needed for `FILE*` writes.                     |
| `fseek`     | 5     | Seek to position in file. Used by WAD reader (`SEEK_SET`, `SEEK_END`).                             |
| `vsnprintf` | 5     | Format string to buffer with length limit. Doom has its own `M_vsnprintf` wrapper that calls this. |
| `putchar`   | 3     | Write single char. Route to serial.                                                                |
| `rename`    | 3     | Rename save file. Implement for ramdisk or stub.                                                   |
| `feof`      | 2     | Check end-of-file. Return based on `FILE*` position.                                               |
| `vfprintf`  | 1     | Used in `I_Error`. Format + print to `stderr` (serial).                                            |
| `sprintf`   | 1     | Unsafe format to buffer. Wrapper around `vsnprintf`.                                               |
| `snprintf`  | 1     | Format to buffer with length. Core function; `vsnprintf` does the work.                            |
| `fgets`     | 1     | Read line from file. Used in config parsing.                                                       |
| `fileno`    | 1     | Behind `#ifdef ORIGCODE`. Stub as `return -1`.                                                     |

### 2.4 `math.h` — almost entirely avoidable

Doom uses fixed-point arithmetic internally. The 187 calls to `floor()` are
actually Doom's `FIXED_TO_INT` macro which is integer division, not
floating-point. The **only** real floating-point math is in `R_InitTables()` in
`r_main.c`, which runs ONCE at startup to build sine/tangent lookup tables from
3 trig calls.

Options:

- **(A)** Implement `sin`/`cos`/`tan`/`atan` with a small Taylor series or
  lookup table.
- **(B)** Precompute the tables and hardcode them.

| Function | Calls | Notes                                                                                                                                                        |
| -------- | ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `floor`  | 187   | **NOT floating-point.** Doom uses this for fixed-point→int conversion. Implement as `(int)(x)` for positive, `(int)(x)-1` for negative with fractional part. |
| `atan`   | 1     | Called once in `R_InitTables`. Builds `FINEANGLES` lookup table.                                                                                             |
| `tan`    | 1     | Called once in `R_InitTables`. Builds `finetangent` table.                                                                                                   |
| `sin`    | 1     | Called once in `R_InitTables`. Builds `finesine` table.                                                                                                      |
| `fabs`   | 1     | Floating-point absolute value. Used once in mouse acceleration check. Trivial.                                                                               |

### 2.5 `ctype.h`, `stdarg.h`, `assert.h`, `errno.h`, `limits.h`

**`ctype.h`:** `isspace` (12), `toupper` (11), `tolower` (7), `isprint` (1),
`isalpha` (1). All trivial ASCII range checks. ✅ **Fully implemented and merged
in Sprint 1 (SCRUM-12)** — `src/ctype.c`.

**`stdarg.h`:** `va_start` (5), `va_end` (5), `va_arg` (2). Provided by the
compiler (`__builtin_va_*`). No implementation needed.

**`assert.h`:** `assert` (6). Implement as macro that prints `file:line` to
serial and halts on failure.

**`errno.h`:** `errno` (8) used as a global int. Only `EISDIR` is tested (in
`M_FileExists`). Define `errno` as a global `int` and `EISDIR` as a constant.

**`limits.h`:** `INT_MAX` used in `I_GetPaletteIndex`. Define as `0x7FFFFFFF`.

**`inttypes.h`:** `PRIx64` macro. Define as `"llx"` or stub.

**`fcntl.h`:** Included but only `O_RDONLY` etc. are used in platform-specific
ports. Can be an empty header.

### 2.6 POSIX / platform functions to stub

| Function            | Used in                | Strategy                                                                      |
| ------------------- | ---------------------- | ----------------------------------------------------------------------------- |
| `getenv`            | `d_iwad.c`, `m_misc.c` | Return `NULL`. WAD is loaded via multiboot module, not filesystem path.       |
| `mkdir`             | `m_misc.c`             | No-op or create ramdisk directory entry. Only called for save game directory. |
| `system`            | `i_system.c`           | Return `-1`. Only used for Zenity error dialogs on Linux.                     |
| `isatty` / `fileno` | `i_system.c`           | Behind `ORIGCODE` guard. Stub: return `0`.                                    |
| `strcasecmp`        | 9 files                | POSIX, not C standard. Implement as case-insensitive `strcmp` loop.           |
| `strncasecmp`       | 2 files                | POSIX, not C standard. Implement as case-insensitive `strncmp`.               |

---

## 3. Exokernel syscall specification

The following syscall interface is derived bottom-up from the actual
requirements of doomgeneric, the libc shim, and the LibOS infrastructure. Each
syscall is mapped to the Doom feature that requires it.

> **This section has a C counterpart:** `src/exo_syscall.h` (SCRUM-24) declares
> the numbers as `EXO_SYS_*`, the shared argument structs, and one inline stub
> per syscall; the `EXO_E*` error codes live in `src/exo_errno.h` (SCRUM-57),
> which `exo_syscall.h` includes so existing users see no difference. Together
> they are the same interface expressed in C, and the two must be changed
> together — this document stays the source of truth for *what* each syscall
> does. Kernel sources are compiled with `-DEXO_KERNEL`, which suppresses the
> user-side stubs; the LibOS includes it plainly. The define comes from the
> compiler command line rather than a per-file `#define` because `#pragma once`
> would make a `#define` placed after any transitive include of the header
> silently ineffective. `tests/kernel/test_exo_syscall_k.c` asserts the header
> still agrees with §3.1 and §3.2; `tests/kernel/test_exo_errno_k.c` does the
> same for §3.2a.

### 3.1 Syscall calling convention

- **Entry:** `syscall` instruction (x86_64 fast syscall mechanism)
- **Syscall number:** `RAX`
- **Arguments:** `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9` (up to 6 arguments)
  - Note: `R10` replaces `RCX` because `syscall` clobbers `RCX` (saves `RIP`
    there) and `R11` (saves `RFLAGS`)
- **Return value:** `RAX` (negative = error code)
- **Register preservation:** the kernel preserves every register except `RAX`
  (the return value), `RCX` and `R11` (destroyed by the `syscall` instruction
  itself). This is deliberately the Linux guarantee, and it is stronger than
  the SysV C ABI: the argument registers `RDI`/`RSI`/`RDX`/`R10`/`R8`/`R9` are
  caller-saved in C, but a syscall must leave them intact. The LibOS stubs in
  `src/exo_syscall.h` declare only `rcx`, `r11` and `memory` as clobbered and
  pass the arguments as plain inputs, so the compiler is entitled to keep a
  value live in an argument register across a `syscall` — if the SCRUM-32
  dispatcher restored only the callee-saved set, a loop that reuses an argument
  would silently pass garbage on its second iteration.

```c
// LibOS-side syscall stub example:
static inline int64_t exo_syscall1(uint64_t num, uint64_t arg1) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(num), "D"(arg1)
                     : "rcx", "r11", "memory");
    return ret;
}
```

### 3.2 Syscall table

> **Implementation status key:** ⬜ Not started · 🔄 Prerequisite in progress ·
> ✅ Done

| #  | Syscall                             | Category    | Status | Description + Doom usage                                                                                                                                                                                                                                                       |
| -- | ----------------------------------- | ----------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 0  | `exo_page_alloc()`                  | Memory      | ✅     | Allocate one 4K physical page. Returns physical address, or `-ENOMEM`. Bound to the dispatcher (SCRUM-34).                                                                        |
| 1  | `exo_page_free(paddr)`              | Memory      | ✅     | Free a physical page. **Ownership-checked (§3.3):** `-EPERM` unless the caller owns `paddr`, `-EINVAL` for a misaligned/out-of-range address or a double free of an already-free page. On success (SCRUM-159) also unmaps every one of the caller's own mappings of `paddr` in its own address space — see §3.7 — so a page the PMM hands to a new owner can never be reached through the previous owner's stale mapping. Bound to the dispatcher (SCRUM-34), ownership enforcement SCRUM-152, mapping teardown SCRUM-159 (`src/syscall_mem.c`, `src/vmm.c`). |
| 2  | `exo_page_map(vaddr, paddr, flags)` | Memory      | ✅     | Map physical page at virtual address in caller's address space. `flags`: `EXO_PAGE_READ`/`WRITE`/`USER`/`EXEC`. `EXEC` is accepted and ignored until `EFER.NXE` is enabled; `READ` is not representable on x86 (present implies readable) and is accepted and ignored. **Ownership-checked (§3.3):** `paddr` must be owned by the caller (or be the framebuffer the caller has acquired), and `vaddr` must lie in the LibOS window `[EXO_USER_VA_BASE, EXO_USER_VA_END)` — §3.7. Returns `0`, `-EINVAL` (misaligned address, unknown flag bit), `-EPERM` (window or ownership) or `-ENOMEM` (no page for an intermediate page table). Implemented in SCRUM-35 (`src/syscall_mem.c`, `src/vmm.c`), enforcement SCRUM-153. |
| 3  | `exo_page_unmap(vaddr)`             | Memory      | ✅     | Unmap a virtual page; the physical page stays allocated (`exo_page_free` returns it). Only unmaps a mapping of a page the caller owns, holds the FB binding for, or that belongs to nobody — §3.7. Returns `0`, `-EINVAL` (misaligned, or nothing mapped there), `-EPERM` or `-ENOMEM`. Implemented in SCRUM-35, enforcement SCRUM-153. |
| 4  | `exo_fb_acquire(info_out)`          | Framebuffer | ✅     | Write framebuffer info (`phys_addr`, `width`, `height`, `pitch`, `bpp`) to `info_out` struct, backed by a **private, RAM-backed virtual framebuffer sized to the real framebuffer's geometry — not the real hardware framebuffer itself** (multiplexing, SCRUM-112, §3.3, §3.5). Every context gets its own on request; there is no exclusivity and no `-EBUSY` anymore. LibOS then calls `exo_page_map` to map it — an ordinary owned-page mapping, not a binding check. Freed (the pages) when the terminating context's pages are reclaimed, and (the directory entry) explicitly on `exo_exit` / forced revocation. Used by `DG_Init`. Returns `0` (including a re-acquire by the current owner, which re-fills the struct with the *same* buffer), `-EFAULT` if `[info_out, info_out + sizeof(exo_fb_info_t))` is not entirely inside `[EXO_USER_VA_BASE, EXO_USER_VA_END)` (SCRUM-54, same `exo_range_in_user_window` check #8 uses), `-ENOMEM` if no contiguous run of pages that size is free, or `-ENODEV` on a machine the bootloader gave no framebuffer. Implemented in SCRUM-154 (`src/syscall_fb.c`, `src/fb_binding.c`) and replaced by SCRUM-112 (`src/fb_shadow.c`); what actually reaches the screen is decided independently by `src/fb_compositor.c`, which composites whichever context is `context_current()` onto the real hardware framebuffer on a throttled PIT tick (`src/pit.c`). |
| 5  | `exo_get_ticks()`                   | Timer       | ✅     | Return `uint32_t` milliseconds since boot. Zero arguments. Never fails. Used by `DG_GetTicksMs` and `DG_SleepMs`. Kernel-side PIT + `kernel_get_ticks_ms()` done (SCRUM-9, -10); bound to the dispatcher in SCRUM-172 (`src/syscall_pit.c`) — `pic_remap()`/IRQ0/`pit_init()` moved ahead of the `TESTING` branch in `kernel_main` so ticks advance during a test boot too. |
| 6  | `exo_kbd_poll(event_out)`           | Input       | ✅     | Dequeue next keyboard event into `event_out` struct `{uint8_t pressed; uint8_t key; uint8_t modifiers; uint8_t reserved}`. `key` is a decoded `ps2_key_t` index (`KEY_A`, `KEY_ESC`, …), not a raw PS/2 scancode — the kernel's scancode decoder runs before the event is queued. `modifiers` is the `EXO_MOD_*` shift/ctrl/alt mask sampled when the event was queued, so a chord decodes correctly even if the modifier is released before the LibOS polls — Doom binds shift (run), ctrl (fire) and alt (strafe). Returns `1` if event available, `0` if empty, `-EXO_EFAULT` if `event_out` isn't entirely inside the LibOS window. No ownership to check — the keyboard isn't acquired/released like the framebuffer. Bound in `src/syscall_kbd.c` (SCRUM-39) on top of the IRQ1 handler + scancode table (SCRUM-13/-14) and ring buffer (SCRUM-18), all now done. |
| 7  | `exo_mouse_poll(state_out)`         | Input       | ⬜     | Write accumulated mouse state `{int16_t dx; int16_t dy; uint8_t buttons; uint8_t reserved}` to `state_out`, then reset accumulators. `reserved` is zeroed by the kernel and keeps the struct a fixed 6 bytes. Returns `0`. Prerequisite: PS/2 mouse init (SCRUM-19, Sprint 2).                                                                                            |
| 8  | `exo_serial_write(buf, len)`        | Debug       | ✅     | Write `len` bytes from `buf` to COM1. Returns `len` on success (COM1 is a busy-wait UART; there is no partial write), `-EFAULT` if `[buf, buf+len)` is not entirely inside `[EXO_USER_VA_BASE, EXO_USER_VA_END)`, `-EINVAL` if `len` exceeds 4096 (`SERIAL_WRITE_MAX_LEN`) — the whole call runs with interrupts off (`syscall`'s FMASK clears IF), so an uncapped write would stall the machine for as long as COM1 takes to drain it. `len == 0` always succeeds regardless of `buf`. No ownership to check — COM1 is not acquired/released the way the framebuffer is, just a channel every LibOS may write to. Implemented in SCRUM-50 (`src/syscall_serial.c`). `printf` now routes through it for the ring-3 LibOS build (SCRUM-51, `src/stdio.c`, gated on `#ifdef EXO_KERNEL`); `fprintf` is still unimplemented (§2.3).                                    |
| 9  | `exo_file_open(path, mode)`         | File I/O    | ⬜     | Open a file on the ramdisk/ATA filesystem. `mode`: `0`=read, `1`=write, `2`=read+write. Returns file descriptor (≥ 0) or negative error. Used by `fopen` shim.                                                                                                                 |
| 10 | `exo_file_close(fd)`                | File I/O    | ⬜     | Close file descriptor. Returns `0` or `-EBADF`. Used by `fclose` shim.                                                                                                                                                                                                         |
| 11 | `exo_file_read(fd, buf, count)`     | File I/O    | ⬜     | Read up to `count` bytes from `fd` into `buf`. Returns bytes read, `0` at EOF, or negative error. Used by `fread` shim.                                                                                                                                                        |
| 12 | `exo_file_write(fd, buf, count)`    | File I/O    | ⬜     | Write `count` bytes from `buf` to `fd`. Returns bytes written or negative error. Used by `fwrite` shim.                                                                                                                                                                        |
| 13 | `exo_file_seek(fd, offset, whence)` | File I/O    | ⬜     | Seek to position. `whence`: `0`=`SEEK_SET`, `1`=`SEEK_CUR`, `2`=`SEEK_END`. Returns new position or negative error. Used by `fseek`/`ftell` shim.                                                                                                                              |
| 14 | `exo_file_stat(path, size_out)`     | File I/O    | ⬜     | Write file size to `*size_out`. Returns `0` or `-ENOENT`. Used by `M_FileExists` (`fopen` check) and `M_FileLength`.                                                                                                                                                           |
| 15 | `exo_file_remove(path)`             | File I/O    | ⬜     | Delete a file. Returns `0` or `-ENOENT`. Used by `remove()` for old save games.                                                                                                                                                                                                |
| 16 | `exo_file_rename(old, new)`         | File I/O    | ⬜     | Rename a file. Returns `0` or negative error. Used by `rename()` for save game rotation.                                                                                                                                                                                       |
| 17 | `exo_sound_tone(freq, dur_ms)`      | Sound       | ⬜     | Play a tone on the PC speaker at `freq` Hz for `dur_ms` milliseconds. Non-blocking (kernel manages PIT ch2). Returns `0`. Used by `I_StartSound` shim.                                                                                                                         |
| 18 | `exo_sound_stop()`                  | Sound       | ⬜     | Silence the PC speaker immediately. Returns `0`. Used by `I_StopSound` shim.                                                                                                                                                                                                   |
| 19 | `exo_yield()`                       | Scheduling  | ✅     | Cooperatively yield CPU to next runnable LibOS context. Returns when rescheduled. Called once per idle-loop iteration by the real shell LibOS (SCRUM-110, `src/shell/shell_main.c`) and optionally by `DG_SleepMs`. Still a no-op in practice today: nothing yet registers a second context via `context_create()` on a normal boot (Doom is not linked in), so the call always finds nothing else `READY`. Bound in SCRUM-109 (`src/syscall_yield.c`) to `context_switch_request()` (SCRUM-108) via a round-robin scan of the context table, `context_next_ready()` (`src/context.c`) — the minimum policy the acceptance criterion needs, not a real scheduler (priority/fairness/wake-on-event is SCRUM-147's job). No error return: if nothing else is `READY`, including a caller with no row in the table (the boot-time default LibOS), it is a no-op that returns `0`.                                                                                                                                          |
| 20 | `exo_exit(code)`                    | Lifecycle   | ✅     | Terminate calling LibOS. Frees its pages and framebuffer binding (`revoke_all`) and hands off to whatever's next-ready via `context_switch_request()` — leaves the now-resourceless `context_t` row behind rather than destroying it (the outgoing side of that switch still needs it live to capture into); the caller that launched this LibOS is what eventually `context_destroy()`s it, on its own next relaunch (see #21/#22). Does not return. Implemented in SCRUM-155, extended in SCRUM-178 (`src/syscall_exit.c`). |
| 21 | `exo_launch_wad_viewer()`           | Lifecycle   | ✅     | Build the WAD/flat/automap viewer (`src/libos_wad_viewer/`) as a second, real LibOS context and `context_switch_request()` to it immediately; reclaims (`revoke_all` + `context_destroy`) whatever the previous launch left behind first. Like `exo_yield()`, does not return control to the caller until something switches back — here, the viewer's own `exo_exit()`/`exo_yield()` round trip. Returns a negative `EXO_E*` only if the launch failed before the switch was armed. Invoked by the shell's `wadview` command. Implemented in SCRUM-178 (`src/syscall_launch.c`). |
| 22 | `exo_launch_clock()`                | Lifecycle   | ✅     | Same shape as #21, for the clock demo LibOS (`src/libos_clock/`) — no external resource to stage and no launch-time parameters, so no `libos_launch_patch_params()` step; multiple live LibOS contexts render concurrently under framebuffer multiplexing (SCRUM-112), so control returns via Ctrl+Tab (SCRUM-111) rather than the clock itself yielding back. Invoked by the shell's `clock` command. Implemented in SCRUM-168 (`src/syscall_launch.c`). |
| 23 | `exo_launch_snake()`                | Lifecycle   | ✅     | Same shape as #21, for the Snake LibOS demo (`src/libos_snake/`) — no external resource to stage and no launch-time parameters, so no `libos_launch_patch_params()` step. Invoked by the shell's `snake` command. Implemented in SCRUM-182 (`src/syscall_launch.c`). |
| 24 | `exo_launch_doom()`                 | Lifecycle   | ✅     | Build and switch to the Doom LibOS (`src/libos_doom/`, the 79 vendored engine objects plus the libc shim). Shaped like #21 rather than #23: the WAD module is mapped read-only at `LIBOS_WAD_VADDR` and its address/length patched in via `libos_launch_patch_params()`, where `DG_Init` reads them. Invoked by the shell's `doom` command. Implemented in SCRUM-66 (`src/syscall_launch.c`). |
| 25 | `exo_memstat(stat_out)`             | Introspection | ✅   | Write a page-usage snapshot of the whole PMM to `stat_out`: `{uint32_t total_pages, free_pages, kernel_pages, libos_pages, region_count, reserved}` (24 bytes). Deliberately unscoped — every context's pages, not just the caller's. Returns `0`, or `-EFAULT` if `[stat_out, stat_out + sizeof(exo_memstat_t))` is not entirely inside the LibOS window and mapped writable (same check #4/#6 use). Invoked by the shell's `memstat` command. Implemented in SCRUM-113 (`src/syscall_stat.c`, `src/page_alloc.c`). |
| 26 | `exo_pslist(out, max)`              | Introspection | ✅   | Write up to `max` `exo_ps_info_t` entries (`{uint16_t id; uint8_t state; uint8_t reserved; uint32_t page_count}`, 8 bytes each) to `out` — one per live LibOS context, the caller included (the shell is itself `context_create()`'d, SCRUM-178). `state` mirrors `context_state_t`: `1`=READY, `2`=RUNNING, `3`=BLOCKED. Returns the number of entries written (`0..max`), `-EINVAL` if `max` exceeds `EXO_PSLIST_MAX` (`#define EXO_PSLIST_MAX CONTEXT_MAX` — `src/exo_syscall.h` `#include`s `src/context.h` rather than restating the number, after a hardcoded copy went stale and silently re-capped this at 3 across SCRUM-193's `CONTEXT_MAX` bump), or `-EFAULT` if `[out, out + max * sizeof(exo_ps_info_t))` is not entirely inside the LibOS window and mapped writable. Invoked by the shell's `pslist` command. Implemented in SCRUM-113 (`src/syscall_stat.c`, `src/context.c`). |
| 27 | `exo_disk_read(lba, buf, count)`    | Storage     | ✅     | Read `count` consecutive 512-byte sectors starting at 28-bit LBA `lba` into `buf` (at least `count * 512` bytes), one `ata_read_sector()` call per sector. Returns `count` on success (`count == 0` always succeeds and touches nothing, checked before everything else below), `-EINVAL` if `count` exceeds `EXO_DISK_MAX_SECTORS` (128, `src/syscall_disk.h`) or `lba + count - 1` overflows 28-bit LBA space, `-ENODEV` if no drive was detected at boot, `-EBUSY` if the caller does not hold the disk binding (§3.5a — a headless machine answers `-ENODEV` rather than `-EBUSY`, same precedence `exo_disk_acquire` uses), `-EFAULT` if `[buf, buf + count*512)` is not entirely inside `[EXO_USER_VA_BASE, EXO_USER_VA_END)` and mapped writable (same `exo_range_in_user_window`/`exo_user_range_mapped` pair #8 uses — checked last, so a caller with no claim on the disk learns nothing about whether its buffer pointer happens to be valid), or `-EIO` if a sector faults partway through the transfer (no partial-success byte count — ATA gives no way to tell "this sector failed" from "this sector was never attempted" once a fault stops the loop). Sector-addressed only, zero filesystem knowledge — the ported FAT-like fs (SCRUM-189) is LibOS-side, on top of this. Implemented in SCRUM-103 (`src/syscall_disk.c`, on top of the ATA PIO driver, SCRUM-102, `src/ata.c`); ownership enforcement SCRUM-188 (`src/disk_binding.c`). |
| 28 | `exo_disk_write(lba, buf, count)`   | Storage     | ✅     | Same shape as #27, writing `count` sectors from `buf` via `ata_write_sector()`. `buf` is checked readable rather than writable. Same error codes, same binding check. Implemented in SCRUM-103 (`src/syscall_disk.c`); ownership enforcement SCRUM-188. |
| 29 | `exo_disk_acquire()`                | Storage     | ✅     | Bind the disk to the caller — one exclusive owner at a time, the same pre-SCRUM-112 shape `exo_fb_acquire` (#4) originally had (§3.5a). Must succeed before #27/#28 will do anything for the caller. Returns `0` (including a re-acquire by the current owner), `-EBUSY` if another context holds it, or `-ENODEV` if this machine has no drive. Implemented in SCRUM-188 (`src/syscall_disk.c`, `src/disk_binding.c`). |

**Total: 30 syscalls.** This is the complete interface needed to run Doom with
save/load, config, sound, and cooperative multitasking, plus the three
LibOS-launch syscalls (#21/#22/#23) that back the shell's interactive demo
commands, the two introspection syscalls (#25/#26) that back its
`memstat`/`pslist` commands, and the three storage syscalls (#27/#28/#29) the
future FAT-like filesystem (SCRUM-189) will build on.

### 3.2a Error codes (SCRUM-57)

`src/exo_errno.h` is the canonical definition; this table is the canonical
*documentation* of it — one place instead of the per-handler comments
scattered across `syscall_mem.c`/`syscall_fb.c`/`syscall_kbd.c`/
`syscall_serial.c`. Change one, update the other, the same rule §3's own
intro states for `exo_syscall.h`.

Every value matches the Linux errno number of the same name, and — not by
coincidence — `src/errno.h`'s value for the unprefixed name: nothing wires
an `EXO_E*` return into libc's `errno` yet (`docs/libc_audit.md`), but
keeping the numbering identical means that wiring, when it happens, is a
pass-through rather than a translation table.

| Code | Value | Meaning | Emitted today by |
| --- | --- | --- | --- |
| `EXO_EPERM` | 1 | Operation not permitted for this LibOS | `exo_page_free` (#1), `exo_page_map`/`exo_page_unmap` (#2/#3) |
| `EXO_ENOENT` | 2 | No such file | not yet — reserved for `exo_file_*` (#9-16) |
| `EXO_EIO` | 5 | Disk I/O fault partway through a sector transfer | `exo_disk_read`/`exo_disk_write` (#27/#28) |
| `EXO_EBADF` | 9 | Bad file descriptor | not yet — reserved for `exo_file_*` |
| `EXO_ENOMEM` | 12 | Out of physical pages / heap | `exo_page_alloc` (#0), `exo_page_map` (#2, no page for an intermediate table), `exo_page_unmap` (#3, split requires a page), `exo_fb_acquire` (#4, no contiguous run free for the virtual framebuffer, SCRUM-112) |
| `EXO_EACCES` | 13 | Permission denied | not yet bound to a handler |
| `EXO_EFAULT` | 14 | Pointer argument outside the caller's address space | `exo_fb_acquire` (#4), `exo_kbd_poll` (#6), `exo_serial_write` (#8), `exo_disk_read`/`exo_disk_write` (#27/#28) |
| `EXO_EBUSY` | 16 | Resource held by another LibOS | `exo_disk_acquire`/`exo_disk_read`/`exo_disk_write` (#27/#28/#29, SCRUM-188) — `exo_fb_acquire` (#4) returned this too, pre-SCRUM-112, when the framebuffer was a single exclusive binding rather than a private buffer per caller |
| `EXO_EEXIST` | 17 | File already exists | not yet — reserved for `exo_file_open`/`exo_file_rename` |
| `EXO_ENODEV` | 19 | The hardware resource does not exist on this machine | `exo_fb_acquire` (#4, no framebuffer), `exo_disk_read`/`exo_disk_write`/`exo_disk_acquire` (#27/#28/#29, no drive detected at boot) |
| `EXO_ENOTDIR` | 20 | Not a directory | not yet — reserved for `exo_file_*` |
| `EXO_EISDIR` | 21 | Is a directory | not yet — reserved for `exo_file_*` |
| `EXO_EINVAL` | 22 | Malformed or out-of-range argument | `exo_page_free` (#1), `exo_page_map`/`exo_page_unmap` (#2/#3), `exo_serial_write` (#8, `len` over `SERIAL_WRITE_MAX_LEN`), `exo_disk_read`/`exo_disk_write` (#27/#28, `count` over `EXO_DISK_MAX_SECTORS` or LBA range overflow) |
| `EXO_ENFILE` | 23 | System-wide open-file table full | not yet — reserved for `exo_file_open` |
| `EXO_EMFILE` | 24 | Per-context file descriptor table full | not yet — reserved for `exo_file_open` |
| `EXO_EFBIG` | 27 | File too large | not yet — reserved for `exo_file_write` |
| `EXO_ENOSPC` | 28 | Ramdisk full | not yet — reserved for `exo_file_write` |
| `EXO_ESPIPE` | 29 | Seek on a non-seekable descriptor | not yet — reserved for `exo_file_seek` |
| `EXO_EROFS` | 30 | Write attempted on a read-only filesystem | not yet — reserved for `exo_file_write` (e.g. a memory-mapped WAD reader, §4.1) |
| `EXO_ENOSYS` | 38 | Syscall number not implemented, or out of range | `exo_syscall_dispatch` (`src/syscall.c`) for every unbound number — #7, #9-20 today |

**"0 on success" is the default, not a universal rule.** Several bound
syscalls document a positive success value instead of `0`, because the value
itself *is* the answer the caller asked for, not a status flag:

| Syscall | Success return | Why not `0` |
| --- | --- | --- |
| `exo_page_alloc` (#0) | the allocated page's physical address | the caller needs the address; a separate out-parameter would be one more pointer to fault-check |
| `exo_get_ticks` (#5) | milliseconds since boot | the syscall's entire purpose is returning this number |
| `exo_kbd_poll` (#6) | `1` if an event was dequeued, `0` if the queue was empty | "queue empty" is not a failure — it is the expected steady state between keystrokes |
| `exo_serial_write` (#8) | bytes written (`len`, since COM1 never partially writes) | mirrors POSIX `write()`; `0` would be indistinguishable from "wrote nothing" |
| `exo_disk_read`/`exo_disk_write` (#27/#28) | sectors transferred (`count`, since success means every sector transferred) | mirrors `exo_serial_write`'s reasoning; `count == 0` is itself the no-op case, not an error |

Every other bound syscall (`exo_page_free`, `exo_page_map`, `exo_page_unmap`,
`exo_fb_acquire`) returns exactly `0` on success, matching the acceptance
criterion literally.

### 3.3 Secure binding & resource ownership

The syscall numbers above describe *what* each call does; this section
describes the protection rule that makes them exokernel operations rather than
an unprotected physical `mmap`/allocator. It is the concrete mechanism behind
the guarantee in `architecture.md` §2 ("no LibOS can corrupt another's pages or
access hardware it hasn't been granted"). Tracked under epic **SCRUM-151
(Resource Protection & Secure Binding)**.

**Ownership model.** The kernel tags every 4 KiB physical page and the
framebuffer with an *owner*: `KERNEL`, `FREE`, or a specific LibOS context id
(SCRUM-152). A resource syscall is a *secure binding* — the kernel enforces the
tag on every operation, without understanding what the LibOS does with the
resource:

- `exo_page_alloc` **establishes** a binding: the returned page is tagged with
  the caller's context id.
- `exo_page_map` / `exo_page_unmap` / `exo_page_free` **enforce** it: an
  operation on a page the caller does not own returns `-EPERM`
  (`EXO_EPERM`, `src/exo_syscall.h`; note this error code was reserved from the
  start as "operation not permitted for this LibOS"). This closes the hole
  where a LibOS could pass an arbitrary `paddr` to `exo_page_map` and reach
  kernel or peer memory (SCRUM-153).
- `exo_fb_acquire` used to bind the *real* framebuffer to one LibOS at a time
  (`-EBUSY` otherwise, SCRUM-154). SCRUM-112 (framebuffer multiplexing)
  replaced that: every caller now gets its own private, RAM-backed virtual
  framebuffer (`src/fb_shadow.c`), allocated from ordinary PMM pages and
  mapped like any other owned page — no exclusivity, no binding to check.
  `src/fb_binding.c`'s binding table still exists and still guards the *real*
  framebuffer's physical range (`fb_binding_check_map()`, below) — nothing
  ever calls `fb_binding_acquire()` from the syscall path anymore, so that
  range is now permanently unmappable by any LibOS, full stop. What actually
  reaches the screen is `src/fb_compositor.c`: it copies whichever context is
  `context_current()`'s virtual framebuffer onto the real one on a throttled
  PIT tick (`src/pit.c`), independent of who has acquired what.
- `exo_exit` **reclaims** all bindings held by the terminating context — frees
  its pages, releases the framebuffer, closes its files (SCRUM-155).

**Revocation.** The kernel additionally reserves the right to *revoke* a
granted resource — the exokernel "repossession" model, and the half of the
bargain that makes generous grants safe: the kernel never has to refuse a
request out of fear that it can never get the resource back. The mechanism and
its ownership-table bookkeeping are in place (SCRUM-156, `src/revoke.c`); for
the single-app v1 demo the *policy* is trivial — nothing asks for a resource
back on the boot path, and reclamation happens only when a context exits. §3.6
is the design note: the full protocol, what exists today, and what SCRUM-147
adds.

**Testing.** `tests/kernel/test_ownership_k.c` asserts a context cannot
map/free a page it does not own and that framebuffer acquisition is mutually
exclusive — distinct from the paging/privilege-wall tests SCRUM-55/56
(SCRUM-157).

### 3.4 Entry path (SCRUM-32)

§3.1 states the contract; this section is how the kernel keeps it. Implemented
in `src/syscall.c` (MSR setup, dispatch), `src/syscall_entry.s` (the stub) and
`src/boot.s` (the GDT), and tested in `tests/kernel/test_syscall_k.c`.

**MSRs.** `syscall_init()` runs from `kernel_main` before any ring-3 code
exists and programs:

| MSR | Value | Why |
| --- | --- | --- |
| `IA32_EFER` | `\|= SCE` | Makes `syscall` legal at all. Read-modify-write: `boot.s` already set `LME` here and clobbering it drops the CPU out of long mode. |
| `IA32_STAR` | `[47:32]=0x08`, `[63:48]=0x18\|3` | Segment selectors for entry and return. |
| `IA32_LSTAR` | `&syscall_entry` | Where `syscall` jumps. |
| `IA32_FMASK` | `IF\|DF\|TF\|AC\|NT` | RFLAGS bits cleared on entry, so the kernel never inherits a hostile flags word. `IF` matters most — see the reentrancy note below. |

**GDT layout is forced, not chosen.** `syscall` loads `CS = STAR[47:32]` and
`SS = STAR[47:32] + 8`; `sysretq` loads `CS = STAR[63:48] + 16` and
`SS = STAR[63:48] + 8`. Both halves name the *base of a descriptor pair*, so
the selectors in `src/boot.s` must appear in exactly this order:

| Selector | Descriptor |
| --- | --- |
| `0x08` | kernel code64, DPL 0 |
| `0x10` | kernel data, DPL 0 |
| `0x18` | user code32, DPL 3 — a placeholder long mode never loads, present only so the arithmetic lands correctly |
| `0x20` | user data, DPL 3 |
| `0x28` | user code64, DPL 3 |

Reordering these, or closing the `0x18` gap, silently breaks every syscall
return. The ring-3 descriptors were originally SCRUM-45's; they landed with
SCRUM-32 because `STAR` cannot be programmed legally without them.

**Stack switch.** Unlike an interrupt, `syscall` does not change `RSP` — the
caller's stack pointer is still live on entry, and from ring 3 it is
attacker-controlled. The stub swaps it in two `%rip`-relative moves, which need
no scratch register (every register except `RCX`/`R11` still belongs to the
caller at that instant), onto a dedicated 16 KiB kernel stack — deliberately
not `boot.s`'s boot stack, which the kernel may already be nested on.

The outgoing user `RSP` is parked in a single global, so **the path is not
reentrant**. That is safe today only because `FMASK` clears `IF` and there is
one CPU. SCRUM-107 added the context table that *tracks* multiple LibOS
contexts (`src/context.c/h` — id, page dir, saved registers, state); SCRUM-108
adds the actual switch (`context_switch_request()` + `src/context_switch.s`'s
`context_switch_tail`, spliced into this file's epilogue right after
`exo_syscall_dispatch()` returns), but deliberately does **not** fix this
reentrancy gap the "real" way. It instead stages the outgoing/incoming
context's state through this same single global at each switch boundary —
still correct, because `FMASK` clears `IF` for the whole syscall/switch window
and this kernel targets exactly one CPU, so no second entry can interleave —
rather than the full `swapgs` + per-CPU block (`IA32_KERNEL_GS_BASE`) rework
this note used to point at. That full rework is tracked as SCRUM-176, needed
once SCRUM-127 (preemptive, IRQ-driven switching) wants to trigger a switch
from inside an interrupt handler with `IF` set — the one case SCRUM-108's
staged fix does not cover. One consequence worth flagging for the next
reader: `RCX`/`R11` are caller-saved in the C ABI, so by the time
`context_switch_tail` runs (after `exo_syscall_dispatch()` and whatever C it
called), the *live* `RCX`/`R11` no longer hold the outgoing context's ring-3
`RIP`/`RFLAGS` — `context_switch_tail` reads them back from the stack slots
this stub's own prologue pushed instead; see that file's comment.

No TSS is involved: `syscall` never consults `TSS.RSP0` — SCRUM-46's TSS
matters to ring-3 code taking an *interrupt or exception*, not to it making a
syscall. `src/tss.c`'s `tss_init()` loads one anyway (called from
`kernel_main` right after `idt_init()`), because every gate `idt_init()`
installs has `IST=0`, and a CPL 3 → CPL 0 exception with `IST=0` loads its
stack from `TSS.RSP0` regardless of whether anything ever calls `syscall`.
Without it, a fault taken at CPL 3 has no valid stack to build its frame on
and triple-faults — see the page-fault bullet under §3.7 below.

**Port I/O is walled off as a side effect of a correctly-sized TSS
(SCRUM-56).** `tss_init()` sets `tss.iomap_base = sizeof(struct tss64)`,
pointing one byte past the segment limit. Nothing in `boot.s`/`libos_enter.s`
ever raises `RFLAGS.IOPL` above 0 either, so a ring-3 `IN`/`OUT` fails both
ways at once: CPL(3) > IOPL(0) traps regardless, and even if IOPL were
raised, "no I/O permission bitmap" would still deny it. The trap lands on
vector 13 (#GP), which `gpf_stub`/`gp_fault_handler` (`src/isr.s`/`src/fault.c`)
now report the same way `pf_stub`/`page_fault_handler` report a page fault —
sharing the same `exception_frame_t` and the same TESTING-only
`fault_set_hook()` resume mechanism, rather than the old `error_stub`, which
discarded the error code and `iretq`'d straight back into the same faulting
instruction forever. `tests/kernel/test_port_io_fault_k.c` (with
`tests/kernel/port_io_fault_probe.s`) launches a real LibOS address space via
`libos_build_image()`, executes `outb %al, $0x80` at CPL 3, and asserts the
hook fired exactly once, at CPL 3, before the probe resumed past the
instruction and returned normally through `libos_return()` — proving the
kernel is the only path to hardware, not just documenting that it should be.

**Register preservation.** The stub saves all 14 registers it must return
intact — the six argument registers included, not merely the SysV callee-saved
set — plus `RCX` and `R11`, which `sysretq` needs as the return `RIP` and
`RFLAGS`. It then shifts each argument one position right into the SysV order
`exo_syscall_dispatch` expects, with the sixth argument on the stack.

`tests/kernel/test_syscall_k.c` enforces this by dropping to CPL 3
(`tests/kernel/ring3_probe.s`), loading a distinct sentinel into every
preserved register, issuing the echo syscall **twice without reloading them**,
and checking all twelve afterwards. The second call is the point: it is the
failure §3.1 describes, where a compiler keeps a value live in an argument
register across a `syscall`. A stub narrowed to the callee-saved set passes the
first call and fails the second.

**Dispatch.** `exo_syscall_dispatch` unsigned-compares the number against
`EXO_SYS_COUNT` — a "negative" number from ring 3 is a very large unsigned one,
and a signed compare would index the table backwards — then calls the
registered handler, or returns `-EXO_ENOSYS` if the number is unbound. Handlers
bind with `exo_syscall_register()`. SCRUM-32 binds none: it delivers the entry
path, and the individual handlers land across Sprints 3-5 starting with
SCRUM-33.

**Ring-3 access during tests.** `boot.s` builds the identity map without the
U/S bit, so ring-3 code cannot execute. TESTING builds are assembled with
`--defsym RING3_PROBE=1`, which sets U/S at every level for the probe's
benefit on *that* map. It stops mattering the moment `vmm_init()` loads CR3
onto its own map, though — nothing runs at CPL 3 before that point on any
boot, test or otherwise — and `src/vmm.c`'s `KERNEL_MAP_USER` used to repeat
the same blanket exposure on the map that actually matters, for the same
reason: `tests/kernel/ring3_probe.s` and `tss_fault_probe.s` predate
SCRUM-48's per-LibOS address spaces and execute directly against the
kernel's own tables. SCRUM-55 closes that properly: `KERNEL_MAP_USER` is
supervisor-only in every build now, and `vmm_init()` separately re-exposes
just those two probes' own code ranges (`expose_ring3_legacy_probes()`) as
the sole, explicitly-scoped legacy exception — see `tests/kernel/
test_kernel_mem_fault_k.c`, which asserts the wall holds everywhere else.

### 3.4a FPU/SSE state across kernel entry (SCRUM-177)

SSE is enabled. `src/boot.s`'s `_start64` clears `CR0.EM`/`CR0.TS`, sets
`CR0.MP`/`CR0.NE` and `CR4.OSFXSR`/`CR4.OSXMMEXCPT`, and loads `MXCSR` with the
architectural default `0x1F80`, before `kernel_main` runs. Doom needs this and
cannot be compiled without it — the x86_64 SysV ABI returns `float` in `%xmm0`,
so `m_config.c`'s `M_GetFloatVariable()` has no SSE-free encoding — and
allowing SSE additionally lets GCC inline struct copies engine-wide as
`movaps`/`movdqa`/`pxor`.

That raises the question this section exists to answer: **neither
`src/syscall_entry.s` nor `src/isr.s` saves any FPU, XMM or MXCSR state.** Both
save general-purpose registers only. Once ring-3 code holds live float values,
is that a corruption bug?

**Decision: no save/restore, deferred deliberately — not overlooked.**

**Why it is safe today.** The kernel cannot touch that state. Every kernel
object is compiled `-mno-sse -mno-sse2 -mno-mmx` (`docker/scripts/build.sh`),
so GCC never emits an instruction that names an XMM or MMX register into any
syscall handler, IRQ handler or fault handler; and the hand-written assembly on
those paths — `syscall_entry.s`, `isr.s`, `libos_enter.s`, `context_switch.s` —
touches general-purpose registers exclusively. A LibOS's XMM/MXCSR/x87 state
therefore survives kernel entry *by construction*: there is nothing on the
other side capable of modifying it. Saving 512 bytes of `fxsave` area on every
syscall would be spilling registers that nothing clobbers.

Note the shape of that argument. It is not "the kernel happens not to use
floats" — it is "the kernel is built such that it cannot", which is a property
a build flag enforces and a test can check.

**And it is checked.** `tests/kernel/test_sse_k.c` seeds all sixteen XMM
registers from ring 3 with distinct patterns and verifies every one of them
afterwards, across both paths:

| Path | Probe | What forces the crossing |
| --- | --- | --- |
| `syscall` → `sysretq` | `tests/kernel/sse_ring3_probe.s` | calls `exo_get_ticks` (#5) with the patterns live |
| IDT interrupt at CPL 3 | `tests/kernel/sse_irq_probe.s` | launched via `libos_enter_irq()` (RFLAGS.IF set), busy-waits until `exo_get_ticks` advances — which only IRQ0 can cause, so `irq0_stub` demonstrably ran while the patterns were live |

So the invariant is enforced by CI rather than by this paragraph.

**What would invalidate it.** Any one of these makes the deferral wrong, and
the second is the one that will actually happen:

1. **Dropping `-mno-sse` from the kernel's own `CFLAGS`.** GCC would
   immediately start inlining struct copies as `movaps` inside kernel code,
   silently clobbering `%xmm0`-`%xmm15` on every kernel entry. The doom
   compile pass (`docker/scripts/build-doom.sh`) drops the flag for
   `src/doom/` only; the kernel build must keep it. The XMM cases above are
   what catch a change here.
2. **A second live ring-3 context.** The argument above covers *the kernel*
   clobbering XMM state; it says nothing about another LibOS doing so. The
   moment two contexts are scheduled against each other, whichever one runs
   second inherits the first's XMM/MXCSR/x87 registers. `context_regs_t`
   (`src/context.h`) has no fields for any of it, and SCRUM-108's
   `context_switch.s` saves the callee-saved GPR set and nothing more — so
   **whoever makes the scheduler real owns adding `fxsave`/`fxrstor` (or
   `xsave`, or a lazy `CR0.TS` scheme) to it.** v1 runs exactly one LibOS,
   which is the whole reason this is deferrable.
3. **A libgcc routine that uses SSE.** The kernel links `-lgcc`; today it
   reaches only integer helpers. A future dependency on a libgcc float path
   would breach the invariant from outside the flag's reach.

**`MXCSR` and vector 19.** `CR4.OSXMMEXCPT` is set, which means an *unmasked*
SIMD floating-point exception is delivered as `#XM` on vector 19 instead of
`#UD`. `idt_init()` leaves `default_stub` there — a bare `iretq` that returns
to the faulting instruction and faults again forever. `boot.s` loads `MXCSR` =
`0x1F80`, masking all six exceptions, which makes that vector unreachable;
`test_sse_k.c` asserts the mask bits. **Anything that unmasks a SIMD exception
owes vector 19 a real handler first.**

### 3.5 Framebuffer binding (SCRUM-154)

§3.3 states the rule for the framebuffer; this section is the mechanism.
Implemented in `src/fb_binding.c` (the ownership table, ABI-agnostic) and
`src/syscall_fb.c` (the `#4` handler and the `-EXO_E*` mapping), tested in
`tests/kernel/test_fb_binding_k.c`. The split mirrors `page_alloc.c` /
`syscall_mem.c`.

**Publication.** `kernel_main` calls `syscall_fb_init(fb_tag)` with the
multiboot2 framebuffer tag, before the `TESTING` branch so acquire behaves
identically under the test runner. A NULL tag — or a degenerate geometry (zero
base, zero extent, or a range that would wrap the physical address space) — is
published as "this machine has no framebuffer", and every acquire then answers
`-EXO_ENODEV`. `#4` is registered either way: the syscall exists, so reporting
`-EXO_ENOSYS` would tell the LibOS the kernel is too old rather than that the
machine is headless.

**Establish.** `exo_fb_acquire` validates `info_out` *before* taking the
binding — a caller that passes garbage must not walk away owning a screen it
never received a handle to — then records `syscall_current_context()` as the
owner and fills the caller's `exo_fb_info_t`, zeroing its reserved bytes. A
re-acquire by the current owner succeeds and re-fills the struct: §3.2 makes
`-EBUSY` the answer to "another LibOS holds it", and a LibOS re-running
`DG_Init` is not that.

**Enforce.** `fb_binding_check_map(paddr, who)` is the per-page permission gate
`exo_page_map` / `exo_page_unmap` consult (SCRUM-153). It is deliberately
three-valued so a caller cannot read "not framebuffer memory" as "permitted":

```c
switch (fb_binding_check_map(paddr, syscall_current_context())) {
case FB_MAP_ALLOW:  break;              /* FB owner — proceed              */
case FB_MAP_DENY:   return -EXO_EPERM;  /* FB memory, someone else's       */
case FB_MAP_NOT_FB: /* fall through to page_owner(paddr) == caller        */
}
```

The extent is page-granular: the framebuffer's physical range is widened to
whole 4 KiB pages, because mapping permission is decided per page and the base
need not be page-aligned. An *unheld* framebuffer denies too — it is not public
property, the LibOS has to bind it first — and there is no kernel bypass: the
kernel reaches the framebuffer through the identity map (`src/fb.c`), never
through `exo_page_map`.

**Reclaim.** `fb_binding_release(who)` drops the binding if `who` holds it and
is a no-op otherwise, so reclamation can call it unconditionally for a context
that may never have acquired. It is the *voluntary* return in §3.6's protocol;
the kernel-driven form is `fb_binding_reclaim(who)`, which does the same thing
and reports whether there was anything to take. `revoke_all()` (`src/revoke.h`)
is what SCRUM-155's `exo_exit` calls to reclaim the framebuffer alongside the
context's pages. Until SCRUM-155 binds `#20`, nothing calls it on the boot
path: a LibOS that exits without releasing keeps the binding for the rest of
the boot.

**No locking.** Syscalls run with `IF` cleared by `IA32_FMASK` (§3.4) and the
entry path is single-threaded, so the read-modify-write in
`fb_binding_acquire()` cannot be interleaved. Preemptive multi-LibOS
scheduling (SCRUM-147) invalidates that assumption and will need a lock here.

> ✅ **SCRUM-112 (framebuffer multiplexing):** everything above this note
> still describes `fb_binding.c` accurately as a *module* — it is unchanged
> — but `exo_fb_acquire` (`src/syscall_fb.c`) no longer calls into it.
> "Establish" is now `src/fb_shadow.c`: every context gets its own private,
> RAM-backed virtual framebuffer, sized to the real framebuffer's published
> geometry and allocated via a new contiguous-run primitive
> (`alloc_pages_contig_owned()`, `src/page_alloc.c`) rather than a binding —
> no exclusivity, no `-EXO_EBUSY`, every caller can hold one simultaneously.
> "Enforce" (`fb_binding_check_map()`) is unchanged and still gates the
> *real* framebuffer's physical range, which is now permanently unheld
> (`fb_binding_owner()` stays `PAGE_OWNER_FREE`) since nothing acquires it
> anymore — the real framebuffer is unmappable by any LibOS, full stop.
> "Reclaim" for the virtual framebuffer is `fb_shadow_release()`, called from
> the same two places `fb_binding_release()`/`fb_binding_reclaim()` already
> were (`exo_exit`, `revoke_all()`). ✅ **SCRUM-187:** it now frees the
> underlying pages itself, from the exact `phys_base`/`page_count` its own
> directory entry tracks, rather than leaving that to the caller's
> owner-wide `reclaim_pages_owned()`/`page_reclaim_all()` sweep — that sweep
> frees *every* page the owner id holds, not just the shadow buffer's, which
> is correct at real `exo_exit` (where "free everything" is the intent) but
> was a hazard anywhere else the same owner id held unrelated pages, as
> `tests/kernel/test_fb_binding_k.c`'s own scratch page did. `exo_exit`'s
> subsequent sweep still reclaims the rest of the context's pages exactly as
> before. What decides what is actually *visible*: `src/fb_compositor.c` copies
> whichever context is `context_current()`'s virtual framebuffer onto the
> real one on a throttled PIT tick (`src/pit.c`'s `irq0_handler()`,
> ~60&nbsp;Hz) — "foreground" is simply "the context currently running,"
> with no separate state, matching the cooperative model where exactly one
> context executes at a time. A future preemptive scheduler would need to
> give "foreground" its own state; the Ctrl+Tab hotkey below does not.
>
> ✅ **SCRUM-111 (Ctrl+Tab switch hotkey):** `src/ps2.c`'s
> `ps2_process_scancode()` detects Ctrl+Tab (either Ctrl) and calls
> `context_next_ready()`/`context_switch_request()` (`src/context.c`) —
> the exact round-robin pair `exo_yield` already uses
> (`src/syscall_yield.c`), including its `PAGE_OWNER_FREE` no-op when
> nothing else is `READY` — directly from the IRQ1 path, the same way
> `irq0_handler()` already calls `fb_compositor_tick()` directly (`src/
> pit.c`). The chord is swallowed, never enqueued, so it never reaches an
> app as ordinary input. No new "foreground" state was needed after all:
> the compositor and the syscall path both simply read `context_current()`
> already. What *is* still deferred is the low-level register-save/
> CR3-swap itself — `context_switch_pending` is only actually carried out
> at the currently-running context's next syscall (`src/syscall_entry.s`'s
> epilogue), so there is a bounded window, up to one syscall long, where
> the outgoing context is still executing its own code on its own CR3.
> ✅ **SCRUM-179** closed the correctness half of that window:
> `context_switch_request()` no longer updates `context_current()` itself —
> it only stages the target id (`context_switch_in_id`) — so
> `context_current()`/`syscall_current_context()` keep naming whichever
> context's CR3 is actually loaded for the whole deferred window, and
> `context_switch_tail` (`src/context_switch.s`) is the only place that
> calls `context_set_current()`, right after it swaps CR3. Before this fix,
> any syscall the still-running outgoing context made in that window —
> including the very syscall whose dispatch return finally consumes
> `context_switch_pending` — resolved `syscall_current_context()` to the
> new target and misattributed its effects (page ownership, framebuffer
> shadow allocation, etc.) to the wrong context. The remaining latency is
> unchanged and still sub-frame in practice: every LibOS
> today (`shell_main.c`, the WAD viewer) polls `exo_kbd_poll()`/
> `exo_get_ticks()` every loop iteration. Genuine zero-latency preemption
> — switching before the running context's next syscall, from arbitrary
> interrupt context — still needs the full `swapgs` + per-CPU rework this
> section already scoped to SCRUM-176, gated on SCRUM-127; this ticket
> does not attempt that.
>
> ⚠️ **Corollary bug this hotkey exposed and fixes:** driving
> `context_next_ready()`/`context_switch_request()` from somewhere other
> than the outgoing context's own syscall handler turned out to matter for
> correctness, not just latency. `exo_exit` (`src/syscall_exit.c`) hands off
> to the next context via this exact pair, but left the exiting context's
> own row `CONTEXT_STATE_READY` — indistinguishable from a context that
> voluntarily `exo_yield()`'d and is safe to resume, even though `exo_exit`
> has already freed every page and framebuffer binding it held. Nothing
> before this ticket was ever positioned to notice: the only things that
> called `context_next_ready()` were themselves running *as* the outgoing
> context, and control never returns to a dead context's own code to make
> that call again. Ctrl+Tab breaks that pattern on purpose — pressing it at
> the shell right after quitting `wadview` round-robins straight into that
> stale, resourceless row and executes garbage from its freed pages.
> `sys_exit()` now overrides the row to `CONTEXT_STATE_BLOCKED` immediately
> after handing off — `context_next_ready()` already skips `BLOCKED` rows —
> which closes this for `exo_yield`'s own round-robin too, not just this
> hotkey's. `tests/kernel/test_syscall_exit_k.c`'s "exit blocks outgoing
> context from round-robin" is the regression test.
>
> ⚠️ **Second corollary bug, same root cause:** `shell_main()`
> (`src/shell/shell_main.c`) called `exo_yield()` unconditionally on every
> loop iteration, not just when genuinely idle. That was harmless before
> this ticket — the only way the viewer's row was ever `READY` again while
> the shell ran was a scenario that never actually arose, since
> `run_automap_viewer()` gives up control solely by exiting, never by
> yielding. Ctrl+Tab is the first thing that returns control to the shell
> while the other context is still alive *and* `READY` (by design — it must
> stay resumable), and in that case the shell's own auto-yield immediately
> round-robined straight back to it on the very next loop pass, before a
> keystroke could land. The low-level switch genuinely completed each time
> — `context_current()` really did flip to the shell — it just didn't stay
> there long enough to be usable. Fixed by removing the shell's per-loop
> `exo_yield()` outright: every real hand-off away from the shell is
> already explicit (`wadview`'s launch syscall, its own `exo_exit`, or
> Ctrl+Tab), so nothing depends on the shell volunteering control on its
> own. Verified by scripted QEMU monitor `sendkey` injection rather than a
> KUnit test, since `shell_main()` is a genuine never-returning interactive
> loop that the test harness deliberately never launches (see
> `tests/kernel/test_shell_libos_k.c`'s own comment).

### 3.5a Disk binding (SCRUM-188)

`exo_disk_read`/`exo_disk_write` (§3.2 #27/#28, SCRUM-103) shipped with no
ownership concept: the raw disk was the one exokernel resource any LibOS
could touch regardless of who else was using it. This section closes that
gap the same way §3.5 closed it for the framebuffer — except the disk stays
at the *pre-SCRUM-112* shape: a single exclusive owner, established by an
explicit acquire call. There is no per-context multiplexing here and none is
planned — a disk head cannot be virtualized into one private copy per caller
the way a framebuffer can (`src/fb_shadow.c`). Implemented in
`src/disk_binding.c` (the ownership table, ABI-agnostic, structurally a
trimmed `fb_binding.c` with no geometry to publish) and `src/syscall_disk.c`
(the `#27`/`#28`/`#29` handlers and the `-EXO_E*` mapping), tested in
`tests/kernel/test_disk_binding_k.c` and `tests/kernel/test_syscall_disk_k.c`.

**Publication.** `syscall_disk_init(drive_present)` calls
`disk_binding_init(drive_present)` before registering any of `#27`/`#28`/`#29`,
with `drive_present` the `ata_init()` result. `disk_binding_present()` is then
the single source of truth for whether a drive exists — `src/syscall_disk.c`
reads it rather than keeping a second copy of the flag, so the two can never
disagree. A machine with no drive answers `-EXO_ENODEV` from `exo_disk_acquire`
and from `exo_disk_read`/`exo_disk_write` (checked ahead of the binding check
below) rather than `-EXO_ENOSYS` — the syscalls exist, the hardware
does not.

**Establish.** `exo_disk_acquire` (`#29`) records
`syscall_current_context()` as the owner. A re-acquire by the current owner
succeeds, same "not punished for calling twice" rule §3.2 states for
`exo_fb_acquire`; only "another LibOS holds it" is `-EXO_EBUSY`.

**Enforce.** `exo_disk_read`/`exo_disk_write` check
`disk_binding_owner() == syscall_current_context()` after the count/LBA
shape check (`validate_disk_shape()`) and the drive-presence check, but
*before* the buffer check (`validate_disk_buf()`) — so `count == 0`'s
no-op-success contract runs first as before, a headless machine still
answers `-ENODEV` rather than `-EBUSY` (the same precedence
`exo_disk_acquire` uses), and a caller with no claim on the disk learns
nothing about whether its own buffer pointer happens to be valid. An unheld
or someone-else's binding is not public property, same rule
`fb_binding_check_map()` enforces for the framebuffer; both cases answer
`-EXO_EBUSY`.

**Reclaim.** `disk_binding_release(who)` drops the binding if `who` holds it
and is a no-op otherwise, mirroring `fb_binding_release()`. It is the
*voluntary* return in §3.6's protocol; the kernel-driven form is
`disk_binding_reclaim(who)`. `revoke_all()` (`src/revoke.h`) reclaims the
disk binding alongside the framebuffer and the context's pages; until
SCRUM-155 binds `#20`, nothing calls it on the boot path, so a LibOS that
exits without releasing keeps the binding for the rest of the boot.

**No locking**, for the same reason as §3.5: syscalls run with `IF` cleared
by `IA32_FMASK` (§3.4) and the entry path is single-threaded, so the
read-modify-write in `disk_binding_acquire()` cannot be interleaved.
Preemptive multi-LibOS scheduling (SCRUM-147) invalidates that assumption
the same way it does for the framebuffer.

### 3.6 Revocation & repossession (SCRUM-156)

§3.3 states that the kernel may take a granted resource back. This section is
the design note for how — the protocol in full, what of it exists today, and
what the multi-LibOS work still has to add. Implemented in `src/revoke.c` (the
protocol), `src/page_alloc.c`, `src/fb_binding.c` and `src/disk_binding.c`
(the per-resource mechanisms, the last added by SCRUM-188), and tested in
`tests/kernel/test_revoke_k.c` and `tests/kernel/test_disk_binding_k.c`.

**Why an exokernel needs this.** Secure binding answers "may this LibOS touch
this resource?". Revocation answers the question that makes binding *safe to
be generous with*: having granted a resource, can the kernel get it back? Aegis
calls this **repossession**, and without it every grant is permanent, so the
kernel has to hedge — hand out less than it could, or refuse a request it
cannot later undo. With it, the kernel can give a LibOS everything that is idle
and take back what it needs later. That is the property SCRUM-147's scheduler
depends on: two LibOSes cannot share one framebuffer and one pool of pages
unless the kernel can move a resource from one to the other.

**The protocol.** Three phases, deliberately in this order — the kernel asks
before it takes, because a LibOS that gets to choose *when* it gives a page
back can flush the state it holds there first:

| Phase | Call | What it does |
| --- | --- | --- |
| 1. Request | `revoke_request(who, res)` | Marks the resource in the ownership table. Nothing else changes: the owner keeps it and keeps using it. |
| 2. Comply | the LibOS's own `exo_page_free`, or releasing the framebuffer | The ordinary return path. The mark disappears with the binding. |
| 3. Force | `revoke_force(who, res)`, or `revoke_all(who)` | The kernel takes what was not returned. |

A request can also be taken back — `revoke_withdraw(who, res)` — for the case
where the demand that prompted it is satisfied elsewhere. A mark that outlives
its reason turns the next sweep into a seizure nobody asked for.

**The mark is an ask, not a seizure.** This is the property everything else
rests on, and both mechanisms enforce it:

- A marked page keeps its owner. `page_owner()` still names the context,
  `free_page_owned()` still lets that context (and only that context) free it,
  and SCRUM-153's `exo_page_map` will still map it.
- A marked framebuffer binding is still held. `fb_binding_check_map()` still
  answers `FB_MAP_ALLOW` for its owner, so a LibOS asked for the screen can
  finish the frame it is drawing before handing it over.

If the mark revoked permission the moment it landed, phase 2 would be
unimplementable — there would be nothing left for the LibOS to return.

**Where the mark lives.** In the ownership table, per §3.3's rule that the
table is the single source of truth about who holds what:

- **Pages** — the top bit of the 16-bit owner tag (`PAGE_OWNER_REVOKED`,
  `0x8000`); the low 15 bits still name the owner, leaving 32,766 context ids
  for SCRUM-147. Keeping it *in* the tag is what makes compliance free:
  `free_page_owned()` resets the tag to `PAGE_OWNER_FREE`, clearing owner and
  mark in one store, so a LibOS that returns a marked page leaves nothing to
  reconcile. Every ownership comparison in `page_alloc.c` masks the bit off
  first — a raw tag compare would read a page under revocation as belonging to
  nobody, and answer `-EPERM` to its own owner.
- **Framebuffer** — a flag beside the single binding in `src/fb_binding.c`,
  cleared by release, reclaim and re-publication. There is one binding to
  describe, so a flag reads better than a masked id at every comparison.

**Forced reclamation is scoped to the context it names.** `revoke_force(who,
res)` and `revoke_all(who)` take a resource only if `who` still holds it. When
the context no longer does — it complied, or the resource has since been
granted to somebody else — the call returns `REVOKE_RETURNED` and touches
nothing. Without that scoping, "revoke" would be a syscall-free way to free a
peer's memory, which is the exact hole §3.3 exists to close.

**Neither sentinel id is a revocable context.** `PAGE_OWNER_KERNEL` and
`PAGE_OWNER_FREE` are refused by *every* page entry point — `page_revoke_mark`,
`page_revoke_clear` and `page_reclaim` through their shared
`owned_page_index()`, and `page_reclaim_all` through its own guard. The refusal
has to come *before* the ownership compare, because each id would otherwise
pass it: `FREE` matches the tag of every free page, and `KERNEL` matches every
reserved one. A single-resource path that skipped this would let
`page_reclaim(kp, PAGE_OWNER_KERNEL)` hand the page bitmap, the owner table or
the kernel image back to the pool and leave the kernel's own `free_page()` to
double-free it — a sweep guard alone is not enough when the force path can name
the same id one page at a time.

**The repossession record.** `revoke_record()` returns the running counts —
asked, withdrawn, forced, returned, pages reclaimed, framebuffers reclaimed.
It is Aegis's repossession vector reduced to what a single-LibOS kernel can act
on: the evidence for whether asking is working, which is the input a real
revocation *policy* needs. Machine-wide today because there is one LibOS;
SCRUM-147 makes it a field of the context structure.

`returned` is the count to read with care. It means "the force step found
nothing to take", which under the protocol is the LibOS having complied — but
it also counts a force whose target had since moved to another context, or one
that named a context which never held the resource. After the fact those are
indistinguishable from the ownership table alone, because a satisfied request
leaves no trace by design (§ the mark clearing with the tag). Good enough as
evidence that asking is working; a policy that acted on the number would first
need per-request state to tell the cases apart.

**What exists today, and what does not.** The mechanism is complete; the policy
is trivial on purpose:

- Nothing on the boot path asks for a resource back. The self-check in
  `kernel_main` walks the whole protocol on a scratch page and the framebuffer,
  which is why it appears in the boot log at all.
- `exo_exit` (`#20`) is not bound yet — that is SCRUM-155, and `revoke_all()`
  is the hook it calls. Until then a LibOS that exits keeps its pages and the
  screen for the rest of the boot.
- **There is no upcall.** Phase 1 marks the resource but cannot *tell* the
  LibOS, because there is no path from the kernel into LibOS code yet. That is
  where Aegis posts to the LibOS's repossession handler, and it is the piece
  SCRUM-147 must add before phase 2 can happen for any reason other than a
  LibOS coincidentally freeing the page. Until then, request-then-force is a
  well-defined sequence with a zero-length wait in the middle.
- **No deadline.** A real protocol gives the LibOS a bounded time to comply
  before the kernel forces the issue. With no upcall there is nothing to time,
  so the deadline arrives with the upcall.
- **One ABA window.** A context that returns a marked page and immediately
  allocates the same frame again gets a fresh, unmarked tag — but a
  `revoke_force` still in flight for that address would find the context
  holding it and take it. Harmless in v1, where force only runs from a context
  that is exiting; SCRUM-147 closes it by generation-stamping the tag.
- **Mapping teardown is done (SCRUM-159).** Taking a resource back in the
  ownership table used to leave the old holder's page-table mapping of it
  live — the kernel considered the resource reclaimed while the previous
  owner could still read and write it, and a shadow framebuffer's pages
  (§3.2 #4) stayed on screen through the old holder after repossession.
  `revoke_force`/`revoke_all` now also clear every mapping `who` has of what
  was reclaimed, via `vmm_unmap_phys_range_in()` (`src/vmm.c`) — a reverse
  scan of `who`'s own LibOS-window PML4 rather than a separate per-context
  mapping record, since post-SCRUM-48 that window is the only place `who`'s
  mappings can live (§3.7). The mark itself (phase 1) is unaffected: a
  marked-but-not-yet-forced resource still changes nothing, per this
  section's own rule.

**No locking**, for the same reason as §3.5, and with a sharper caveat: under
preemption the lock has to span the whole mark-then-reclaim sequence, not each
half, or a resource can change hands between the two.

---

### 3.7 Address-space mapping (SCRUM-35)

§3.3 says a LibOS may only map pages it owns. This section is how the mapping
itself works — the half of `exo_page_map` that is mechanism rather than policy.
Implemented in `src/vmm.c` (the walker) and `src/syscall_mem.c` (the rule),
tested in `tests/kernel/test_vmm_k.c` and `tests/kernel/test_page_map_k.c`.

**What the LibOS is actually editing.** `boot.s` builds one 4 GiB identity map
out of 2 MiB pages and enables paging (SCRUM-15). There is exactly one address
space, and it is the one the kernel is running in: until each LibOS gets its
own PML4 (SCRUM-48), `exo_page_map` edits the kernel's page tables. Two rules
make that safe:

- **The LibOS window.** `vaddr` must lie in
  `[EXO_USER_VA_BASE, EXO_USER_VA_END)` = `[64 TiB, 128 TiB)`, and anything
  else is `-EPERM`. Ownership answers *which physical page*; the window answers
  *where*, and both questions have to be asked — a LibOS must not be able to
  install a page it legitimately owns over kernel text.

  **Why the base is 64 TiB and not 4 GiB.** The kernel map is an *identity* map
  (`vmm_init`, SCRUM-15): every byte of usable RAM is mapped at a virtual
  address equal to its physical one. A window that begins below the top of
  physical memory therefore overlaps live kernel mappings. A 4 GiB base looked
  correct against `boot.s`'s 4 GiB map and was wrong the moment SCRUM-15 began
  mapping all usable RAM: on an 8 GiB machine, `exo_page_map` in the window hit
  `VMM_EEXIST` (the address was already taken) and `exo_page_unmap` cheerfully
  unmapped the kernel's own RAM, because that page is unallocated and so
  `PAGE_OWNER_FREE`. The base has to be an address physical memory cannot
  reach; `syscall_mem_init()` checks that against the multiboot map at boot and
  warns rather than assuming it, and
  `tests/kernel/test_page_map_k.c:test_window_is_clear_of_kernel_mappings`
  asserts both the invariant and its observable consequence.
- **Page tables are kernel-owned.** Intermediate PDPT/PD/PT pages come from
  `alloc_page()`, i.e. `PAGE_OWNER_KERNEL`, so a LibOS cannot hand the page
  describing its own address space back to the PMM with `exo_page_free` and
  then watch the kernel write into a page somebody else now owns.

**Splitting 2 MiB pages.** A mapping request is 4 KiB granular and the boot map
is not, so a `vaddr` covered by a 2 MiB page is handled by first replacing that
page with a 512-entry PT reproducing it exactly — same physical range, same
flags — and only then editing the one entry. The other 511 mappings survive
untouched, which is what lets the kernel keep running while its own identity
map is being edited under it. A split allocates, which is why even
`exo_page_unmap` can answer `-ENOMEM`. TLB maintenance follows the same shape:
`invlpg` for an ordinary change, a full CR3 reload after a split, because the
stale entry there is a 2 MiB translation rather than the single page `invlpg`
names.

**Replacing and removing a mapping.** Mapping over an existing mapping is
allowed — it is how a LibOS moves a window over physical memory — but the page
being displaced must be one the caller could have unmapped itself, or "map over
it" would be a way around `exo_page_unmap`'s check. That check is deliberately
one notch looser than the check on the page being *installed*: what it refuses
is a page that currently belongs to somebody else, not every page the caller
could not map. Dropping a mapping changes an address space, not a page, so it
cannot hurt the page's owner, and two cases make the looser rule necessary:

- A page owned by **nobody** — §3.2 #3 lets a LibOS free a page that is still
  mapped, and it must then be able to clean up the mapping it left behind.
- **Framebuffer memory the caller no longer holds** — a LibOS that acquires the
  framebuffer, maps it, and then loses the binding (to revocation, §3.6, or to
  its own release) would otherwise be stuck with an address it can neither
  unmap nor reuse for the rest of its life.

A page belonging to the kernel or to another context is still refused.

**What is deliberately not done yet.**

- **No NX.** `EFER.NXE` is not enabled, so every mapping is executable and
  `EXO_PAGE_EXEC` is accepted and ignored. The flag exists in the ABI now so
  that enabling NXE later is a kernel change rather than a renumbering.
- **No page-table reclamation.** An intermediate table that becomes empty is
  kept. Walking a table on every unmap to discover it is empty costs more than
  the page is worth at v1 scale; the bound is one PT per 2 MiB of address space
  a LibOS has ever touched.
- **Per-LibOS address spaces exist (SCRUM-48).** Each context can have its own
  PML4 (`vmm_create_address_space`/`vmm_bind_address_space`), sharing only
  PML4[0] — the kernel's own subtree — with every other one, so the window is
  now a per-context policy rather than the single global one this bullet used
  to describe: two LibOSes each get their own private copy of it and cannot
  reach each other's mappings at all, let alone unmap them.
- **Mapping teardown is done (SCRUM-159).** The kernel still keeps no
  per-context *record* of what a context has mapped — nothing appends to a
  list on `exo_page_map`. What closes the gap instead is
  `vmm_unmap_phys_range_in()` (`src/vmm.c`): a reverse scan of one context's
  own LibOS-window PML4 for every leaf pointing at a given physical range (or
  at anything at all, for a full sweep), safe to do on demand because
  post-SCRUM-48 that window is the only place the context's mappings can
  live. `exo_page_free` uses it on its own caller to unmap what it just freed
  before the PMM can hand the frame to somebody else; `revoke_force`/
  `revoke_all` (§3.6) use it on the target context, so repossessing a
  resource — a shadow framebuffer's pages included — actually revokes write
  access to it rather than only updating the ownership table.
- **Page-table pages are quota-bounded per context (SCRUM-160), not
  reclaimed and not reassigned.** Every level `vmm.c` allocates is still a
  `PAGE_OWNER_KERNEL` page no sweep reclaims (unchanged — see `vmm.c`'s own
  file header: never switch these to `alloc_page_owned()` with a LibOS id,
  or a LibOS could `exo_page_free` a page table out from under its own
  address space), and `vmm_unmap_page()` still never reclaims an empty one
  (the bullet above this one). What closes the actual exhaustion gap is a
  *quota*, not either of those: `exo_page_map` (`sys_page_map`,
  `src/syscall_mem.c`) calls `vmm_map_page_in_owned()` instead of
  `vmm_map_page_in()`, which charges every table page it causes
  `alloc_table()` to create against the caller's own slot in `vmm.c`'s
  per-context address-space registry (`addrspace_binding_t.table_pages_charged`).
  Once a context's charge reaches `VMM_MAX_TABLE_PAGES_PER_CONTEXT` (64,
  `src/vmm.h`), `alloc_table()` refuses — before calling `alloc_page()`, so
  hitting the quota consumes no page — and the failure surfaces as the same
  `-EXO_ENOMEM` real PMM exhaustion already produces. The charge resets to
  0 whenever `vmm_bind_address_space()` (re)points that slot at a new
  `pml4_phys`, which is every real relaunch (`libos_build_image()` always
  builds a fresh address space first), so a LibOS gets a clean quota each
  time it starts, not a debt inherited from a previous life of the same
  context id.

  64 is sized against the largest legitimate ring-3 caller of `exo_page_map`
  known today — `libos_fb_map()` (`src/libos_fb.c`), which the shell calls
  once at startup to map the acquired framebuffer a page at a time, a
  handful of PT pages for a real VESA mode — with several times that much
  headroom, while still capping a single context's worst-case table growth
  at 64 pages (256 KiB) rather than unbounded. Kernel-internal callers of
  `vmm_map_page_in()` — the boot identity map, `libos_launch.c`'s image
  loader, `libos_wad_map.c` — are untouched: they keep going through the
  unowned/unlimited path (equivalent to passing `PAGE_OWNER_KERNEL`), exactly
  as before this ticket, because each is already bounded by its own
  fixed-size window (`LIBOS_LAUNCH_MAX_CODE_PAGES`/`_DATA_PAGES`/
  `LIBOS_LAUNCH_STACK_PAGES`, `LIBOS_WAD_MAX_BYTES`). `exo_page_unmap` is
  also untouched — the split it can trigger stays unowned/unlimited too,
  since unmapping was never the exhaustion vector this ticket closes.
- **A page fault reports, from either ring, but nothing recovers.**
  SCRUM-17 put a real handler on vector 14: an access to an unmapped address
  now yields CR2, the decoded error code, the faulting RIP and the live
  mapping state on COM1 instead of a silent loop in `error_stub`. A CPL 3 →
  CPL 0 exception needs `TSS.RSP0`; SCRUM-46 loads a TSS with a valid one, so
  a ring-3 fault reaches the handler and reports instead of triple-faulting
  (`tests/kernel/test_tss_k.c` drives one for real, and
  `tests/kernel/test_libos_launch_k.c` drives one from a genuine, separate
  LibOS address space rather than the kernel's own). What is still missing is
  policy on top of that report: the handler halts either way today.
  Terminating a faulting LibOS and reclaiming its resources instead still
  needs building, now that SCRUM-47/48 supply the address space and launch
  mechanism it would act on.

### 3.8 Syscall round-trip benchmarks (SCRUM-60)

Blocked on SCRUM-33 (`exo_get_ticks`, the first end-to-end syscall) until it
landed; six more syscalls have since been bound, so
`tests/kernel/test_syscall_bench_k.c` covers `exo_page_alloc`/`_free`
(#0/#1), `exo_page_map`/`_unmap` (#2/#3), `exo_fb_acquire` (#4),
`exo_get_ticks` (#5), `exo_kbd_poll` (#6) and `exo_serial_write` (#8), plus
the dispatcher's bare `-EXO_ENOSYS` floor via the still-unbound
`exo_mouse_poll` (#7) — a "no handler work at all" baseline the others sit
on top of.

**Method.** Each case is a real ring-3 `syscall`/`sysretq` round trip, not an
in-kernel call to `exo_syscall_dispatch()` — that would skip exactly the
entry/exit cost this ticket wants. `tests/kernel/syscall_bench_probe.s`
loops the syscall under test back to back inside a real launched LibOS image
(`libos_build_image()`/`libos_enter()`, §3.7's launch mechanism, *not*
`tests/kernel/ring3_probe.s`'s older `ring3_run()` — `src/vmm.c` (SCRUM-55)
re-exposes only that file's own two probes as user-executable, "the sole,
explicitly-scoped legacy exception"), timing the whole run with `rdtsc`
(`CR4.TSD` is never set, so it executes fine at CPL 3) and reporting cycles
rather than wall time — there is no calibrated clock to convert against, and
the ticket asks for cycles regardless. `exo_page_alloc`/`_free` and
`exo_page_map`/`_unmap` run as alternating pairs (alloc immediately freed,
map immediately unmapped) so the PMM and the test's own mapping end exactly
where they started; the reported number is the pair's total cycles divided
by 2.

**Baseline (QEMU/TCG, `make docker-test`, 5000 iterations for the four
argument-only cases and the ENOSYS floor, 2000 for the alloc/free and
map/unmap pairs):**

| Syscall                          | Cycles/call | Notes |
| --------------------------------- | ----------: | ----- |
| Dispatcher `-ENOSYS` floor (#7)   |         266 | No handler bound; range-check + return only. |
| `exo_get_ticks` (#5)              |         225 | No args, no memory touched. |
| `exo_serial_write(len=0)` (#8)    |         216 | `len==0` returns before touching `buf` or COM1. |
| `exo_kbd_poll` (empty ring) (#6)  |         266 | Window check only; ring empty, `event_out` untouched. |
| `exo_fb_acquire` (re-acquire) (#4)|        1385 | Writes a 24-byte `exo_fb_info_t`; re-acquire is idempotent. |
| `exo_page_map`+`exo_page_unmap` (#2/#3) | 943 | Per syscall, averaged over the pair; edits real page tables. |
| `exo_page_alloc`+`exo_page_free` (#0/#1) | 32914 | Per syscall, averaged over the pair; PMM bitmap scan dominates. |

These are QEMU/TCG numbers on the machine that ran `make docker-test` at
the time this table was written, not a hardware measurement — `rdtsc`
reflects the *host's* real clock under TCG emulation, which varies across
developer machines and CI runners, so treat the relative shape (dispatcher
floor ≈ 220-270 cycles; `exo_page_alloc` two orders of magnitude above
everything else) as the durable finding, not the absolute counts. The
`CU_ASSERT`s in `test_syscall_bench_k.c` are loose sanity bounds (nonzero,
under 10,000,000) for exactly this reason — a regression gate on exact
cycle counts would be flaky by construction on this kernel's only timing
source. `exo_page_alloc`'s cost is the standout: `src/page_alloc.c`'s
bitmap PMM does a linear scan for a free page, an order of magnitude above
`exo_page_map`'s page-table edit and nearly 150x the dispatcher floor —
worth keeping in mind for SCRUM-117's eventual performance report if
anything above this layer starts allocating pages in a hot loop.

---

## 4. Architectural decision: file I/O strategy

> **Superseded in part (2026-09-19 team decision, SCRUM-189).** The
> "recommended approach" at the end of this section — a minimal ramdisk
> filesystem *in the kernel*, exposed through `exo_file_*` — is no longer
> what the port does, and implementing it now would be a step backwards.
> Storage moved to a real device-backed model: the kernel knows sectors and
> nothing above them (`exo_disk_read`/`exo_disk_write`, §3.2 #27/#28), and a
> **LibOS-space** filesystem decides what a file is (`src/libos_fs/`,
> `docs/filesystem.md`). Putting a filesystem in the kernel would put policy
> back where this project exists to keep it out of.
>
> What still holds: the three *use cases* below (WAD, config, saves) and
> their differing requirements, which is why they are kept. `exo_file_*`
> (#9–16) remains the planned syscall surface for SCRUM-44, but it will be
> implemented on top of the LibOS filesystem rather than on a kernel
> ramdisk. §4.1's memory-mapped WAD shortcut is what `fopen` does today
> (SCRUM-65) and is itself due to be retired by SCRUM-190, which moves the
> IWAD onto the disk and drops the multiboot module.

Doom uses `FILE*` I/O for three distinct purposes. Each can be handled
differently.

### 4.1 WAD file reading (`w_file_stdc.c`)

The WAD is loaded as a GRUB multiboot module and is already in physical memory.

- **Option A (recommended):** Replace `w_file_stdc.c` with a memory-mapped
  reader that returns pointers into the module's mapped memory.
  `fopen("freedoom2.wad")` returns a fake `FILE*` backed by the in-memory WAD
  data. `fseek`/`fread` operate on offsets into the mapped region. This avoids
  any actual file I/O for the largest data source.
- **Option B:** Funnel through `exo_file_*` syscalls with the kernel maintaining
  a "virtual file" backed by the multiboot module memory.

### 4.2 Config files (`m_config.c`)

Doom reads/writes `default.cfg` for key bindings, video settings, etc. This is a
small text file (< 4 KB). A ramdisk is sufficient initially (config lost on
reboot). For persistence, the ATA driver can store it in a reserved disk sector.
Goes through `exo_file_*` syscalls.

### 4.3 Save games (`g_game.c`)

Save files are ~200 KB each. Doom supports 6 save slots (`doomsav0.dsg` through
`doomsav5.dsg`) plus temp/recovery files. This needs real read/write/seek
semantics.

- Ramdisk-backed for **Sprint 2: VMem + Input + libc** (config files),
  transitioning to ATA-backed in a later sprint once block storage is
  implemented.
- Goes through `exo_file_*` syscalls.
- The save path also uses `remove()` and `rename()` for safe file rotation.

**Recommended approach (historical — see the note at the head of §4):**
Implement a minimal ramdisk filesystem in the kernel (flat list of named files
with read/write/seek), exposed through `exo_file_*` syscalls. The LibOS libc
shim translates `FILE*` to file descriptors. WAD reading uses the
memory-mapped shortcut. Later, add ATA persistence behind the same syscall
interface.

**What the port actually does instead:** the ATA driver (SCRUM-102) and
`exo_disk_read`/`exo_disk_write` (SCRUM-103) give the kernel sector access
and no more; ExoFS (SCRUM-189, `src/libos_fs/`, `docs/filesystem.md`) is a
FAT-like filesystem linked as LibOS code on top of those syscalls; and
`exo_file_*` / the `FILE*` shim (SCRUM-44/42) will sit on ExoFS. The libc
shim still translates `FILE*` to descriptors, exactly as above — only the
layer underneath it changed sides of the kernel boundary.

---

## 5. Memory allocation pattern

Doom's memory usage is well-defined and predictable.

**Zone allocator:** `I_ZoneBase()` calls `malloc(6 * 1024 * 1024)` once to get 6
MiB. All game-internal allocations (level data, sprites, sounds, intermission
screens) go through `Z_Malloc` which manages this zone internally. If 6 MiB
fails, Doom retries with decreasing sizes down to the `MIN_RAM` of 6 MiB (then
crashes). On a 256 MiB QEMU VM this will never fail.

**Screen buffer:** `doomgeneric_Create()` calls `malloc(640 * 400 * 4)` =
1,024,000 bytes for `DG_ScreenBuffer`.

**Scattered small allocs:** ~20 direct `malloc` calls for strings, paths, config
values, temp buffers. Largest is ~4 KB. Total < 100 KB.

**Total LibOS heap requirement:** ~8 MiB is safe. This means the LibOS page
allocator needs about 2,048 pages (8 MiB / 4 KB) from the exokernel's physical
page pool.

---

## 6. Sound architecture

When `FEATURE_SOUND` is not defined (the default for non-SDL builds), the
`sound_module` pointer is `NULL` and all `I_StartSound`/`I_StopSound` calls are
no-ops. This means Doom will run silently with zero sound code.

To add PC speaker sound, there are two options:

**Option A (minimal):** Keep `FEATURE_SOUND` undefined. Doom runs silently. No
sound syscalls needed.

> **This is what the port does, and it needed no code (SCRUM-82).** The
> vendored tree ships `FEATURE_SOUND` undefined already
> (`src/doom/doomfeatures.h`), so `sound_modules[]` collapses to `{ NULL }`,
> `InitSfxModule()`/`InitMusicModule()` leave both module pointers `NULL`, and
> every `I_*` entry point in `src/doom/i_sound.c` takes its existing
> null-pointer branch: `0` from `I_StartSound`/`I_GetSfxLumpNum`, `false` from
> `I_SoundIsPlaying`, nothing from `I_StopSound`/`I_UpdateSound`. `s_sound.c`
> is untroubled by that -- it stores a handle that is never played, and
> `I_SoundIsPlaying(0)` answering `false` simply retires the channel. So
> SCRUM-82's acceptance holds without stubbing anything, and stubbing would
> have meant editing vendored source (which SCRUM-63 exists to avoid) to
> replace working code with identical behaviour.
>
> What SCRUM-82 did add is the thing that makes it stay true: a gate at the
> end of `docker/scripts/build-doom.sh` that reads `build/doom/i_sound.o` and
> fails the build if `DG_sound_module`/`DG_music_module`/`Mix_*`/`SDL_*` ever
> go undefined there, or if any of `I_StartSound`/`I_StopSound`/`I_UpdateSound`
> stops being defined (`s_sound.c` calls all three unconditionally).
>
> It covers the quiet half of the regression. Defining `FEATURE_SOUND` on its
> own is already loud -- `i_sound.c` includes `<SDL_mixer.h>` under the same
> guard and the compile pass dies there. But define it with that include
> bypassed (`-D__DJGPP__`, or an `SDL_mixer.h` appearing on the include path)
> and the file compiles cleanly while leaving both module symbols undefined;
> a compile-only pass says nothing, and the first sign would be an undefined
> symbol during SCRUM-66's link.

**Option B (PC speaker):** Implement a `sound_module_t` with
`Init`/`StartSound`/`StopSound`/`Update` that maps Doom SFX lump data to PC
speaker tone frequencies and calls `exo_sound_tone`/`exo_sound_stop`. Define
`FEATURE_SOUND` and register the module. The mapping from Doom's 8-bit PCM sound
lumps to single-frequency tones is lossy but recognizable.

---

_This specification was generated by analyzing the doomgeneric repository at
[github.com/ozkl/doomgeneric](https://github.com/ozkl/doomgeneric) via automated
grep-based static analysis of 82 core C source files. Function call counts are
approximate (grep matches include declarations, comments, and string literals in
some cases) but representative of actual usage patterns._
