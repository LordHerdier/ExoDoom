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
6b. [Kernel heap](#6b-kernel-heap-scrum-25)
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
Phase 3  page_alloc_init()        Bitmap page allocator (alloc_page / free_page) [Sprint 1]
Phase 3b kmalloc/heap_alloc()     Kernel heap, first-fit, backed by alloc_page [SCRUM-25]
Phase 4  vmm_init()        Kernel page tables from PMM pages + exo_page_map/-unmap [SCRUM-15/-35]
Phase 5  LibOS heap        first-fit allocator backed by exo_page_alloc [Sprint 3]
```

The two structures that bootstrap the PMM itself (the bitmap, the owner
table) are allocated from the bump allocator during boot and never freed;
page tables come from the PMM directly once it is up. Every other kernel
allocation past that point goes through the kernel heap (§6b) — `kmalloc()`
routes to it automatically once the PMM is live.

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
| PMM bitmap (Phase 3)     | `total_pages / 8` bytes, rounded to 4K | `page_alloc_init()` |
| Page owner table (SCRUM-152) | `total_pages * 2` bytes, rounded to 4K | `page_alloc_init()` |

After Phase 3, this bump path is retired for everything except the two
allocations above — `page_alloc_init()` needs it to bootstrap the PMM before
`alloc_page()` exists to serve anyone else. Every later `kmalloc()` call
routes to the kernel heap instead (§6b, SCRUM-25); page tables `vmm_init()`
builds still come from `alloc_page()` directly (4K each, `PAGE_OWNER_KERNEL`),
bypassing the heap the same way they always have.

---

## 6. Phase 3 — Bitmap page allocator

**Files:** `src/page_alloc.c`, `src/page_alloc.h` **Status:** ✅ Done (SCRUM-7
bitmap allocator, SCRUM-152 ownership table); SCRUM-8 (region reservation) in
progress

### Design

The physical memory manager (PMM) tracks 4K pages using a flat bitmap stored in
bump-allocated memory. Each bit represents one physical page: `0` = free, `1` =
used.

```
total_pages  = (top of usable RAM) / 4096
bitmap_size  = ceil(total_pages / 8) bytes   — bump-allocated, 4K aligned
owners_size  = total_pages * sizeof(page_owner_t) bytes  — ditto (SCRUM-152)
```

For QEMU `-m 256M`: 256 MiB / 4K = 65,536 pages → 8,192 bytes (8 KB) bitmap,
plus a 131,072-byte (128 KB) owner table — `page_owner_t` is a `uint16_t`, so
the owner table dominates the bump-allocator budget at 16× the bitmap.

### API

```c
void   page_alloc_init(const struct mb2_info* mb); // build bitmap + owner table, reserve kernel/WAD
void*  alloc_page(void);              // KERNEL-owned page (kernel-internal use)
void   free_page(void* phys_addr);    // free a KERNEL page; detect + log double-free

// Ownership-aware API (SCRUM-152) — see "Ownership table" below.
void*        alloc_page_owned(page_owner_t owner);        // stamp owner on the page
int          free_page_owned(void* phys_addr, page_owner_t owner); // owner-checked free
page_owner_t page_owner(void* phys_addr);                 // query a page's owner

// Revocation API (SCRUM-156) — see "Revocation mark" below.
int          page_revoke_mark(void* phys_addr, page_owner_t owner);  // ask for it back
int          page_revoke_pending(void* phys_addr);        // is it marked?
int          page_reclaim(void* phys_addr, page_owner_t owner);      // take it back
uint32_t     page_reclaim_all(page_owner_t owner);        // sweep a whole context
```

(`alloc_page` / `free_page_checked` are thin `PAGE_OWNER_KERNEL` wrappers over
the owned variants.)

### Initialisation sequence

`page_alloc_init()` must be called after both `mmap_init()` and `memory_init()`:

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

### Ownership table (SCRUM-152)

The bitmap answers *is this page allocated*; it cannot answer *who may free or
map it*. That second question is what turns a physical `alloc`/`mmap` into an
exokernel **secure binding** (`docs/syscall_spec.md` §3.3, architecture.md §2).
So the PMM keeps a second array parallel to the bitmap and sharing its indexing:

```c
typedef uint16_t page_owner_t;      // 16-bit: room for the multi-LibOS future (SCRUM-147)
static page_owner_t* owners;        // owners[i] tags page i

#define PAGE_OWNER_FREE    0        // not allocated
#define PAGE_OWNER_KERNEL  1        // reserved kernel memory / kernel-internal alloc
#define PAGE_OWNER_LIBOS   2        // a LibOS context id (v1 uses this single id)

#define PAGE_OWNER_REVOKED 0x8000   // revocation mark (SCRUM-156), see below
#define PAGE_OWNER_ID_MASK 0x7FFF   // the owner id is the low 15 bits
```

- The table is `kmalloc`'d right after the bitmap in `page_alloc_init` and
  zero-initialised to `PAGE_OWNER_FREE`. Its own backing lives below
  `memory_base_address()`, so it falls inside the reserved range and is tagged
  `KERNEL` — it is never handed out.
- `reserve_region` stamps every page it marks used as `PAGE_OWNER_KERNEL`
  (kernel image, bump pool, WAD module), keeping the bit and the tag in step.
- `alloc_page_owned(owner)` sets the free bit **and** stamps `owner`.
  `alloc_page()` is `alloc_page_owned(PAGE_OWNER_KERNEL)`, so kernel-internal
  pages (e.g. future page tables) can never be freed by a LibOS.
- `free_page_owned(addr, owner)` enforces the tag and returns an ABI-agnostic
  status; the syscall layer (`src/syscall_mem.c`) maps it to an `EXO_E*` code:

  | condition | `free_page_owned` | `exo_page_free` |
  | --- | --- | --- |
  | unaligned / out-of-range address | `PAGE_FREE_EINVAL` | `-EXO_EINVAL` |
  | page bit clear (double free / never allocated) | `PAGE_FREE_EINVAL` | `-EXO_EINVAL` |
  | allocated but owned by KERNEL / another LibOS | `PAGE_FREE_EPERM` | `-EXO_EPERM` |
  | owned by the caller | `PAGE_FREE_OK` (tag → FREE) | `0` |

The "current context" a syscall handler stamps/checks comes from
`syscall_current_context()` (`src/syscall.c`), which returns `PAGE_OWNER_LIBOS`
in v1 and is the hook the SCRUM-147 scheduler will make context-aware. Enforcing
the same tag in `exo_page_map`/`exo_page_unmap` is SCRUM-153; framebuffer
binding is SCRUM-154; reclaiming a terminating context's pages on `exo_exit` is
SCRUM-155.

### Revocation mark (SCRUM-156)

The ownership table answers *who holds this page*. Revocation adds one more
state to it: **the kernel has asked for this page back**. The mark is the top
bit of the tag, so it costs no extra memory and no second array:

```c
owners[i] = PAGE_OWNER_LIBOS | PAGE_OWNER_REVOKED;   // asked for, still owned
```

The low 15 bits still name the owner, which is the whole point — a page under
revocation is *still that context's page* until the kernel takes it, so it stays
readable, writable, freeable by its owner and (SCRUM-153) mappable. Two
consequences for anyone editing `page_alloc.c`:

- **Every ownership comparison masks the bit off first** (the file's
  `owner_id()` helper). A raw tag compare would read a marked page as owned by
  a context nobody can name, and `free_page_owned()` would answer `-EPERM` to
  the page's own owner — making it impossible for a LibOS to comply with the
  request it was just handed.
- **Compliance is free.** `free_page_owned()` resets the tag to
  `PAGE_OWNER_FREE`, which clears owner and mark in one store, so a returned
  page leaves no revocation state to reconcile.

The entry points, all of which resolve the address and check ownership through
one shared helper so a reclaim can never apply a looser rule than the mark did:

```c
int      page_revoke_mark(void* addr, page_owner_t owner);    // phase 1: ask
int      page_revoke_clear(void* addr, page_owner_t owner);   // withdraw the ask
int      page_revoke_pending(void* addr);                     // is it marked?
int      page_reclaim(void* addr, page_owner_t owner);        // phase 3: take it
uint32_t page_reclaim_all(page_owner_t owner);                // sweep one context
uint32_t page_count_owned(page_owner_t owner);                // accounting
```

`page_reclaim` returns `PAGE_REVOKE_ENOENT` — not success — when `owner` no
longer holds the page, so a reclaim can never free a frame that has since been
handed to another context.

`PAGE_OWNER_KERNEL` and `PAGE_OWNER_FREE` are refused as the *holder* argument
by all four: `page_reclaim_all` guards them directly, and the other three
through the shared `owned_page_index()`. The check must precede the ownership
compare, since each id would otherwise pass it — `FREE` matches every free
page's tag and `KERNEL` every reserved page's. Miss it on the single-page path
and `page_reclaim(kp, PAGE_OWNER_KERNEL)` returns the bitmap, this owner table
or the kernel image to the pool, and the kernel's own `free_page()` then
double-frees it.

The protocol that sequences these — and the framebuffer's equivalent — lives in
`src/revoke.c`; `docs/syscall_spec.md` §3.6 is the design note.

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

## 6b. Kernel heap (SCRUM-25)

**Files:** `src/heap.c`, `src/heap.h`, `src/memory.c` **Status:** ✅ Done
**Called from:** `kmalloc`/`kfree`/`krealloc` (`src/memory.c`), once the PMM is
live

### What it does

A first-fit, segmented free-list allocator backed by `alloc_page()`. This is
the kernel-internal heap (Sprint 3) — distinct from the LibOS-side heap in
§8 below, which is Sprint 4 and backed by the `exo_page_alloc` syscall instead
of a direct PMM call.

`kmalloc()`'s original bump-pointer behavior (§5) is still used for the two
allocations `page_alloc_init()` makes to bootstrap the PMM itself (the bitmap
and the owner table) — the heap can't serve those because it depends on
`alloc_page()`, which isn't usable until that bootstrap finishes. Once
`page_alloc_is_live()` is true, `kmalloc()` hands off to `heap_alloc()`
instead, and `kfree()`/`krealloc()` become available. A pointer into the bump
region is permanent kernel infrastructure and is refused by `kfree`/`krealloc`
rather than freed.

### Segments

A "segment" is one run of physically contiguous pages. `alloc_page()` scans
its bitmap forward from index 0, so pages come back adjacent as long as
nothing lower has since been freed — true for a heap that only ever grows,
but not guaranteed once page-freeing lands elsewhere, so segment growth
checks contiguity explicitly: a non-adjacent page starts a new segment
instead of assuming one. First-fit search walks every block in every
segment; a block's `size` never spans a segment boundary, and coalescing
only ever merges blocks within the same segment (its `next`/`prev` pointers
never cross one). A segment's own bookkeeping lives in the first bytes of its
first page, immediately followed by that page's first block header — no
separate metadata allocator is needed to bootstrap it.

### Properties

- Allocations are 16-byte aligned; the acceptance criterion ("aligned
  pointers") this ticket was written against.
- `heap_alloc` grows the heap one page at a time via `alloc_page()` and
  retries the first-fit search, so a multi-page allocation triggers several
  growth-and-retry passes rather than one bulk reservation — simple and
  correct, not throughput-optimized; revisit if profiling later shows it
  matters.
- `heap_free` coalesces with both neighbours in the same segment.
- Freed pages are never returned to the PMM (no segment ever shrinks) — out
  of scope for SCRUM-25's acceptance criteria; a future ticket if heap
  fragmentation or memory pressure makes it worth the complexity of partial-
  segment frees.
- Not thread/interrupt-safe — no locking, consistent with the rest of the
  allocator stack (`page_alloc.c` has none either); fine while allocation
  only ever happens from kernel code running with a single execution
  context.

---

## 7. Phase 4 — Virtual memory and paging

**Files:** `src/vmm.c`, `src/vmm.h`, `src/fault.c`, `src/fault.h`
**Status:** ✅ Done (SCRUM-15 — kernel page tables; SCRUM-35 —
`exo_page_map`/`exo_page_unmap` on top of them; SCRUM-17 — page fault handler);
SCRUM-16 (per-region permissions, WAD read-only) still to do

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

### Two maps, in order

There are two identity maps in the boot, and it matters which one is live:

1. **The boot map** (`boot.s`, described next) — a static 4 GB map in `.bss`
   that exists only to get long mode running.
2. **The kernel map** (`vmm_init()`, SCRUM-15) — built from PMM pages once the
   allocator is up, and loaded into `CR3` in `kernel_main` before the `TESTING`
   branch. From that point on it is the map everything runs against, the ring-3
   probe included.

### Boot-time mapping (done in boot.s trampoline)

The trampoline builds a 4 GB identity map before entering long mode:

1. PML4[0] → PDPT
2. PDPT[0..3] → PD0..PD3
3. Each PD contains 512 entries mapping 2 MB pages (total: 4 × 512 × 2 MB = 4 GB)
4. Page table memory (24 KB) lives in BSS, 4K-aligned

This maps the entire 32-bit address space including the framebuffer (typically
at ~0xFD000000 in the PCI MMIO aperture).

#### U/S bit: supervisor-only, except in test builds (SCRUM-32)

The links and PDEs above are written **without** the U/S bit, so nothing in the
identity map is reachable from ring 3. That is the shipped behaviour and the
right default.

TESTING builds are the exception. `docker/scripts/build.sh` assembles `boot.s`
with `--defsym RING3_PROBE=1`, which sets U/S at *every* level (`0x07` on the
links, `0x87` on the 2 MB PDEs) — the ring-3 syscall probe in
`tests/kernel/ring3_probe.s` cannot execute a single instruction otherwise, and
the U/S bit is only honoured when set at all four levels of the walk.

This is not left to convention. `boot.s` exports the two flag words it
actually used as absolute symbols (`boot_pt_link_flags`, `boot_pt_leaf_flags`,
costing no space in the image), and `build.sh` step `[1b/6]` asserts they match
the build variant — supervisor-only unless `TESTING=1`. A shipped kernel that
somehow acquired the U/S bit fails the build rather than shipping, and a test
kernel that lost it fails loudly instead of triple-faulting into an
unexplained CI timeout. Do not delete those symbols; the check depends on them.

The consequence is blunt: in a test build, all 4 GB of the identity map is
readable and writable from CPL 3, including kernel text and the page tables
themselves. There is no isolation to speak of yet — and the kernel map below
deliberately mirrors the same gate, since it is the map the ring-3 probe
actually runs against. It is gated to test builds so a shipped kernel never
carries it. SCRUM-48 gives each LibOS its own page directory; SCRUM-55
and SCRUM-56 then assert that a LibOS faults on kernel memory and on port I/O.

### The kernel map (`vmm_init`, SCRUM-15)

The boot map is a scaffold, not an address space. It is static (its tables are
`.bss`, not pages the PMM knows about, so nothing can be mapped or unmapped at
runtime), blanket (4 GB of address space, most of which does not exist), and
uniform (one permission for everything). `vmm_init(mb, fb)` replaces it with
tables built from `alloc_page()` pages and loads `CR3` with the result.

What it maps, all identity (virtual == physical), in this order:

| Region | Granularity | Why |
| --- | --- | --- |
| `0x1000`–`0x100000` | 4 KiB | low memory; BIOS/VGA structures |
| `_load_start` → `memory_base_address()` | 4 KiB | kernel image + bump pool (the PMM bitmap and owner table live here) |
| every `MB2_MMAP_AVAILABLE` region | 2 MiB where aligned, else 4 KiB | the PMM pool — page tables included — the WAD module, and normally the multiboot info |
| the multiboot info struct | 4 KiB | firmware may place it outside a usable region, and the kernel reads it after the switch |
| `fb->addr` → `+ pitch × height` | 2 MiB / 4 KiB | the framebuffer sits in the PCI MMIO hole above RAM, so no mmap region covers it — without this the console dies the instant `CR3` is loaded |

Two deliberate omissions:

- **Page 0 is left unmapped.** A NULL dereference faults instead of silently
  reading the interrupt vector table. That fault is still fatal — SCRUM-17
  turned it into a reported halt (CR2, error code, faulting RIP) rather than a
  silent loop, but there is nothing to recover *to* until a LibOS exists to
  terminate.
- **No `NX`.** Bit 63 is reserved while `EFER.NXE` is clear and faults the
  walk. Enabling NXE and marking non-text mappings NX belongs with the
  per-section permissions in SCRUM-16.

On QEMU `-m 256M` with a 1024×768 framebuffer the whole map costs **8 pages
(32 KiB)** of PMM memory — one PML4, one PDPT, two PDs, and four page tables
for the 4 KiB regions; the 254 MB of RAM above 2 MB is 2 MiB leaves. The count
is printed to serial and to the boot console (`vmm_table_pages()`).

Tables are allocated with `alloc_page()`, i.e. `PAGE_OWNER_KERNEL`, so a LibOS
calling `exo_page_free` on one gets `-EXO_EPERM` (SCRUM-152). That is not
incidental: page tables are the one resource where a stray free is
unrecoverable.

#### U/S again: the kernel map mirrors boot.s

`vmm.c` sets the `USER` bit on its leaves and links under `-DTESTING` and
nowhere else — the same gate `build.sh` gives `boot.s` via
`--defsym RING3_PROBE=1`, for the same reason. `vmm_init()` runs *before*
`run_tests()`, so the ring-3 probe executes against the kernel map; if the two
disagreed, a test build would map ring-3 code supervisor-only and triple-fault.
`tests/kernel/test_vmm_k.c` asserts the `USER` bit is present in a test build,
the C-side mirror of `build.sh`'s step 1b.

#### API

```c
int  vmm_init(const struct mb2_info *mb, const struct mb2_tag_framebuffer *fb);
int  vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags);
int  vmm_map_range(uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags);
int  vmm_unmap_page(uint64_t vaddr);
int  vmm_translate(uint64_t vaddr, uint64_t *paddr_out, uint64_t *flags_out);
uint64_t vmm_kernel_pml4(void);
uint32_t vmm_table_pages(void);
```

Status codes are ABI-agnostic like the PMM's (`VMM_OK`, `VMM_ENOMEM`,
`VMM_EINVAL`, `VMM_EEXIST`, `VMM_ENOENT`); the syscall layer maps them to
`EXO_E*` when SCRUM-153 exposes mapping to a LibOS.

Three behaviours worth knowing before building on this:

- **`vmm_map_page` splits 2 MiB leaves on demand.** Mapping a single 4 KiB page
  inside a bulk-mapped region replaces the leaf with a page table describing
  the same 512 pages, then edits the one entry — the neighbours keep their
  mappings. This is what lets `exo_page_map` (SCRUM-153) hand a LibOS one page
  out of a region the kernel mapped in bulk.
- **Remapping is idempotent, repointing is refused.** Mapping an address that
  already resolves to the same physical page succeeds (and updates flags);
  pointing it somewhere else returns `VMM_EEXIST`. Unmap first if that is what
  you meant.
- **Empty page tables are not reclaimed** on unmap. Proving all 512 entries are
  clear on every unmap costs more than the page is worth for a map that is
  built once; per-LibOS address spaces tear down whole trees instead
  (SCRUM-155).

`vmm_init` verifies that the kernel text, the current stack, the PML4, the
multiboot info and the framebuffer all translate correctly **before** writing
`CR3`, and on failure leaves the boot map live and returns an error, so a
missing mapping surfaces as a serial diagnostic rather than a triple fault with
nothing on the wire.

### Still to come (SCRUM-16, SCRUM-48)

1. Per-section kernel permissions: `.text` read-execute, `.rodata` read-only,
   everything else NX (needs `EFER.NXE`).
2. The WAD module mapped read-only.
3. Per-LibOS address spaces: a second PML4 with the kernel half shared.

### Exokernel syscalls

Three syscalls expose page management to the LibOS, implemented in
`src/syscall_mem.c` on top of the primitives above (SCRUM-34, SCRUM-35):

```c
// Allocate one 4K physical page; returns physical address or -ENOMEM
int64_t exo_page_alloc(void);

