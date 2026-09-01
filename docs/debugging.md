# Debugging ExoDoom with GDB

ExoDoom uses QEMU's built-in GDB stub for remote debugging. GDB runs on your
host machine and connects to the VM over a TCP socket — no extra tooling
required beyond what's already in the repo.

## How it works

```
gdb (host)  <──RSP/tcp:1234──>  QEMU GDB stub  <──>  exodoom (x86_64 VM)
```

QEMU exposes a GDB Remote Serial Protocol stub on port 1234. Your host GDB reads
debug symbols from the unstripped ELF64 (`build/exodoom`) and sends commands
through that socket. The VM can be frozen at boot so you can set breakpoints
before any code runs.

## Prerequisites

- Docker (same as normal builds)
- `gdb` on your host with x86_64/multiarch support
  - **NixOS/Arch/Debian:** `gdb` from your package manager includes multiarch
  - **NixOS one-liner:** `nix-shell -p gdb`

## Building in debug mode

Pass `DEBUG=1` to get an unoptimized build with full symbols (`-g -O0`). Without
it you get the normal release build (`-O2`, harder to step through).

```bash
make docker-build DEBUG=1
```

## Starting the debugger

**Terminal 1** — start QEMU, frozen at boot, GDB port exposed:

```bash
make docker-run-debug DEBUG=1
```

QEMU will hang silently — that's correct, it's waiting for GDB to connect.

**Terminal 2** — connect GDB:

```bash
gdb build/exodoom
```

Then at the GDB prompt:

```gdb
(gdb) set architecture i386:x86-64
(gdb) target remote localhost:1234
(gdb) break kernel_main
(gdb) continue
```

The VM will unfreeze and run until it hits `kernel_main`.

## Shutting down

From GDB:

```gdb
(gdb) kill
```

From the QEMU monitor (switch with `Ctrl+A C`):

```
(qemu) quit
```

Shortcut: `Ctrl+A X` kills QEMU immediately from anywhere.

---

## GDB command reference

### Breakpoints

| Command                            | Description                     |
| ---------------------------------- | ------------------------------- |
| `break kernel_main`                | Break on function entry         |
| `break fb.c:45`                    | Break on file:line              |
| `info breakpoints`                 | List all breakpoints            |
| `delete 2`                         | Remove breakpoint #2            |
| `disable 2` / `enable 2`           | Temporarily toggle a breakpoint |
| `break fb_clear if fb->width == 0` | Conditional breakpoint          |

### Stepping

| Command    | Shorthand | Description                            |
| ---------- | --------- | -------------------------------------- |
| `continue` | `c`       | Run until next breakpoint              |
| `next`     | `n`       | Step over (don't enter function calls) |
| `step`     | `s`       | Step into function calls               |
| `finish`   |           | Run until current function returns     |
| `until 72` |           | Run until line 72 in current file      |

### Inspecting values

| Command                        | Description                       |
| ------------------------------ | --------------------------------- |
| `print mb->flags`              | Print expression                  |
| `print/x mb->framebuffer_addr` | Print as hex                      |
| `print *fb`                    | Dereference and print a struct    |
| `info locals`                  | All locals in current stack frame |
| `info registers`               | All CPU registers                 |
| `info registers rip rsp`       | Specific registers                |

### Examining memory

```gdb
x/10x  0xB8000     # 10 words at address, hex
x/20i  $rip        # disassemble 20 instructions at RIP
x/4bx  fb->addr    # 4 raw bytes (useful for pixel format checks)
x/s    0x...       # treat address as a C string
```

Format letters: `x` hex, `d` decimal, `b` byte, `i` instruction, `s` string.

### Watchpoints

Watchpoints pause execution whenever a value changes — useful for tracking down
memory corruption.

```gdb
watch fb->width              # break when fb->width is written
rwatch con.cursor_x          # break when cursor_x is read
awatch some_global           # break on read or write
info watchpoints
```

### Call stack

```gdb
backtrace          # full call stack (bt)
frame 2            # switch to stack frame #2
info frame         # details about current frame
```

### Convenience

```gdb
list kernel_main   # show source around a function
list fb.c:40,60    # show lines 40–60 of a file
display $rip       # print RIP automatically after every step
undisplay 1        # stop auto-displaying item #1
set var x = 5      # modify a variable at runtime
```

---

## Tips for bare-metal debugging

**Keep the unstripped ELF around.** The ISO (`build/exodoom.iso`) is what boots;
`build/exodoom` is what GDB reads for symbols. Don't delete it after a build.

**`qemu_exit()` needs the right QEMU flags.** The `docker-run-debug` target
includes `-device isa-debug-exit,iobase=0xf4,iosize=0x04`. Without it, calling
`qemu_exit()` in the kernel does nothing.

**`-O2` makes stepping confusing.** The compiler reorders and inlines
aggressively, so GDB's `next` can jump around non-linearly. Use `DEBUG=1` for
any session where you're actually stepping through code.

**Framebuffer byte-order check.** If colours look wrong, inspect raw pixel bytes
directly:

```gdb
(gdb) x/4bx fb.addr
```

Expected for BGRX8888: a red pixel (`fb_fill_rect(..., 255,0,0)`) should show
`00 00 FF 00`.

**Hang at boot?** If the kernel loops at `for(;;)` before the framebuffer check,
verify the Multiboot 2 framebuffer tag was found. Check `fb_tag` is non-NULL:

```gdb
(gdb) print fb_tag
```

If NULL, the framebuffer request tag in `boot.s` may not be formatted correctly,
or GRUB could not find a compatible video mode.
