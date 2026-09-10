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
   - [3.3 Secure binding & resource ownership](#33-secure-binding--resource-ownership)
   - [3.4 Entry path](#34-entry-path-scrum-32)
   - [3.5 Framebuffer binding](#35-framebuffer-binding-scrum-154)
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

The following is an exhaustive list of every standard C library function called
by doomgeneric's 82 core source files, with call counts from static grep
analysis. This determines what the freestanding libc shim must provide.

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

| Function  | Calls    | Notes                                                                                         |
| --------- | -------- | --------------------------------------------------------------------------------------------- |
| `free`    | 153      | Most calls are `Z_Free` (internal zone). ~20 are direct libc `free()`.                        |
| `exit`    | 31       | Called on fatal errors. Implement as halt loop.                                               |
| `malloc`  | 21       | One 6 MiB zone alloc + ~20 small allocs (strings, paths, structs).                            |
| `abs`     | 30       | Integer absolute value. Trivial macro.                                                        |
| `atoi`    | 14       | String to integer. Used for config/command-line parsing.                                      |
| `atof`    | 2        | String to float. Used only for mouse acceleration config. Can return `1.0` as stub.           |
| `atexit`  | 4        | Register cleanup functions. Implement as linked list (Doom already does this via `I_AtExit`). |
| `getenv`  | 3        | Returns `DOOMWADPATH`/`DOOMWADDIR`. Return `NULL` — WAD is a multiboot module.                |
| `system`  | 13       | All behind `#ifdef` guards (Zenity error boxes). Stub as `return -1`.                         |
| `realloc` | 3        | Resize allocation. Implement as `malloc` + `memcpy` + `free`.                                 |
| `calloc`  | 2        | `malloc` + `memset(0)`. Trivial wrapper.                                                      |
| `abort`   | 2        | Abnormal termination. Implement as halt loop.                                                 |
| `qsort`   | 0 direct | Not called directly but may be pulled in. Implement a simple quicksort.                       |

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
> the numbers as `EXO_SYS_*`, the shared argument structs, the `EXO_E*` error
> codes, and one inline stub per syscall. It is the same interface expressed in
> C, and the two must be changed together — this document stays the source of
> truth for *what* each syscall does. Kernel sources are compiled with
> `-DEXO_KERNEL`, which suppresses the user-side stubs; the LibOS includes it
> plainly. The define comes from the compiler command line rather than a
> per-file `#define` because `#pragma once` would make a `#define` placed after
> any transitive include of the header silently ineffective. `tests/kernel/test_exo_syscall_k.c` asserts the header still agrees
> with §3.1 and §3.2.

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
| 1  | `exo_page_free(paddr)`              | Memory      | ✅     | Free a physical page. Bound to the dispatcher (SCRUM-34); currently `0` or `-EINVAL`. **Ownership-checked (§3.3)** — will return `-EPERM` unless the caller owns `paddr` once the page ownership table (SCRUM-152) lands.                  |
| 2  | `exo_page_map(vaddr, paddr, flags)` | Memory      | ✅     | Map physical page at virtual address in caller's address space. `flags`: `EXO_PAGE_READ`/`WRITE`/`USER`/`EXEC`. `EXEC` is accepted and ignored until `EFER.NXE` is enabled; `READ` is not representable on x86 (present implies readable) and is accepted and ignored. **Ownership-checked (§3.3):** `paddr` must be owned by the caller (or be the framebuffer the caller has acquired), and `vaddr` must lie in the LibOS window `[EXO_USER_VA_BASE, EXO_USER_VA_END)` — §3.7. Returns `0`, `-EINVAL` (misaligned address, unknown flag bit), `-EPERM` (window or ownership) or `-ENOMEM` (no page for an intermediate page table). Implemented in SCRUM-35 (`src/syscall_mem.c`, `src/vmm.c`), enforcement SCRUM-153. |
| 3  | `exo_page_unmap(vaddr)`             | Memory      | ✅     | Unmap a virtual page; the physical page stays allocated (`exo_page_free` returns it). Only unmaps a mapping of a page the caller owns, holds the FB binding for, or that belongs to nobody — §3.7. Returns `0`, `-EINVAL` (misaligned, or nothing mapped there), `-EPERM` or `-ENOMEM`. Implemented in SCRUM-35, enforcement SCRUM-153. |
| 4  | `exo_fb_acquire(info_out)`          | Framebuffer | ✅     | Write framebuffer info (`phys_addr`, `width`, `height`, `pitch`, `bpp`) to `info_out` struct and **record the caller as the framebuffer owner (secure binding, §3.3, §3.5)**. LibOS then calls `exo_page_map` to map it — that map requires FB ownership. Released on `exo_exit`. Used by `DG_Init`. Returns `0` (including a re-acquire by the current owner), `-EBUSY` if another LibOS holds the FB, `-EFAULT` for an unusable `info_out`, or `-ENODEV` on a machine the bootloader gave no framebuffer. Implemented in SCRUM-154 (`src/syscall_fb.c`, `src/fb_binding.c`); the LibOS-side mapping of the returned range still waits on SCRUM-16/-35. |
| 5  | `exo_get_ticks()`                   | Timer       | ✅     | Return `uint32_t` milliseconds since boot. Zero arguments. Used by `DG_GetTicksMs` and `DG_SleepMs`. Kernel-side PIT + `kernel_get_ticks_ms()` done (SCRUM-9, -10).                                                                                                            |
| 6  | `exo_kbd_poll(event_out)`           | Input       | 🔄     | Dequeue next keyboard event into `event_out` struct `{uint8_t pressed; uint8_t key; uint8_t modifiers; uint8_t reserved}`. `key` is a decoded `ps2_key_t` index (`KEY_A`, `KEY_ESC`, …), not a raw PS/2 scancode — the kernel's scancode decoder runs before the event is queued. `modifiers` is the `EXO_MOD_*` shift/ctrl/alt mask sampled when the event was queued, so a chord decodes correctly even if the modifier is released before the LibOS polls — Doom binds shift (run), ctrl (fire) and alt (strafe). Returns `1` if event available, `0` if empty. Prerequisite: IRQ1 handler (SCRUM-13, In Progress) + scancode table (SCRUM-14, In Progress). Ring buffer planned Sprint 2 (SCRUM-18). |
| 7  | `exo_mouse_poll(state_out)`         | Input       | ⬜     | Write accumulated mouse state `{int16_t dx; int16_t dy; uint8_t buttons; uint8_t reserved}` to `state_out`, then reset accumulators. `reserved` is zeroed by the kernel and keeps the struct a fixed 6 bytes. Returns `0`. Prerequisite: PS/2 mouse init (SCRUM-19, Sprint 2).                                                                                            |
| 8  | `exo_serial_write(buf, len)`        | Debug       | ⬜     | Write `len` bytes from `buf` to COM1. Returns bytes written. Used by `printf`/`fprintf` shim. Validates `buf` is in user address space. Kernel serial driver exists; syscall gate not yet wired. `printf` shim planned Sprint 2 (SCRUM-20).                                    |
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
| 19 | `exo_yield()`                       | Scheduling  | ⬜     | Cooperatively yield CPU to next runnable LibOS context. Returns when rescheduled. Used by shell LibOS and optionally by `DG_SleepMs`.                                                                                                                                          |
| 20 | `exo_exit(code)`                    | Lifecycle   | ⬜     | Terminate calling LibOS. Frees all pages, closes all files, removes from scheduler. Does not return.                                                                                                                                                                           |

**Total: 21 syscalls.** This is the complete interface needed to run Doom with
save/load, config, sound, and cooperative multitasking.

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
- `exo_fb_acquire` binds the framebuffer to one LibOS at a time (`-EBUSY`
  otherwise); mapping FB physical pages requires holding that binding
  (SCRUM-154, §3.5). Framebuffer pages need a table of their own rather than
  the PMM's owner tags, because MMIO sits outside the usable-RAM region the
  page allocator manages — `page_owner()` reports `FREE` for every one of
  them, so the generic check above cannot speak for them.
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
one CPU. When SCRUM-107 introduces multiple LibOS contexts this becomes
`swapgs` plus a per-CPU block reached through `IA32_KERNEL_GS_BASE`.

No TSS is involved: `syscall` never consults `TSS.RSP0`. SCRUM-46 is needed
before ring-3 code can take an *interrupt*, not before it can make a syscall.

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
benefit. This opens all of physical memory to CPL 3 and is scoped to test
builds for that reason; SCRUM-48 (per-LibOS page directories) and SCRUM-55/-56
(isolation tests) close it properly.

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

### 3.6 Revocation & repossession (SCRUM-156)

§3.3 states that the kernel may take a granted resource back. This section is
the design note for how — the protocol in full, what of it exists today, and
what the multi-LibOS work still has to add. Implemented in `src/revoke.c` (the
protocol), `src/page_alloc.c` and `src/fb_binding.c` (the per-resource
mechanisms), and tested in `tests/kernel/test_revoke_k.c`.

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
- **No per-LibOS address space.** With one PML4, the window is what separates a
  LibOS from the kernel, and nothing separates two LibOSes from each other:
  they would share the window and could unmap each other's mappings of unowned
  pages. SCRUM-48 makes `vmm.c`'s implicit "current PML4" a parameter, at which
  point the window becomes a per-context policy rather than a global one.
- **The kernel does not know where a context mapped anything.** Nothing records
  a context's mappings, so nothing can tear them down: `exo_page_free` leaves a
  live PTE pointing at a page the PMM may hand to somebody else, and
  repossessing the framebuffer (§3.6) clears the binding while the old holder's
  mapping keeps writing to the screen. Both need per-context address-space
  tracking, which arrives with SCRUM-48; neither is reachable before a LibOS
  runs in ring 3 (SCRUM-47).
- **No quota on page tables.** Every level `vmm.c` allocates is a
  `PAGE_OWNER_KERNEL` page that no sweep reclaims, and a caller can walk the
  128 TiB window installing one mapping per 2 MiB to consume them without
  bound. Harmless while the only caller is the kernel itself; a per-context
  quota is required before ring 3 can reach it.
- **No page fault handler.** A LibOS that touches an address it never mapped
  faults into the `error_stub` from SCRUM-135 rather than into a diagnostic.
  SCRUM-17 is what turns "unmapped access faults cleanly" from true-by-halt
  into true-by-report.

---

## 4. Architectural decision: file I/O strategy

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

**Recommended approach:** Implement a minimal ramdisk filesystem in the kernel
(flat list of named files with read/write/seek), exposed through `exo_file_*`
syscalls. The LibOS libc shim translates `FILE*` to file descriptors. WAD
reading uses the memory-mapped shortcut. Later, add ATA persistence behind the
same syscall interface.

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