// Map a physical page at a virtual address in the caller's address space
// flags: EXO_PAGE_READ | EXO_PAGE_WRITE | EXO_PAGE_USER | EXO_PAGE_EXEC
int64_t exo_page_map(uint64_t vaddr, uint64_t paddr, uint32_t flags);

// Unmap a virtual page (does not free the physical page)
int64_t exo_page_unmap(uint64_t vaddr);
```

`exo_page_free` frees the physical page back to the PMM without unmapping it —
the LibOS is expected to call `exo_page_unmap` first.

Both mapping calls are ownership-checked (SCRUM-153) — `exo_page_map` refuses
any `paddr` the caller neither owns nor holds the framebuffer binding for — and both confine `vaddr`
to the **LibOS window**, `[EXO_USER_VA_BASE, EXO_USER_VA_END)` = `[64 TiB,
128 TiB)`. While there is one address space shared with the kernel, a LibOS
that owns a page must still be unable to install it over kernel text; anything
outside the window is `-EXO_EPERM`.

The base is 64 TiB rather than something closer because §7's map is an
*identity* map — every usable RAM region is mapped at `vaddr == paddr`, so any
window starting below the top of physical memory overlaps kernel mappings. See
`docs/syscall_spec.md` §3.7.

### LibOS address space

Each LibOS gets its own PML4 (allocated from the kernel PMM). The kernel is
mapped into the upper portion of every LibOS address space (ring 0 only, not
user-accessible) so that syscall entry doesn't require a separate PML4 switch.
The LibOS's own code, heap, and stack live in the lower virtual address range.
The full 64-bit virtual address space provides ample room for separation.

### Page fault handler (SCRUM-17) ✅

Vector 14 is handled by `pf_stub` (`src/isr.s`) → `page_fault_handler()`
(`src/fault.c`). The stub saves all 15 GPRs in the layout `exception_frame_t`
describes, hands the frame to C, and the handler reports to COM1 and halts:

```
=== PAGE FAULT (#PF, vector 14) ===
  cr2:       0x0000400000005000
  error:     0x0000000000000002  (not-present write supervisor)
  rip:       0x0000000000202BC1
  cs:rsp:    0x0000000000000008:0x0000000000222F10
  rflags:    0x0000000000010087
  mapping:   none (no present entry along the walk)
  context:   ring 0 (kernel) -- fatal
=== halted ===
```

Three things are worth knowing about it:

- **The `mapping:` line walks the live tables** via `vmm_translate()`. That is
  what separates "faulted at X" from "faulted at X, which is unmapped" and from
  "…which is mapped, but supervisor-only" — the distinction that decides
  whether a future ring-3 fault is a missing mapping or a protection failure
  (SCRUM-48/55/56).
- **`idt_init()` now runs early**, right after `memory_init()` and above the
  `TESTING` branch, so a fault during `page_alloc_init()`, `vmm_init()` or the
  test suite is reported rather than looped on.
- **Only ring 0 faults actually reach it.** The handler classifies by the
  saved `CS`'s CPL and prints which ring faulted, but the ring-3 arm is
  written, not exercised: **no TSS is loaded anywhere in the kernel yet**
  (SCRUM-46) and `idt_set_gate` leaves `IST` at 0, so a fault taken at CPL 3
  has no `RSP0` to switch to — the CPU raises `#GP`, then `#DF`, which needs
  the same stack switch, and the machine triple-faults before `pf_stub` runs.
  Gating vector 14 on an IST once the TSS exists is what makes that arm live;
  terminating the faulting LibOS through `revoke_all()` then needs SCRUM-47/48
  on top.

Not covered, for the same reason: a fault taken on a corrupt or unmapped stack
still double-faults, because the handler runs on whatever stack was live. The
recursion guard catches the ordinary case — a fault raised while reporting a
fault — but cannot rescue a bad `RSP`.

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
together; the WAD reservation is part of `page_alloc_init()` and the two stories should
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
