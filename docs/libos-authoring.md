# Writing a LibOS program for ExoDoom

SCRUM-238. How to write a new program for ExoDoom, from an empty file to
`run hello.exo` at the shell. It is for someone outside the team: it assumes
C and the build commands in `CLAUDE.md`, not familiarity with this repository.

`docs/exokernel-architecture.md` explains *why* the system is built this way.
This guide is the *how*. Its worked example, `src/libos_hello/libos_hello.c`,
is built and packaged on every build as `hello.exo`, so the steps below are
known to work.

---

## 1. What a program is

A program here is a **LibOS program**: one ring-3 image that links in whatever
library-OS code it needs and talks to the kernel only through the system calls
in `src/exo_syscall.h`.

- **The image** is three sections: `.text` (code), `.data` (initialised data)
  and `.bss` (zeroed data). The kernel loads them into the program's own
  address space at fixed addresses (`src/libos_launch.h`). It enters at the
  first byte of `.text`, with a 64 KiB stack.
- **The package** (`.exo`, `src/exo_pkg.h`) wraps the image with a manifest
  that says what the program wants: a framebuffer, the keyboard, disk access,
  sound voices, its own disk volume, and how many memory pages. The shell's
  `run` command reads it from the program volume, checks the request against
  free resources, and asks the kernel to launch it.
- **The library pieces** you can link, all plain C in `src/`:

| You want | Link | Gives you |
|---|---|---|
| To draw | `src/libos_fb.c`, `src/fb.c` | `libos_fb_map()` maps your private framebuffer; `fb_*` draws pixels and rectangles |
| Text on screen | `src/fb_console.c` | `fbcon_*`: an 8×16 text console over a framebuffer |
| A heap | `src/libos_page_alloc.c`, `src/libos_heap.c` | `libos_heap_alloc/_free`: pages from `exo_page_alloc`, up to 16 MiB |
| A C library | `src/stdlib.c`, `src/stdio.c`, `src/string.c`, `src/ctype.c`, `src/errno.c`, `src/fpconv.c` | `malloc` (over the heap), `printf` (to the serial port), `snprintf`, `str*`/`mem*`, … |
| Files | `src/libos_fs/*.c` | ExoFS: mount a volume on sectors you hold, then `exofs_open/read/write/…` |
| `FILE*` over files | `src/libos_fs/exo_file.c`, `exo_file_stdio.c` | `fopen`/`fread`/`fprintf` over ExoFS |

`src/libos_voldemo/` (ExoFS plus the C library) and `src/libos_doom/`
(everything, plus Doom) are the larger examples. `src/libos_clock/` and
`src/libos_snake/` are small ones in the same shape as `hello`.

---

## 2. Quick start: four edits

Using `hello` as the name. Replace it with yours throughout.

1. **Create `src/libos_hello/libos_hello.c`.** It must be in its own
   subdirectory (§4.4). The next section walks through the file.
2. **Build it.** In `docker/scripts/build.sh`, next to the clock and `top`
   targets:
   ```bash
   ring3_embed=0
   build_ring3_link_target libos_hello src/libos_hello "" "" \
     src/libos_hello/libos_hello.c src/fb.c src/fb_console.c src/libos_fb.c
   ring3_embed=1
   ```
   The arguments are: the target name, the directory to write the generated
   layout header into, extra include dir, extra compiler flags, then sources
   with **your entry file first** (§4.1). `ring3_embed=0` means "package it,
   don't link it into the kernel".
3. **Package it.** In the same script, with the other
   `package_ring3_target` lines:
   ```bash
   package_ring3_target libos_hello src/libos_hello hello.exo fb,kbd
   ```
   `fb,kbd` is the manifest: a framebuffer and the keyboard (§5).
4. **Ignore the generated header.** Add `src/libos_hello/libos_hello_layout.h`
   to `.gitignore`. The build writes it (§4.4).

Then build and boot:

```bash
make docker-run-kernel
```

At the shell, `programs` lists `hello.exo`, and `run hello.exo` starts it.
The name must include `.exo`. Press keys to see their codes; Q or Esc returns to the shell, and
Ctrl+Tab switches away without quitting.

---

## 3. The example, explained

`src/libos_hello/libos_hello.c`, in order:

```c
__attribute__((section(".text.entry")))
void libos_hello_main(void);
```
The entry point. The build names the ELF entry symbol `<target>_main`, and
the attribute puts the function at offset 0 of `.text`, which is where the
kernel jumps (§4.1).

