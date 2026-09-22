# ExoFS — the LibOS-space filesystem

ExoFS (`src/libos_fs/`) is the team's `cfat` FAT-like filesystem, ported to
run as ExoDoom **LibOS code** over `exo_disk_read`/`exo_disk_write`
(SCRUM-189). It is a library, not a kernel subsystem: the exokernel knows
sectors, and this code is what decides that some of those sectors are a file.
That split is the whole point — see `docs/architecture.md` and
`docs/syscall_spec.md` §3.2 #27, which says in as many words that the disk
syscalls carry "zero filesystem knowledge".

This document is the format contract and the design rationale. The on-disk
structs themselves live in `src/libos_fs/exofs_layout.h`, which is
deliberately plain-old-data with no dependency beyond `<stdint.h>` so
SCRUM-190's host-side image builder can compile against that exact header
rather than restating the format in a script.

> **Status.** Landed so far: the block-I/O seam, the volume layer
> (superblock/format/mount/FAT cache), the FAT layer (allocation, chain
> traversal), the name area, directory entries and path resolution. The
> directory and file operations built on them are in progress under the same
> ticket. Sections below describe what exists.

---

## 1. Ordering invariants

Two write orderings in ExoFS are load-bearing rather than incidental. Both
are about what an **interrupted or failed operation leaves behind**, and both
would look arbitrary to someone rearranging the code for readability. They
are stated here as well as at their call sites because the call-site comment
is the first thing lost in a refactor.

The general rule they are both instances of: **order writes so that a
failure partway through leaves a state the filesystem already knows how to
handle.** There is no journal and no transaction; ordering is the only tool
available.

### 1.1 Allocation zeroes a block before claiming it

`exofs_fat_alloc()` (`src/libos_fs/exofs_fat.c`) writes 512 zero bytes to the
block **and only then** sets its FAT entry to `EXOFS_BLOCK_EOC`.

| Order | If it fails or is interrupted in the middle |
|---|---|
| **zero, then claim** (what ExoFS does) | A zeroed block that is still marked free. That is just a free block — nothing is lost, and the next allocation finds it. |
| claim, then zero | A block owned by a chain, still holding its previous owner's bytes. For a directory block those bytes are read as **live entries**, with stale names and stale `first_block` values pointing into chains that now belong to somebody else. |

This is also why zeroing is not left to the caller as an option. A caller
that forgets it does not get a slightly untidy block; it gets a directory
that appears to contain another file's entries.

### 1.2 Chain teardown validates before it frees

`exofs_chain_free()` walks the chain **twice** — once to validate it end to
end, once to free it — rather than freeing blocks as it walks.

A single pass that frees as it goes will, on a chain containing a cycle,
free some blocks and then discover the corruption. The result is a
half-freed chain: strictly worse than what it started with, and no longer
diagnosable. Validating first means `-EXO_EIO` leaves the corrupt chain
exactly as it was, so a repair tool (or a human with a hex dump) still has
something to look at.

Two `O(n)` passes over an in-RAM array cost nothing worth optimising, and
the alternative — a single pass recording block numbers as it goes — needs
scratch storage proportional to the chain length, which is the one thing a
teardown path should not need to allocate.

### 1.3 Format writes the superblock last

The same rule again, at volume scope: `exofs_format()` writes the FAT and the
root block first and the superblock last. Until that final sector lands
there is no magic on the disk, so `exofs_mount()` refuses the volume — which
is the honest answer for one whose FAT was only half written. The opposite
order yields a *mountable* volume with a garbage FAT.

---

## 2. Geometry

A volume starts at a caller-supplied base LBA, not at LBA 0:

```
base_lba + 0                    superblock (exactly one sector)
base_lba + 1                    FAT, fat_blocks sectors
base_lba + 1 + fat_blocks       data region, total_blocks blocks of 512 B
```

The base is a parameter because the disk is shared —
`tests/kernel/test_ata_k.c` already owns LBA 2048/2049 of the scratch image
and `tests/kernel/test_syscall_disk_k.c` owns LBA 3072 — and because it lets
SCRUM-190 place a volume at a partition-like offset without a format change.

Block size equals sector size (512 B), which is what lets a block index
become an LBA with one addition.

`exofs_format()` takes the sector count from its caller because **there is no
disk-capacity syscall**: `ata_init()` issues IDENTIFY, which carries the
count, but `src/ata.c` does not surface it. Worth a small follow-up. The
geometry derived from that count is recorded in the superblock, so
`exofs_mount()` needs only the base.

---

## 3. Why there is a superblock

cfat had none: geometry was compile-time constants that every reader had to
agree on by convention, and a blank or foreign disk read as a FAT full of
zeros. Mounting a real device means **detecting**, so the superblock carries
magic, version, block size and the full geometry, and `exofs_mount()`
validates each field against another rather than against itself.

That cross-checking is the part that matters. A corrupted sector that happens
to start with the right four bytes must not produce a mounted volume whose
`data_lba` points anywhere at all, so `fat_blocks` must be exactly the size
`total_blocks` implies, and `data_lba` must be exactly where that FAT ends.

---

## 4. Caching

**The FAT lives in RAM; nothing else is cached.** The whole FAT is read at
mount and written back per *sector* through a dirty bitmap. Data and
directory blocks are read through on every access.

