# ExoDoom: exokernel architecture

**Audience:** the client, and anyone reviewing ExoDoom's design without
reading its code. It covers the architecture, the system-call interface, the
isolation guarantees and the reasons behind the main design decisions. The
team's working documents (`docs/architecture.md`, `docs/syscall_spec.md` and
the rest of `docs/`) carry the full detail and the history; each section here
ends with a pointer to the one that goes deeper.

Describes `main` as of 26 October 2026.

---

## 1. Summary

ExoDoom is a small operating-system kernel for 64-bit x86 PCs, written from
scratch in C and assembly, that boots on real hardware or QEMU and runs the
original Doom engine with no conventional operating system underneath it.

It is built as an **exokernel**. The kernel protects the machine's
resources (memory pages, the screen, disk sectors, sound voices, the CPU) and
hands them out. It does **not** decide how they are used. Everything a
conventional kernel would decide for a program instead lives in a library that
the program links in, a **library operating system (LibOS)**. A file system,
a memory allocator, a text console and the shell's program-admission policy
are all LibOS code in ExoDoom, not kernel code.

| | |
|---|---|
| Kernel | freestanding C and x86-64 assembly, no external libraries; 935 KiB image |
| System calls | 41 numbers, 33 bound; the rest are reserved and deliberately unbound (§5) |
| Programs | a shell, Doom, a WAD/map viewer, a clock, Snake, a live resource monitor (`top`), and a filesystem isolation demo, all ring-3 LibOS programs |
| Doom | the unmodified doomgeneric engine on a 6-function platform layer and a C library shim |
| Multitasking | up to 15 programs at once, switched cooperatively; Ctrl+Tab brings another to the foreground |
| Tests | about 1,100 tests, run inside the kernel on every boot of a test build, plus a 1,000,000-call random-argument syscall fuzzer |

---

## 2. The exokernel idea, applied

A conventional kernel abstracts the hardware behind rich interfaces (files,
processes, sockets) and makes the policy decisions behind them. An exokernel
separates **protection** from **management**. The kernel does only what must
be trusted: keep each program to the resources it was given, and get those
resources back when the program is done. A program's LibOS manages those
resources however that program needs.

| Decision | Conventional kernel | ExoDoom |
|---|---|---|
| Which physical page backs a virtual address | kernel | the LibOS asks for a page, then maps it where it wants |
| What a file is | kernel file system | ExoFS, a LibOS library; the kernel knows only sectors |
| How the heap grows | kernel `brk`/`mmap` | the LibOS heap pulls pages one at a time |
| Whether a program can be admitted | kernel | the shell compares the program's stated needs against free resources the kernel reports |
| What is drawn on screen | kernel console or window system | each program draws into its own framebuffer; the kernel only shows the foreground one |
| How sound effects are mixed into channels | kernel or sound server | Doom's LibOS picks voices, volumes and panning; the kernel mixes and plays |