```c
static char hello_text[] =
    "Hello from a LibOS.\nPress keys; Q or Esc quits.\n";
```
A **writable** array, deliberately: it gives `.data` some content. A program
whose `.data` is empty fails the build (§4.2).

```c
static void hello_exit(void)
{
    exo_syscall1(EXO_SYS_EXIT, 0);
    for (;;) { exo_yield(); }
}
```
How to quit (§4.9). `exo_exit` reclaims everything the program holds and
switches to the next program; the loop is there for the compiler.

```c
    if (libos_fb_map(&lfb) != 0 ||
        !fb_init_bgrx8888(&fb, (uintptr_t)lfb.vaddr, lfb.pitch, lfb.width,
                          lfb.height, lfb.bpp) ||
        !fbcon_init(&con, &fb)) {
        hello_exit();
    }
    fbcon_write(&con, hello_text);
```
`libos_fb_map()` calls `exo_fb_acquire` to get this program's own
framebuffer, which is the size of the screen. It then maps it page by page
with `exo_page_map`. `fb_init_bgrx8888` and `fbcon_init` turn that memory
into a text console. Everything drawn there appears on screen whenever this
program is in the foreground.

```c
        if (exo_kbd_poll(&ev) <= 0) {
            for (volatile uint32_t spin = 0; spin < 20000; spin++) {
                __asm__ volatile("pause");
            }
            continue;
        }
```
Input is polled. Between empty polls, back off with `pause` instead of calling
the kernel again straight away (§4.10).

---

## 4. The rules, and why each exists

### 4.1 The entry point goes first in `.text`

The kernel enters a program at offset 0 of its `.text` blob
(`libos_build_image()`, `src/libos_launch.h`). Two things together put your
entry function there:

- **`__attribute__((section(".text.entry")))`** on it. The shared linker
  script, `tests/kernel/ring3_link_target.ld.in`, places `*(.text.entry)`
  before every other `.text` input.
- **Listing the entry file first** in `build_ring3_link_target()`'s source
  list.

"Just list the entry file first" is **not enough on its own**. At `-O0`
(`make docker-build DEBUG=1`), GCC does not inline `exo_syscall.h`'s
`static inline` syscall stubs. It emits them as real functions, ahead of your
entry point in the same file, and a stub lands at offset 0. The program then
starts by executing a random syscall wrapper. The attribute fixes this
regardless of order. The file still goes first because of §4.3.

### 4.2 `.data` must not be empty

`build_ring3_link_target()` extracts `.text` and `.data` with
`objcopy --only-section`, and an empty `.data` fails that step. Give the
program at least one writable, non-zero-initialised global, as `hello_text`
does. A `const` string goes to `.rodata`, which is folded into `.text`, so it
doesn't count.

### 4.3 Launch parameters: the first non-zero global

A program that needs input from its launcher gets it through a **params
struct**. The kernel copies the struct over the first bytes of the program's
`.data` before entering it (`libos_launch_patch_params()`). For that copy to
land on your struct:

- it must be the **first global defined** in the entry file;
- the entry file must be **first in the source list**, because globals land in
  `.data` in source order and `ld` collects files in link order;
- it must be **non-zero-initialised**. GCC puts a zero-initialised global in
  `.bss`, which has no bytes in the image, so nothing would be there to
  patch. Initialise it with sentinels:
  ```c
  libos_wad_params_t g_wad_params = { .wad_vaddr = ~0ULL, .wad_size = ~0ULL };
  ```
- the package must declare its size: `--params-len 16` for that struct.

Today only the `wad` request uses it, to hand the WAD viewer the address and
size of the mapped IWAD (`src/libos_wad_viewer/libos_wad_viewer.c`). Treat the
struct as your own mutable state once the program runs.

### 4.4 Your own directory, and no `-DEXO_KERNEL`

The build compiles every `src/*.c` **as kernel code**, with `-DEXO_KERNEL`.
A program in `src/` directly would be compiled into the kernel, the wrong way
round. Put it in `src/libos_<name>/`; subdirectories are not globbed.
`build_ring3_link_target()` compiles your sources once, without that define,
and writes `src/libos_<name>/libos_<name>_layout.h` with your `.bss` length,
which the packager reads.

The same `src/*.c` files you link (`fb.c`, `stdio.c`, …) behave differently
without `EXO_KERNEL`:

