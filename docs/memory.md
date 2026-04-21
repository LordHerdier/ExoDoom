# ExoDoom Memory Subsystem

**Last updated:** 20 Apr 2026 — reflects x86_64 migration

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
           │  .multiboot2                │
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
            .multiboot2         ALIGN(8)     — MB2 magic, tags (framebuffer request, end)
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

**Files:** `src/mmap.c`, `src/mmap.h`, `src/multiboot2.h` **Status:** ✅ Done
(SCRUM-6) **Called from:** `kernel_main` before `memory_init`

### What it does

GRUB populates a memory map in the Multiboot 2 info struct as a tag (type 6).
`mmap_init(mb)` uses `mb2_find_tag(mb, MB2_TAG_MMAP)` to locate the memory map
tag, then iterates entries using the tag's `entry_size` field and records up to
`MAX_MMAP_REGIONS` (32) entries in a static array.

```c
// src/mmap.h
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} mmap_region_t;
```

Entry types:

| Value | Constant                  | Meaning                                  |
| ----- | ------------------------- | ---------------------------------------- |
| 1     | `MB2_MMAP_AVAILABLE`      | Usable RAM — page allocator can use this |
| 2     | `MB2_MMAP_RESERVED`       | Reserved — do not touch                  |
| 3     | `MB2_MMAP_ACPI_RECLAIM`   | ACPI reclaimable                         |
| 4     | `MB2_MMAP_ACPI_NVS`       | ACPI NVS                                 |
| 5     | `MB2_MMAP_BADRAM`         | Faulty RAM                               |

The Multiboot 2 memory map tag contains an `entry_size` field that specifies the
stride between entries. The walker advances by `entry_size` bytes per entry,
which is the correct way to handle varying entry sizes across firmware.

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
| Page tables (Phase 4)    | 4K per table (512 × 8-byte entries)    | `vmm_init()` |

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

x86_64 long mode uses **4-level paging**: PML4 → PDPT → PD → PT. Each table
is 4 KB with 512 64-bit entries. The boot trampoline in `boot.s` sets up an
initial identity map using 2 MB pages (skipping the PT level):

```
Virtual address (x86_64, 48-bit canonical):
 47    39 38    30 29    21 20    12 11       0
 ┌──────┬──────┬──────┬──────┬──────────────┐
 │PML4  │PDPT  │ PD   │ PT   │   offset     │
 │9 bits│9 bits│9 bits│9 bits│  12 bits      │
 └──────┴──────┴──────┴──────┴──────────────┘
```

With 2 MB pages (PS bit set in PD entries), the PT level is skipped and the
offset is 21 bits, giving 512 × 2 MB = 1 GB per PD.

`CR3` holds the physical address of the PML4. Writing `CR3` flushes the TLB.

### Current boot-time mapping (done in boot.s trampoline)

The trampoline builds a 4 GB identity map before entering long mode:

1. PML4[0] → PDPT
2. PDPT[0..3] → PD0..PD3
3. Each PD contains 512 entries mapping 2 MB pages (total: 4 × 512 × 2 MB = 4 GB)
4. Page table memory (24 KB) lives in BSS, 4K-aligned

This maps the entire 32-bit address space including the framebuffer (typically
at ~0xFD000000 in the PCI MMIO aperture).

### Future refinement (Sprint 2+)

`vmm_init()` will build proper 4K page tables with correct permissions:

1. **Identity map the kernel** with read/write, not user-accessible
2. **Map the framebuffer** as present + read/write
3. **Map the WAD module** as read-only
4. Remove the blanket 4 GB identity map and map only what is needed

After this, the MMU will enforce page permissions per region.

### Exokernel syscalls

Once paging is refined and a LibOS address space exists, three syscalls expose
page management to the LibOS:

```c
// Allocate one 4K physical page; returns physical address or -ENOMEM
int64_t exo_page_alloc(void);

// Map a physical page at a virtual address in the caller's PML4
// flags: PAGE_PRESENT | PAGE_WRITE | PAGE_USER
int64_t exo_page_map(uint64_t vaddr, uint64_t paddr, uint64_t flags);

// Unmap a virtual page (does not free the physical page)
int64_t exo_page_unmap(uint64_t vaddr);
```

`exo_page_free` frees the physical page back to the PMM without unmapping it —
the LibOS is expected to call `exo_page_unmap` first.

### LibOS address space

Each LibOS gets its own PML4 (allocated from the kernel PMM). The kernel is
mapped into the upper portion of every LibOS address space (ring 0 only, not
user-accessible) so that syscall entry doesn't require a separate PML4 switch.
The LibOS's own code, heap, and stack live in the lower virtual address range.
The full 64-bit virtual address space provides ample room for separation.

### Page fault handler (SCRUM-17)

Vector 14 (page fault) must be handled before paging refinement begins. On a
fault, the CPU pushes an error code and the faulting address is in `CR2`. The
handler should:

1. Print the faulting virtual address (`CR2`), error code, and `RIP` to serial.
2. Determine if it is a kernel fault (fatal — halt) or a LibOS fault (terminate
   the LibOS, log the fault).

> ⚠️ **Open issue:** The current `default_stub` in `isr.s` does a bare `iretq`
> and cannot handle error-code-pushing exceptions (SCRUM-135). A dedicated
> `error_stub` must be installed on vector 14 before paging refinement begins.

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

**The SCRUM-135 / paging issue.** The page fault handler (vector 14) requires
an `error_stub` (pops error code before `iretq`). That `error_stub` must exist
before paging refinement begins (the current 4 GB identity map from boot works
but enforces no permissions). SCRUM-135 must be closed before Sprint 2 paging
refinement.

**WAD reservation timing.** SCRUM-8 (reserve kernel + WAD pages in PMM) must
complete before SCRUM-7 (bitmap page allocator) ships, or the allocator could
hand out pages that overlap the WAD module. In practice both are in-flight
together; the WAD reservation is part of `pmm_init()` and the two stories should
be merged or sequenced carefully in review.

**64-bit base/length in mmap entries.** The Multiboot 2 mmap uses `uint64_t` for
`base` and `length`. On x86_64, the kernel can address the full 64-bit physical
space, so no regions need to be skipped due to addressing limitations. However,
the current 4 GB identity map only covers the low 4 GB — regions above this
would need additional page table entries to be accessible.

**`serial_flush()` before `qemu_exit()`.** In testing mode, `kernel_main` calls
`serial_flush()` before `qemu_exit()`. This is important: QEMU's
`isa-debug-exit` device triggers an immediate VM shutdown, and any bytes still
in the UART FIFO will be lost. Always flush before exiting if serial output
needs to be captured by CI.