Chain walks are the hot path, and a FAT lookup that cost a syscall would make
each walk a series of interrupt-disabled disk transfers — `syscall`'s FMASK
clears IF, so every disk syscall runs with interrupts off (`docs/syscall_spec.md`
§3.2 #27). Keeping data blocks uncached means the only dirty-state invariant
in the filesystem is the FAT's single bitmap.

The bitmap is per-sector rather than one flag for the whole FAT because a
128 MiB volume has a 1 MiB FAT: one flag would turn a single-block
allocation into a 2048-sector writeback, with interrupts off throughout.

FAT changes are **marked dirty, not flushed**. The flush belongs to the
operation, not the block — extending a file by 100 blocks should cost one
writeback, not 100 — so callers `exofs_sync()` at the point an operation
completes.

---

## 5. What ExoFS fixes relative to cfat

Beyond the superblock above, and recorded here because each one is a defect
in the original rather than a matter of taste:

| cfat | ExoFS |
|---|---|
| `exit(1)` from library code on a full disk or missing file | negative `EXO_E*` from every entry point; a library that terminates its caller cannot be used by a shell or by Doom |
| Unbounded chain walks (`while (FAT[i] != USHRT_MAX)`) — a FAT with a cycle hangs forever | every walk bounded by the block count, `-EXO_EIO` on a cycle or a link into a free block. cfat only ever read a file it had written itself; ExoFS reads a real disk |
| `findFreeBlock()` rescans from block 0 per allocation — O(n) each, O(n²) to fill | a wrapping hint resumes where the last search stopped; a freed block moves the hint back to itself |
| Block 0 kept out of the allocator only by `FAT[0]` never reading as free | the search starts at block 1, so the root cannot be handed out even if the on-disk FAT says it is free |
| 16-bit FAT entries, capping a volume at 32 MiB | 32-bit entries. SCRUM-190 must fit a ~28 MB IWAD once the multiboot WAD module is retired |
| `MAXFILENAME 11` (8.3), inline in the directory entry | variable-length names through an indirection (§6) |
| Directory iteration finds "the next entry" by `strcmp`-ing names — O(n) per step, so a full walk is O(n²), and wrong outright with duplicate names | positional `(block, index)` cursor |
| Entry slots never reclaimed; create/delete/create grows a directory forever | `EXOFS_ENT_FREE` slots reused before the directory is extended |
| One entry marked `isLast` terminates a directory — an invariant every insert and delete has to repair, and one that silently truncates the directory if it is ever wrong | no terminator flag at all. A directory's extent is its FAT chain; a slot is live iff its attributes are non-zero and `EXOFS_ENT_FREE` is clear. Early exit was all the flag bought, over 16 entries per block |
| Paths tokenised with `strtok()`, whose state is a single static — and both `findEntryFromPath()` and `findParentFromPath()` use it, so a nested walk silently destroys the outer one | an iterator carrying its own cursor; two walks can interleave, which `test_path_iterator` checks directly |
| Path copied into `char path[MAXPATH]` with `strcpy()` — an unchecked copy of caller-supplied data into a 255-byte stack buffer | the path is never copied; the iterator walks it in place and copies one component at a time into a buffer whose size it is told |
| No cycle detection anywhere | every chain walk bounded, at the FAT layer and again at the directory iterator |

---

## 6. Variable-length names

The ticket's headline fix. A directory entry stays **fixed at 32 bytes** — so
16 per block, and an entry's position is arithmetic rather than a scan —
which is precisely why the name cannot be inline. The entry instead stores
`(name_block, name_off, name_len)` into a **name area** of ordinary
FAT-allocated blocks.

Records never straddle a block boundary, so reading a name is always one
block read. Freed records are marked and reused rather than compacted,
because SCRUM-104's save-file rotation renames constantly and a bump-only
name area would grow without bound.

---

## 7. Paths

**Absolute only.** A path must start with `/`. There is no working directory
in this library and there should not be one: a shell that wants a `cd` keeps
the state itself and passes absolute paths down, which is also the only
arrangement that still works when two callers share a mounted volume.

Repeated separators (`//`) and a trailing `/` are skipped rather than
producing empty components. A component longer than `EXOFS_MAX_NAME` is
**reported, not truncated** — a truncated component resolves to a different
file, which is worse than failing.

`.` and `..` are not special-cased in the path code. They are resolved
through the directory's own entries, so they mean whatever the directory says
they mean, which is what makes `..` work once those entries exist.

Descending through a non-directory is `-EXO_ENOTDIR`, distinct from
`-EXO_ENOENT` for a component that is simply absent: "your path is wrong" and
"it isn't there" are different answers and a caller can act on the
difference.

---

## 8. Limits and scope

- **One volume at a time.** The mounted volume is a single static state.
  This is not a simplification awaiting removal: the disk binding
  (SCRUM-188) is exclusive, so only one context can reach the disk at all,
  and that context has one disk.
- **No threading.** There is no preemption inside a LibOS today, and a lock
  here would not help if there were — the FAT cache would need one too.
- **`EXOFS_MAX_BLOCKS` = 262144** (a 128 MiB volume, 1 MiB of FAT). The cap
  is really on LibOS heap spent on the resident FAT; 1 MiB is already a
  noticeable share of a 256 MiB QEMU VM. Raising it is one line; paying for
  it in resident memory is the part to think about.
- **Timestamps are `exo_get_ticks()`** — monotonic milliseconds since boot,
  not a wall-clock date. There is no clock to ask. They order writes within
  one boot and nothing more, so nothing should compare them across a mount.