| | Kernel build (`EXO_KERNEL`) | Your program (no `EXO_KERNEL`) |
|---|---|---|
| `exo_syscall.h` | numbers and structs only | also the inline `syscall` stubs and typed wrappers (`exo_kbd_poll`, …) |
| `malloc`/`free` (`stdlib.c`) | kernel heap | `libos_heap_alloc/_free` |
| `printf` (`stdio.c`) | straight to the serial port | buffered per line, sent with `exo_serial_write` |
| page source (`libos_page_alloc.c`) | in-kernel dispatch | real `exo_page_alloc`/`exo_page_map` syscalls |

### 4.5 One compiler flag set, no SSE unless you ask

Your sources compile with the kernel's flags, minus kernel-only memory-model
flags. That includes **`-mno-sse`**, because "not the kernel" does not mean
"has SSE". If your code uses `float` or `double`, pass `"-msse -msse2"` as the
fourth argument to `build_ring3_link_target()`, as `libos_doom` does.

In shared code, test `__SSE2__`, never `!EXO_KERNEL`. A ring-3 target without
SSE is the normal case.

### 4.6 Syscalls only through `exo_syscall.h`

There is no other way into the kernel. Every pointer you pass must point into
your own memory, or the call returns `-EXO_EFAULT`.

- A negative return is an error code (`src/exo_errno.h`, Linux numbering).
- The full table, with every error each call can return, is
  `docs/syscall_spec.md` §3.2.
- A ring-3 program cannot execute `in`/`out` or touch kernel memory. Both
  fault (`docs/exokernel-architecture.md` §6).

### 4.7 The stack, and why RSP starts at top − 8

Every program gets 16 stack pages (64 KiB), with an unmapped guard page below
them. Overflowing the stack faults; it does not corrupt your data. Deep
recursion or large stack arrays belong on the heap.

The entry RSP is 8 below the top of the stack
(`LIBOS_LAUNCH_STACK_TOP = LAYOUT_END - 8`). The SysV ABI promises every
function `RSP ≡ 8 (mod 16)` on entry, because `call` pushes an 8-byte return
address onto a 16-aligned stack. A program is not *called*. It is entered by
`iretq`, which pushes nothing, so the kernel sets that alignment by hand.
Without it, the first aligned SSE store to a stack variable raises `#GP`; Doom
found this the first time it ran.

### 4.8 Size limits

| Region | Limit | Where |
|---|---|---|
| `.text` (code + read-only data) | 192 pages (768 KiB) | `LIBOS_LAUNCH_MAX_CODE_PAGES` |
| `.data` + `.bss` | 192 pages (768 KiB) | `LIBOS_LAUNCH_MAX_DATA_PAGES` |
| Stack | 16 pages | `LIBOS_LAUNCH_STACK_PAGES` |
| Heap | 4,096 pages (16 MiB), on demand | `LIBOS_PAGE_ALLOC_MAX_PAGES` |

A program over the code or data limit fails at package time (`exopkg`) or at
launch. **First move large buffers to the heap.** A big `static` array is
`.bss` and counts against the 768 KiB, whereas `malloc` or `libos_heap_alloc`
memory does not. Doom's 6 MiB zone heap is allocated this way.

If you still need more, raise the cap in `src/libos_launch.h` **and** in
`src/exo_pkg.h`; a `_Static_assert` fails the build if they disagree. Then
check that the layout still ends below the heap base (`EXO_USER_VA_BASE` +
16 MiB), as the comment above `LIBOS_LAUNCH_LAYOUT_END` explains.

### 4.9 Quitting

Call `exo_syscall1(EXO_SYS_EXIT, code)` and follow it with
`for (;;) exo_yield();`. Do not call the `exo_exit()` wrapper. Its body ends
in an infinite loop, so GCC marks everything after a call to it unreachable,
but a program's slot can in fact be resumed before the shell tidies it away.
The raw call plus the loop keeps that path valid. On exit the kernel reclaims
your pages, framebuffer, disk bindings and sound; there is nothing to free by
hand.

### 4.9a Crashing