The kernel stays small and its rules stay few, and a program can do something
unusual (Doom's custom music synthesiser, for example) without a kernel
change.

---

## 3. System architecture

![System architecture: ring 3 LibOS programs above the syscall gate, the ring 0 exokernel below it, hardware at the bottom](diagrams/01-system-architecture.svg)

Four layers, top to bottom:

- **Programs (ring 3).** The shell and the applications. Each one is linked
  with the LibOS pieces it needs and runs in its own address space.
- **LibOS (ring 3).** Shared library code linked into each program:
  - a page allocator and heap over the page syscalls;
  - framebuffer mapping and a text console;
  - ExoFS, the file system, and an `exo_file_*` descriptor layer above it;
  - a freestanding C library (`string`, `stdio` including `FILE*`,
    `stdlib`, `ctype`, fixed-point maths);
  - for Doom only: the doomgeneric platform functions, the sound and music
    modules, and an OPL3 FM synthesiser for music.
- **The syscall gate.** The `syscall`/`sysretq` instructions are the only way
  into the kernel. Every pointer and number passed in is checked before use.
- **Kernel (ring 0).**
  - Memory: physical page allocator with per-page owner tags, per-program
    page tables.
  - Interrupts and the timer: IDT, PIC, PIT at 1 kHz.
  - Devices: framebuffer, PS/2 keyboard and mouse, USB (UHCI with HID
    keyboards), ATA disks, Intel HDA audio with a software mixer, PC speaker,
    CMOS clock, PCI, ACPI tables, serial.
  - Protection: the binding tables (§6) and the revocation mechanism.
  - Programs: the context table, which holds each running program's saved
    state, and the program loader.

How a program starts: the shell reads a **package** (`.exo`) from the disk.
The package's manifest states the program's minimum and recommended memory,
its sound voices and whether it wants its own disk volume. The shell runs the
admission check. If the program fits, the kernel builds its address space,
assigns its resources and switches to it.

→ `docs/architecture.md` §3–§7, `docs/diagrams/README.md`.

---

## 4. Boot and memory

### Boot

![Boot sequence](diagrams/06-boot-sequence.svg)

GRUB loads the kernel (Multiboot 2) and sets a graphics mode. The kernel then:
1. Builds its own page tables and enters 64-bit mode.
2. Initialises memory, interrupts, the syscall gate, timers and devices.
3. Draws a boot log, runs a self-check, and launches the shell as the first
   ring-3 program.

From there everything the user sees is LibOS code. On a fresh disk the shell
offers to format it, which is the "new install" path.

### Memory

![Memory layout](diagrams/02-memory-layout.svg)

- **The kernel maps all of RAM 1:1** (virtual address = physical address).
  That mapping is supervisor-only, so no program can touch it. Page 0 is left
  unmapped to catch null pointers.
- **Every program has its own page tables.** Its code, data, stack, heap and
  framebuffer live in a window that starts at 64 TiB, above any address the
  kernel uses. A program can only place pages there, so it cannot map over
  kernel memory even by accident.
- **Every physical page has an owner tag**: free, kernel, or one program. The
  tag is what the protection rules in §6 check.
- **The stack has a guard page** below it, so an overflow faults instead of
  corrupting data.

→ `docs/memory.md`, `docs/syscall_spec.md` §3.7.

---

## 5. System-call interface

### Convention

A program loads the call number into `RAX` and up to six arguments into
`RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9`, then executes `syscall`. The result
comes back in `RAX`:
- A **negative** value is an error code. The codes use Linux `errno` numbers,
  so a C library can pass them straight through.
- A **non-negative** value means success. It is usually `0`, but some calls
  return the answer itself: a page address, a tick count, or a byte or sector
  count.

The kernel preserves every register except `RAX`, `RCX` and `R11`.

### Table

| # | Call | What it does |
|---|---|---|
| **Memory** |||
| 0 | `exo_page_alloc()` | Allocate one physical page, owned by the caller; returns its physical address |
| 1 | `exo_page_free(paddr)` | Free a page the caller owns (and unmap it from the caller) |
| 2 | `exo_page_map(vaddr, paddr, flags)` | Map a page the caller owns into its own address-space window |
| 3 | `exo_page_unmap(vaddr)` | Remove a mapping; the page stays allocated |
| **Screen and input** |||
| 4 | `exo_fb_acquire(info_out)` | Give the caller its own framebuffer (same size as the screen) to map and draw into |
| 6 | `exo_kbd_poll(event_out)` | Take the next keyboard event (non-blocking) |
| 7 | `exo_mouse_poll(state_out)` | Read and reset accumulated mouse motion and buttons |
| **Time** |||
| 5 | `exo_get_ticks()` | Milliseconds since boot |
| 35 | `exo_rtc_read(out)` | Calendar date and time from the CMOS clock |
| **Sound** |||
| 17 | `exo_sound_tone(freq, ms)` | Play a tone on the PC speaker |
| 18 | `exo_sound_stop()` | Silence the caller's speaker tone |
| 28 | `exo_sound_pcm(buf, n, rate, vol, sep, prio)` | Play an 8-bit sample on one of 8 mixer voices; returns a handle |
| 29 | `exo_sound_pcm_stop(handle)` | Stop a voice the caller started |
| 30 | `exo_sound_pcm_params(handle, vol, sep)` | Change a playing voice's volume and stereo position |
| 32 | `exo_sound_stream_write(buf, frames)` | Queue 16-bit stereo music onto the single music stream (binds it to the caller) |
| 33 | `exo_sound_stream_stop()` | Release the music stream |
| 34 | `exo_sound_stream_volume(vol)` | Set the music level |
| **Storage** |||
| 24 | `exo_disk_read(dev, lba, buf, n)` | Read sectors from a disk range the caller holds |
| 25 | `exo_disk_write(dev, lba, buf, n)` | Write sectors to a disk range the caller holds (refused on a read-only binding) |
| 26 | `exo_disk_acquire(dev, base, count)` | Bind a range of sectors to the caller; ranges of different programs never overlap |
| 27 | `exo_disk_release(dev)` | Give a disk binding back |
| 31 | `exo_diskstat(dev, out)` | Whether a disk exists, and its size |
| 37 | `exo_disk_extent(out)` | The disk volume the caller's package was assigned at launch |
| **Programs** |||
| 19 | `exo_yield()` | Let another ready program run |
| 20 | `exo_exit(code)` | End the caller; everything it holds is reclaimed |
| 21 | `exo_launch(app_id)` | Start one of the programs built into the kernel image |
| 36 | `exo_launch_pkg(pkg, len, flags, name)` | Start a program from a package read off the disk |
| 40 | `exo_launch_start(ctx)` | Start a program prepared with #36's "hold" flag |
| **Introspection** |||
| 22 | `exo_memstat(out)` | Page usage across the whole machine |
| 23 | `exo_pslist(out, max)` | The running programs: id, name, state, pages |
| 38 | `exo_resstat(out)` | Free pages, program slots and sound voices, for admission checks |
| 39 | `exo_grantstat(ctx, out)` | Everything one program currently holds |
| **Debug** |||
| 8 | `exo_serial_write(buf, len)` | Write bytes to the serial port |
| **Reserved** |||
| 9–16 | (file I/O) | Never bound. File I/O is LibOS code over #24–#27, so a stale caller gets "not implemented" |

### Pointer checking

Every pointer a program passes must lie inside its own window and be mapped.
Writable pointers must also be mapped writable. Anything else is refused with
`EFAULT` before the kernel touches it. The fuzzer drives one million calls
with random numbers and arguments to back this up.

→ `docs/syscall_spec.md` §3 (full table with every error code), §3.2a (error
codes), §3.4 (the entry path).

---

## 6. Isolation guarantees

The kernel's job is to make the following true for any two programs A and B,
whatever A's code does. "Secure binding" is the exokernel term. A grant ties
a resource to an owner, and every later operation on the resource is checked
against that owner.

![Secure binding: how the kernel tracks, checks and reclaims every resource](diagrams/05-secure-binding.svg)

| Resource | Guarantee | Enforced by | Proven by (test suite) |
|---|---|---|---|
| Kernel memory and hardware | A cannot read or write kernel memory, or use I/O ports | Supervisor-only kernel mappings; ring 3 has no I/O permission | `kernel_mem_fault`, `port_io_fault` |
| Physical pages | A cannot free, map or unmap a page owned by B | Owner tag checked on every page call (`EPERM`) | `ownership`, `memory_isolation`, `page_map` |
| Address space | A's mappings live in A's page tables only, inside A's window | Per-program page tables; the window check on `exo_page_map` | `vmm`, `libos_launch` |
| Screen | A never sees B's pixels, and cannot write to the real screen | Each program draws into its own private framebuffer; only the kernel copies the foreground one to the screen, and the real framebuffer cannot be mapped by any program | `fb_shadow`, `fb_binding` |
| Disk | A can only read and write sectors inside ranges bound to A; a read-only binding refuses writes | Extent table; overlapping ranges of different owners are refused (`EBUSY`) | `disk_binding`, `syscall_disk`, `multidisk` |
| Sound | A cannot stop or re-place B's sound-effect voices or tone, or write to or stop B's music stream. The music *volume* is the exception: any program may set it | Voice ownership (`EPERM`); the music stream is bound to one program at a time | `syscall_sound_pcm`, `syscall_sound_stream`, `syscall_sound` |
| Kernel arguments | Nothing A passes can make the kernel read or write outside A's memory | Pointer checks at the gate (§5) | `syscall_fuzz`, `exo_syscall` |
| End of life | When A exits, everything it held comes back | `exo_exit` reclaims pages, framebuffer, disk ranges and sound | `syscall_exit`, `revoke` |

**Revocation.** The kernel can also take a resource back while its owner is
still running. It does this in three phases:
1. It marks the resource ("I want this back").
2. It lets the owner return it the ordinary way.
3. It takes it if the owner doesn't.

Taking is scoped to the named owner, so a revocation can never free another
program's resource.

### What is *not* guaranteed

| Limit | Consequence | Why, and what would fix it |
|---|---|---|
| **No preemption** | A program that never makes a syscall keeps the CPU; the others wait | Switching is cooperative (§7). Ctrl+Tab still switches the foreground, but only at the spinning program's next syscall. Timer-driven preemption is planned (SCRUM-127) |
| **Introspection is global** | Any program can see every program's page counts and bindings (#22, #23, #38, #39) | Deliberate, for `top` and admission checks; a hardened system would scope or gate them |
| **Single CPU** | No parallelism | Out of scope for this project |
| **No NX, SMEP or SMAP** | Data pages are executable; the kernel is not hardware-blocked from user pages | Not enabled yet. The kernel never follows a user pointer without checking it (§5), so isolation does not depend on them |
| **Shared devices are trusted to the kernel's drivers** | A driver bug is a kernel bug | Inherent to a kernel that drives devices; the drivers are small and tested |

→ `docs/syscall_spec.md` §3.3 (secure binding), §3.5–§3.5c (screen, disk,
sound), §3.6 (revocation).

---

## 7. Running several programs

![Context switch](diagrams/07-context-switch.svg)

- **Up to 15 programs** can run at once, each with its own address space,
  saved registers and resources.
- **Switching is cooperative.** It happens when a program calls `exo_yield`,
  launches another program, or exits, or when Ctrl+Tab is pressed. A
  Ctrl+Tab takes effect at the running program's next syscall, which is the
  point where the kernel can save its state safely.
- **The screen shows the foreground program.** About 60 times a second the
  kernel copies the current program's private framebuffer to the real screen.
  A background program keeps drawing into its own buffer, and nothing it
  draws is lost.
- **Each program can get its own disk volume.** Two programs, two
  filesystems, one disk, and the kernel knows only that each owns a range of
  sectors.

→ `docs/architecture.md` §7 "Multi-application", `docs/filesystem.md` §8.

---

## 8. Design rationale

| Decision | Why | Rejected alternative |
|---|---|---|
| **File system in the LibOS** (ExoFS), kernel exposes sectors | File layout is policy. With it outside the kernel, programs can choose their own, the kernel stays small, and a filesystem bug cannot corrupt the kernel | A small file system in the kernel, behind `exo_file_*` syscalls. It was planned, and numbers #9–#16 were reserved for it, then dropped by team decision |
| **Ownership tags on every page**, checked on each call | The exokernel protection model needs *who owns this* answered in constant time for every operation | Trusting a program with any physical address it names, which is how the first mapping call worked until it was closed |
| **A private framebuffer per program**, composited by the kernel | Several programs can draw at once, switching shows the right one, and no program can draw over another | One exclusive screen binding (the first design); a second program got `EBUSY` and switching showed stale pixels |
| **Cooperative switching** | Simple and safe to get right: a switch only happens at a syscall, where the kernel's state is consistent | Preemptive scheduling: correct, but needs per-CPU state and reentrancy work. Planned as a follow-up |
| **Program window above 64 TiB** | The kernel maps all RAM 1:1, so a program window lower down would overlap kernel mappings | Remapping the kernel to the top of the address space. A larger change for the same isolation |
| **No floating point or SIMD in the kernel** | The kernel then never has to save a program's SSE registers on entry, which keeps every syscall cheap | Saving FPU/SSE state on every entry |
| **Linux-style register preservation** | Compiler-generated syscall stubs can keep values live across a call, which the plain C ABI would not allow | Saving only the C callee-saved registers. That silently corrupts a loop that reuses an argument |
| **Revocation: mark, return, take** | The kernel can hand out generously because it can always get a resource back, and it asks before it takes | Refusing grants that might be needed later |
| **Tests inside the kernel** | They run on the real hardware model, in ring 0 and ring 3, with no host/target mismatch; every boot of a test build runs all of them | Host-side unit tests that cannot reach real page tables, interrupts or devices |
| **Doom unmodified** | Proves the interface is enough for a real program, not one written for it | Patching the engine to fit the kernel |

---

## 9. Performance at a glance

Measured under QEMU without hardware acceleration. `docs/performance.md`
has the method, the charts and the full tables.

- **Doom:** 15 ms per frame on average (about 66 fps), against a 30 ms
  budget.
- **Kernel entry and exit:** a few hundred CPU cycles. Allocating a page is
  the most expensive call, at about 130 times that.
- **Memory:** a drawing program costs 3 MiB for its private framebuffer
  before anything else. Doom uses 11.4 MiB in total.

---

## 10. Further reading

| For | Read |
|---|---|
| Full design, boot sequence, LibOS and Doom integration, build | `docs/architecture.md` |
| Syscall ABI, every error code, ownership, revocation, sound | `docs/syscall_spec.md` |
| Paging, allocators, heaps | `docs/memory.md` |
| The ExoFS file system | `docs/filesystem.md` |
| Measurements | `docs/performance.md` |
| Testing framework | `docs/testing.md` |
| Device drivers | `docs/drivers/` |
| Diagrams (D2 sources and SVGs) | `docs/diagrams/` |
