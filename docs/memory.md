# ExoDoom Memory Subsystem

**Last updated:** 2 Apr 2026 — reflects Sprint 1 state

---

## Table of Contents

1. [Overview](#1-overview)
2. [Physical address space](#2-physical-address-space)
3. [Kernel image layout](#3-kernel-image-layout)
4. [Phase 1 — Multiboot mmap parsing](#4-phase-1--multiboot-mmap-parsing)
5. [Phase 2 — Bump allocator](#5-phase-2--bump-allocator)
6. [Phase 3 — Bitmap page allocator](#6-phase-3--bitmap-page-allocator)
7. [Phase 4 — Virtual memory and paging](#7-phase-4--virtual-memory-and-paging)
8. [Phase 5 — LibOS heap](#8-phase-5--libos-heap)
9. [Doom memory requirements](#9-doom-memory-requirements)
10. [Design decisions and gotchas](#10-design-decisions-and-gotchas)

---

## 1. Overview

The memory subsystem is built in five sequential phases, each depending on the
last. The kernel starts with nothing but a stack and a bump pointer, then
progressively builds up the infrastructure needed to hand Doom a working
`malloc`.

```
Phase 1  mmap_init()       Parse multiboot memory map → usable/reserved regions
Phase 2  memory_init()     Bump allocator from &_bss_end → used for early boot allocs
Phase 3  pmm_init()        Bitmap page allocator (alloc_page / free_page) [Sprint 1]
Phase 4  vmm_init()        Enable paging, build page tables, exo_page_* syscalls [Sprint 2]
Phase 5  LibOS heap        first-fit allocator backed by exo_page_alloc [Sprint 3]
```

At no point does the kernel use a general-purpose heap for itself. Internal
kernel structures (IDT, bitmap, page directory) are allocated from the bump
allocator during boot and never freed.

---

## 2. Physical address space

On QEMU with `-m 256M`, the physical address space looks like this at boot,
before the kernel does anything:

```
Physical address
0x00000000 ┌─────────────────────────────┐
           │  Real-mode IVT + BDA        │  ~1 KB  (BIOS interrupt vectors)
0x00000500 ├─────────────────────────────┤
           │  Free low memory            │  ~29 KB
0x00007C00 ├─────────────────────────────┤
           │  GRUB bootloader            │
0x00080000 ├─────────────────────────────┤
           │  BIOS data / EBDA           │  reserved
0x000A0000 ├─────────────────────────────┤
           │  VGA / VESA memory          │  384 KB  (0xA0000–0xFFFFF)
0x00100000 ├─────────────────────────────┤  ← 1M boundary
           │  (reserved gap)             │
0x00200000 ├─────────────────────────────┤  ← kernel load address
           │  Kernel image               │  ~512 KB typical
           │  .multiboot_header          │
           │  .text                      │
           │  .rodata                    │
           │  .data                      │
           │  .bss                       │  ← &_bss_end marks end of image
           ├─────────────────────────────┤
           │  Bump allocator pool        │  grows upward from align_up(&_bss_end, 4K)
           ├─────────────────────────────┤
           │  WAD module                 │  freedoom2.wad, placed by GRUB
           │  (~12 MB for Freedoom2)     │
           ├─────────────────────────────┤
           │  Free physical RAM          │  available for page allocator
           │                             │
0x10000000 └─────────────────────────────┘  ← 256M top
```

The VESA framebuffer physical address is passed in `mb->framebuffer_addr` and is
typically in the range `0xFD000000`–`0xFF000000` on QEMU — above the 256M RAM
region, in the PCI MMIO aperture.

The multiboot mmap (from `mmap_init`) describes which ranges are usable RAM vs.
reserved. The page allocator must honour these boundaries and additionally
exclude the kernel image and WAD module regions.

---

## 3. Kernel image layout

Defined in `src/linker.ld`. The kernel is linked and loaded at **virtual address
2M** (`0x200000`). Pre-paging, virtual == physical (identity mapped by default
since GRUB loads the kernel at that physical address).

```
0x00200000  _load_start
            .multiboot_header   ALIGN(4)     — magic, flags, checksum, video fields
            .text               BLOCK(4K)    — executable code
            .rodata             BLOCK(4K)    — read-only data (font table, const strings)
            .data               BLOCK(4K)    — initialised globals
_load_end
            .bss                BLOCK(4K)    — zero-initialised globals (zeroed by GRUB)
_bss_end                                    — bump allocator starts here (aligned up to 4K)
```

Key linker symbols used in C:

| Symbol        | Where used                               |
| ------------- | ---------------------------------------- |
| `_bss_end`    | `memory_init()` — base of bump allocator |
| `_load_start` | Future: PMM reservation of kernel pages  |
| `_load_end`   | Future: PMM reservation of kernel pages  |

The 16 KiB stack is in `.bss` (`stack_bottom` / `stack_top` defined in
`boot.s`). It is zeroed by GRUB as part of the BSS section.

---

## 4. Phase 1 — Multiboot mmap parsing

**Files:** `src/mmap.c`, `src/mmap.h`, `src/multiboot.h` **Status:** ✅ Done
(SCRUM-6) **Called from:** `kernel_main` before `memory_init`

### What it does

GRUB populates a memory map in the `multiboot_info` struct. `mmap_init(mb)`
checks that `MULTIBOOT_INFO_FLAG_MMAP` (bit 6) is set in `mb->flags`, then walks
the variable-length entry list and records up to `MAX_MMAP_REGIONS` (32) entries
in a static array.

```c
// src/mmap.h
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} mmap_region_t;
```

Entry types:

| Value | Constant                      | Meaning                                  |
| ----- | ----------------------------- | ---------------------------------------- |
| 1     | `MULTIBOOT_MMAP_AVAILABLE`    | Usable RAM — page allocator can use this |
| 2     | `MULTIBOOT_MMAP_RESERVED`     | Reserved — do not touch                  |
| 3     | `MULTIBOOT_MMAP_ACPI_RECLAIM` | ACPI reclaimable                         |
| 4     | `MULTIBOOT_MMAP_ACPI_NVS`     | ACPI NVS                                 |
| 5     | `MULTIBOOT_MMAP_BADRAM`       | Faulty RAM                               |

Each entry in the multiboot mmap has a `size` field (the entry's own size, not
the region size) that varies between firmware implementations. The walker
advances by `entry->size + sizeof(entry->size)` — not a fixed stride — which is
the correct way to handle this.

### Output

`mmap_get_regions(uint32_t* count)` returns a pointer to the static array and
writes the count. The page allocator (Phase 3) uses this to know which physical
ranges are safe to hand out.

### QEMU output (typical, `-m 256M`)

```
Multiboot memory map:
 base=0x0000000000000000 len=0x000000000009FC00 type=1 usable
 base=0x000000000009FC00 len=0x0000000000000400 type=2 reserved
 base=0x00000000000F0000 len=0x0000000000010000 type=2 reserved
 base=0x0000000000100000 len=0x000000000FEF0000 type=1 usable
 base=0x00000000FFFC0000 len=0x0000000000040000 type=2 reserved
```

The large usable region starting at 1M (`0x100000`) is where both the kernel and
all available RAM live. The page allocator will operate within this region,
minus the kernel image and WAD module.

---

## 5. Phase 2 — Bump allocator

**Files:** `src/memory.c`, `src/memory.h` **Status:** ✅ Done **Called from:**
`kernel_main` after `mmap_init`

### What it does

The bump allocator is the simplest possible allocator: a single pointer that
only moves forward. It is used exclusively during early kernel boot to allocate
permanent structures (the PMM bitmap, the IDT, page directory entries) before
the page allocator is ready.

```c
static uintptr_t placement_address = 0;

void memory_init(void) {
    if (placement_address != 0) return;
    placement_address = align_up((uintptr_t)&_bss_end, 0x1000);
}

void* kmalloc(size_t size) {
    if (placement_address == 0) memory_init();
    uintptr_t addr = placement_address;
    placement_address = align_up(placement_address + size, 0x1000);
    return (void*)addr;
}
```

### Properties

- **No `free`.** Allocations are permanent. This is intentional — everything
  allocated here is kernel infrastructure that lives for the life of the system.
- **Always 4K-aligned.** Every allocation is rounded up to the next 4K boundary.
  This makes it trivial to later map these regions into page tables since they
  are already page-aligned.
- **Thread-unsafe.** There is no locking. This is fine because interrupts are
  disabled during early boot when `kmalloc` is called.
- **`memory_base_address()`** returns the current `placement_address`, used for
  serial diagnostics and to tell the page allocator where bump-allocated data
  ends.

### What gets bump-allocated

| Allocation               | Size                                   | When         |
| ------------------------ | -------------------------------------- | ------------ |
| PMM bitmap (Phase 3)     | `total_pages / 8` bytes, rounded to 4K | `pmm_init()` |
| Page directory (Phase 4) | 4K (1024 × 4-byte entries)             | `vmm_init()` |
| Initial page tables      | 4K each                                | `vmm_init()` |

After Phase 4 (paging enabled), `kmalloc` is retired. All further kernel
allocations go through `alloc_page` directly.

---

## 6. Phase 3 — Bitmap page allocator

**Files:** `src/pmm.c`, `src/pmm.h` _(not yet merged — SCRUM-7, SCRUM-8)_
**Status:** 🔄 In Review / In Progress (SCRUM-7 open PR, SCRUM-8 in progress)

### Design

The physical memory manager (PMM) tracks 4K pages using a flat bitmap stored in
bump-allocated memory. Each bit represents one physical page: `0` = free, `1` =
used.

```
total_pages  = (top of usable RAM) / 4096
bitmap_size  = ceil(total_pages / 8) bytes   — bump-allocated, 4K aligned
```

For QEMU `-m 256M`: 256 MiB / 4K = 65,536 pages → 8,192 bytes (8 KB) bitmap.

### API

```c
void   pmm_init(void);               // build bitmap, mark all pages used, free usable ones
void*  alloc_page(void);             // find first free page, mark used, return phys addr
void   free_page(void* phys_addr);   // mark page free; detect + log double-free
```

### Initialisation sequence

`pmm_init()` must be called after both `mmap_init()` and `memory_init()`:

1. Allocate bitmap via `kmalloc` — marks all pages as used by default.
2. Walk the mmap regions: for each `MULTIBOOT_MMAP_AVAILABLE` region, mark pages
   in that range as free.
3. Re-mark as used: all pages covered by the kernel image (`_load_start` →
   `_bss_end`), the bump allocator pool (up to `memory_base_address()`), and the
   WAD multiboot module region. This is SCRUM-8.
4. Optionally re-mark the low 1M as used (BIOS/VGA reserved).

After this, only genuinely free physical RAM is available for allocation.

### `alloc_page`

Scans the bitmap for the first `0` bit, sets it to `1`, and returns
`page_index * 4096` as the physical address. Returns `NULL` if no pages are
free.

For efficiency the scanner should track a "next free hint" index rather than
always starting from bit 0, but a linear scan is correct and acceptable for the
current page count.

### `free_page`

Computes `page_index = (uintptr_t)phys_addr / 4096`, checks the bit is currently
`1` (double-free detection — logs to serial and returns without corrupting the
bitmap if it is `0`), then clears it.

### Page accounting (QEMU `-m 256M`)

```
Total pages:       65,536   (256 MiB)
Low 1M reserved:     256   (BIOS/VGA)
Kernel image:         ~128   (~512 KB)
Bump pool:            ~16   (bitmap + early structs)
WAD module:         ~3,072   (~12 MB for Freedoom2)
──────────────────────────
Available to alloc: ~62,064  (~242 MiB)
```

---

## 7. Phase 4 — Virtual memory and paging

**Files:** `src/vmm.c`, `src/vmm.h` _(planned — SCRUM-15, SCRUM-16, SCRUM-17)_
**Status:** ⬜ Sprint 2

### Overview

x86 32-bit paging uses a two-level structure: a **page directory** (PD) with
1024 entries, each pointing to a **page table** (PT) with 1024 entries. Each PT
entry maps one 4K physical page to a 4K virtual page. Both the PD and each PT
are exactly 4K in size and must be page-aligned.

```
Virtual address (32-bit):
 31      22 21      12 11       0
 ┌────────┬──────────┬──────────┐
 │  PD idx│  PT idx  │  offset  │
 │ 10 bits│ 10 bits  │ 12 bits  │
 └────────┴──────────┴──────────┘
     │           │         │
     ▼           ▼         │
  PD entry    PT entry     │
  (phys addr  (phys addr ──┘─→ physical byte
   of PT)      of page)
```

`CR3` holds the physical address of the active page directory. Writing `CR3`
flushes the TLB. Setting bit 31 of `CR0` enables paging.

### Boot-time mapping plan

`vmm_init()` builds the initial page directory and page tables using `kmalloc`,
then enables paging:

1. **Identity map the kernel** (virtual `0x200000` → physical `0x200000`): all
   pages from `_load_start` to `memory_base_address()`. This keeps the kernel
   running without any address changes after `CR0.PG` is set.
2. **Identity map low memory** (virtual `0x0` → physical `0x0`, first 1M):
   needed for BIOS data structures and VGA compatibility.
3. **Map the framebuffer**: virtual address TBD (likely a fixed high address
   like `0xFD000000` matching physical) → `mb->framebuffer_addr`. Marked
   present + read/write, not user-accessible.
4. **Map the WAD module**: virtual address → physical address of the multiboot
   module. Read-only.
5. **Load CR3** with the physical address of the page directory.
6. **Set CR0.PG** to enable paging.

After this, nothing about the kernel's execution changes (addresses are the
same), but the MMU is now enforcing page permissions.

### Exokernel syscalls

Once paging is running and a LibOS address space exists, three syscalls expose
page management to the LibOS:

```c
// Allocate one 4K physical page; returns physical address or -ENOMEM
int32_t exo_page_alloc(void);

// Map a physical page at a virtual address in the caller's page directory
// flags: PAGE_PRESENT | PAGE_WRITE | PAGE_USER
int32_t exo_page_map(uint32_t vaddr, uint32_t paddr, uint32_t flags);

// Unmap a virtual page (does not free the physical page)
int32_t exo_page_unmap(uint32_t vaddr);
```

`exo_page_free` frees the physical page back to the PMM without unmapping it —
the LibOS is expected to call `exo_page_unmap` first.

### LibOS address space

Each LibOS gets its own page directory (allocated from the kernel PMM). The
kernel is mapped into the upper portion of every LibOS address space (ring 0
only, not user-accessible) so that syscall entry doesn't require a separate
kernel page directory switch. The LibOS's own code, heap, and stack live in the
lower virtual address range.

```
LibOS virtual address space (planned):
0x00000000 – 0x00100000   low memory (not mapped for LibOS — access GPFs)
0x00100000 – 0x40000000   LibOS code + heap + stack
0x40000000 – 0xFD000000   unmapped
0xFD000000 – 0xFE000000   framebuffer (mapped by exo_fb_acquire + exo_page_map)
0xFE000000 – 0xFF000000   WAD module (read-only)
0xFF000000 – 0xFFFFFFFF   kernel (ring 0 only, not user-accessible)
```

### Page fault handler (SCRUM-17)

Vector 14 (page fault) must be handled before paging is enabled. On a fault, the
CPU pushes an error code and the faulting address is in `CR2`. The handler
should:

1. Print the faulting virtual address (`CR2`), error code, and `EIP` to serial.
2. Determine if it is a kernel fault (fatal — halt) or a LibOS fault (terminate
   the LibOS, log the fault).

> ⚠️ **Blocker:** The current `default_stub` in `isr.s` cannot handle
> error-code-pushing exceptions (SCRUM-135). A dedicated `error_stub` that pops
> the error code before `iret` must be installed on vector 14 before paging work
> begins. Without it, any page fault immediately triple-faults.

---

## 8. Phase 5 — LibOS heap

**Files:** LibOS source _(planned — SCRUM-25, SCRUM-26, SCRUM-37, SCRUM-38)_
**Status:** ⬜ Sprint 3

### Design

The LibOS heap is a **first-fit free-list allocator** that grows by requesting
pages from the kernel via `exo_page_alloc`. It lives entirely in user space —
the kernel has no knowledge of it beyond handing out physical pages.

```
LibOS malloc(size):
    1. Search free list for a block >= size
    2. If found: split if significantly larger, return pointer
    3. If not found: exo_page_alloc() + exo_page_map() to extend heap
       then add new region to free list and retry
```

`free(ptr)` adds the block back to the free list and coalesces adjacent free
blocks to reduce fragmentation.

`realloc(ptr, size)` is implemented as `malloc(size)` + `memcpy` + `free(ptr)` —
no in-place resize for the initial implementation.

### Sizing

Doom's heap requirements are well-defined (see `docs/syscall-spec.md` §5):

| Allocation                    | Size                         |
| ----------------------------- | ---------------------------- |
| Zone allocator (`I_ZoneBase`) | 6 MiB (one `malloc` call)    |
| `DG_ScreenBuffer`             | ~1 MiB (640 × 400 × 4 bytes) |
| Scattered small allocs        | < 100 KB                     |
| **Total**                     | **~8 MiB safe**              |

The LibOS page allocator needs to request approximately 2,048 pages (8 MiB) from
the kernel over its lifetime. This is well within the ~62,000 pages available on
a 256M QEMU VM.

---

## 9. Doom memory requirements

Understanding how Doom uses memory helps size the allocator correctly and avoid
surprises.

### Zone allocator

Doom has its own internal memory manager (`Z_Malloc` / `Z_Free`) that manages a
large contiguous zone. It calls `I_ZoneBase()` once at startup, which calls
`malloc(6 MiB)`. All game data — level geometry, textures, sprites, sounds,
intermission screens — is allocated from this zone. The zone manager handles
fragmentation internally using a doubly-linked block list.

If the 6 MiB `malloc` fails, Doom retries with decreasing sizes until `MIN_RAM`
(also 6 MiB) — at which point it prints a fatal error and exits. On a 256 MiB
QEMU VM this will never fail as long as the LibOS heap can service an 8 MiB
contiguous `malloc` (requiring 2,048 contiguous virtual pages, which is easily
achievable).

### WAD data

The WAD file is **not** loaded into the zone allocator. `W_Init()` memory-maps
lumps from the WAD using `fread` into zone-allocated buffers on demand. With the
memory-mapped WAD reader (Sprint 7, SCRUM-75), `fread` becomes a `memcpy` from
the in-memory module — zero additional physical memory needed for WAD storage.

### Screen buffer

`DG_ScreenBuffer` is a 640×400 RGBA8888 buffer (1,024,000 bytes, ~1 MiB)
allocated by `doomgeneric_Create()` via a direct `malloc`. This is separate from
the zone. `DG_DrawFrame` reads from this buffer and blits to the framebuffer on
every frame.

---

## 10. Design decisions and gotchas

**Why a bump allocator before a page allocator?** The page allocator's bitmap
needs somewhere to live before the page allocator exists — a classic
chicken-and-egg problem. The bump allocator breaks the cycle by providing a
one-way, no-overhead allocation primitive that works with no data structures at
all.

**Why always 4K-align bump allocations?** Two reasons: the hardware requires
page tables and the page directory to be 4K-aligned anyway, and it means the
bump allocator's output slots neatly into the PMM's page-granularity model.
Wasting a few bytes per allocation is irrelevant at boot time.

**Why does `memory_init` guard against being called twice?** `kmalloc` calls
`memory_init()` defensively if `placement_address == 0`. The explicit check
prevents a second call from resetting the pointer and clobbering
already-allocated memory if something calls `kmalloc` before `kernel_main` gets
to `memory_init()`.

**The SCRUM-135 / paging deadlock.** The page fault handler (vector 14) requires
an `error_stub` (pops error code before `iret`). That `error_stub` must exist
before paging is enabled. Paging must be enabled before the LibOS runs. The
LibOS must run before Doom. This is a strict prerequisite chain — SCRUM-135 must
be closed before any Sprint 2 paging work begins.

**WAD reservation timing.** SCRUM-8 (reserve kernel + WAD pages in PMM) must
complete before SCRUM-7 (bitmap page allocator) ships, or the allocator could
hand out pages that overlap the WAD module. In practice both are in-flight
together; the WAD reservation is part of `pmm_init()` and the two stories should
be merged or sequenced carefully in review.

**64-bit base/length in mmap entries.** The multiboot mmap uses `uint64_t` for
`addr` and `len` (as seen in `struct multiboot_mmap_entry`). On a 32-bit kernel,
addresses above 4 GiB cannot be mapped. The PMM should silently skip any usable
region whose `base + len` exceeds `0xFFFFFFFF`. On QEMU `-m 256M` no such
regions exist, but on real hardware with > 4 GiB RAM they will.

**`serial_flush()` before `qemu_exit()`.** In testing mode, `kernel_main` calls
`serial_flush()` before `qemu_exit()`. This is important: QEMU's
`isa-debug-exit` device triggers an immediate VM shutdown, and any bytes still
in the UART FIFO will be lost. Always flush before exiting if serial output
needs to be captured by CI.