If your program takes a CPU exception (an address that is not mapped or not
yours, a privileged instruction such as `cli`, `hlt` or port I/O, a divide by
zero, an illegal instruction), the kernel ends it as if it had called
`exo_exit`: your pages, framebuffer, disk bindings and sound are reclaimed and
the next ready program, normally the shell, runs. You get no chance to clean
up or handle it, so there is no signal to catch. The shell then prints
`program <name> stopped: <why>`, and `exo_fault_info` (#41) gives the same
record (vector, faulting `rip`, address) to any program that wants it. A
program that crashes as the only one running is replaced by a fresh shell.
`run crash` (`src/libos_crash/`) does it on purpose, three seconds after it starts, so you can Ctrl+Tab back to the shell and watch the report arrive.

### 4.10 You share the CPU

ExoDoom switches programs cooperatively. A switch happens at a syscall: your
own `exo_yield`, an exit or launch, or a Ctrl+Tab from the user, which takes
effect at your next syscall.

- **Don't assume you run continuously.** Ctrl+Tab can move you to the
  background between any two syscalls. You keep running when you come back,
  and your framebuffer keeps everything you drew meanwhile.
- **Don't poll the kernel in a tight loop.** Every syscall runs with
  interrupts off, so back-to-back calls starve the 1 kHz timer. The clock
  program measured itself running at 83% speed that way. Spin on `pause`
  between polls, as `hello` does.
- **Don't spin without making syscalls.** A program that never calls the
  kernel keeps the CPU, and nothing can take it away (no preemption yet).

### 4.11 Disk buffers come from your heap

If you use ExoFS or `exo_disk_read/write` directly, the buffer must be in your
own address space. Use `libos_heap_alloc()`. ExoFS already does this for its
own buffers; `docs/filesystem.md` "Pitfalls" has the rest.

---

## 5. Asking for resources: the manifest

`package_ring3_target <target> <dir> <file>.exo <wants> [options]` writes the
manifest with `tools/exopkg`:

| Want / option | Means |
|---|---|
| `fb` | You will draw (a private framebuffer) |
| `kbd` | You read the keyboard |
| `disk` | You will bind disk sectors |
| `progvol` | Read-only access to the program volume, where the packages and the IWAD live |
| `wad` | Map the IWAD into your address space and pass you its address (§4.3); needs `--params-len 16` |
| `--sound-voices N` | PCM mixer voices you will use |
| `--volume-slot S --volume-sectors N` | A private disk volume of your own; find it at run time with `exo_disk_extent` (#37) |
| `--min-pages N`, `--recommended-pages N` | Memory to admit you with; both default to measured values |
| `--stack-pages N` | Stack pages, up to 16 |

When you `run` a program, the shell compares the manifest with what
`exo_resstat` (#38) says is free. Below the minimum it refuses the launch;
between the minimum and the recommendation it starts the program and reports
"granted, degraded". The kernel only reports free resources; the shell, which
is a LibOS, makes the call.

**Your own filesystem:**
1. Ask for a volume (`--volume-slot`).
2. At run time, read its extent with `exo_disk_extent`.
3. Bind it with `exo_disk_acquire`.
4. Mount it with `exofs_mount`, and format it on first run if the mount
   fails.

`src/libos_voldemo/libos_voldemo.c` does exactly this, in one short
function.

---

## 6. Debugging

- **Serial is the log.** Link the C library and `printf` goes there, or call
  `exo_serial_write` yourself. `make docker-run-kernel` shows serial in the
  terminal.
- **A fault in your program prints a report on serial**: the faulting
  address (`cr2`), `rip`, the error bits, and "ring 3 (LibOS) id N "name" --
  terminating: <why>". The program is then ended and the shell says so on
  screen (§4.9a). Look `rip` up in `build/libos_<name>.elf` with
  `x86_64-elf-addr2line -e`.
- **`make docker-build DEBUG=1`** builds at `-O0` with symbols. For GDB, see
  `docs/debugging.md`.
- **`top`** (`run top.exo`) shows what each program holds, live.

---

## 7. Checklist

- [ ] Source in `src/libos_<name>/`, entry function `libos_<name>_main` with
      `__attribute__((section(".text.entry")))`
- [ ] Entry file first in `build_ring3_link_target()`'s source list
- [ ] At least one writable, non-zero-initialised global (non-empty `.data`)
- [ ] Params struct, if any: first global, non-zero-initialised,
      `--params-len` set
- [ ] `-msse -msse2` in the extra flags if you use floating point
- [ ] Large buffers on the heap, not in `static` arrays
- [ ] Quit with `EXO_SYS_EXIT` plus a `for (;;) exo_yield();` loop
- [ ] `pause` back-off between empty polls
- [ ] `package_ring3_target` line with the right wants
- [ ] Generated `libos_<name>_layout.h` in `.gitignore`
- [ ] Booted, `programs` lists it, `run <name>.exo` works, Q returns to the
      shell
