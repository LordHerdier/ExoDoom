# ExoFS — the LibOS-space filesystem

ExoFS (`src/libos_fs/`) is the team's `cfat` FAT-like filesystem, ported to
run as ExoDoom **LibOS code** over `exo_disk_read`/`exo_disk_write`
(SCRUM-189). It is a library, not a kernel subsystem: the exokernel knows
sectors, and this code is what decides that some of those sectors are a file.
That split is the whole point — see `docs/architecture.md` and
`docs/syscall_spec.md` §3.2 #24, which says in as many words that the disk
syscalls carry "zero filesystem knowledge".

This document is the format contract and the design rationale. The on-disk
structs themselves live in `src/libos_fs/exofs_layout.h`, which is
deliberately plain-old-data with no dependency beyond `<stdint.h>` so
SCRUM-190's host-side image builder can compile against that exact header
rather than restating the format in a script.

> **Status.** Feature-complete for SCRUM-189: the block-I/O seam, the volume
> layer (superblock/format/mount/FAT cache), the FAT layer, the name area,
> directory entries, path resolution, the directory operations
> (mkdir/rmdir/opendir/readdir) and the file operations
> (open/read/write/seek/stat/unlink). What remains on the ticket is wiring
> and documentation, not filesystem behaviour.

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

**One exception: whole-block file appends (SCRUM-233).**
`exofs_fat_alloc_run()` claims a run of consecutive free blocks *without*
zeroing them, and only `exofs_write()`'s append path calls it. It is still
"data before claim", because the claim lives only in the in-RAM FAT until
the next flush. The append writes every byte of the run in one transfer, then
links it onto the chain. If that write fails, it frees the run before anything
can flush. So an interrupted append leaves at most free blocks with new bytes
in them, the same outcome as the "zero, then claim" row above. The point is
cost. Per-block allocation wrote each block twice (zeros, then data) and
walked the whole chain to find its tail, which made the 28 MB IWAD take
minutes to write from the shell's `install`. Directory blocks, partial blocks,
overwrites and writes past the end still go through `exofs_fat_alloc()`.

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

A volume lives on a caller-supplied **block device** and starts at a
caller-supplied base LBA, not at LBA 0. The device (SCRUM-244) is the index
the kernel's `exo_disk_*` syscalls take — 0 is reserved for the system disk,
other disks are 1 and up (`exo_diskstat`, #31, enumerates them). `exofs_format(dev, base, sectors)` and
`exofs_mount(dev, base)` take it, the mounted volume records it
(`exofs_volume_t::dev`), and every transfer goes to it. It is **not** in the
superblock: a volume does not know which disk it is carried on, so the same
volume reads the same on a stick and on the system disk.

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

`exofs_format()` still takes the sector count from its caller — not because
there is no disk-capacity syscall (`exo_diskstat`, #31,
`docs/syscall_spec.md` §3.2, surfaces each device's IDENTIFY sector count,
SCRUM-202/244), but because several fixed-base-LBA
volumes can share one disk (§2 above; the shell's scratch volume and the
WAD's own, `src/doom_wad_disk.h`), so "the whole disk" is never the right
size for one of them. `exofs_region_sectors()` (`src/libos_fs/exofs.h`,
SCRUM-203) is the caller-side computation — `min(next region's base,
disk capacity) - this volume's base` — that the shell's `mkfs`/`disk`
commands use instead of a hardcoded constant. The geometry derived from
whatever sector count results is recorded in the superblock, so
`exofs_mount()` needs only the device and the base.

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

**The FAT lives in RAM; no other block's contents are cached.** The whole
FAT is read at mount and written back per *sector* through a dirty bitmap.
Data and directory blocks are read through on every access.

Chain walks are the hot path, and a FAT lookup that cost a syscall would make
each walk a series of interrupt-disabled disk transfers — `syscall`'s FMASK
clears IF, so every disk syscall runs with interrupts off (`docs/syscall_spec.md`
§3.2 #24). Keeping data blocks uncached means the only dirty-state invariant
in the filesystem is the FAT's single bitmap.

The bitmap is per-sector rather than one flag for the whole FAT because a
128 MiB volume has a 1 MiB FAT: one flag would turn a single-block
allocation into a 2048-sector writeback, with interrupts off throughout.

FAT changes are **marked dirty, not flushed**. The flush belongs to the
operation, not the block — extending a file by 100 blocks should cost one
writeback, not 100 — so callers `exofs_sync()` at the point an operation
completes.

### What is remembered that is not a cache

Two pieces of per-mount state exist purely to avoid work, and neither holds
anything that is on the disk or has to be written back to it — so the FAT's
bitmap is still the only dirty-state invariant there is:

- `next_free_hint` — where the next free-block search starts (§5).
- `name_room[]` — for each block of the name chain, the longest name it can
  still take (§6, SCRUM-223).

Both follow the same rule: **they may only ever be unhelpful, never wrong in
a way that changes an answer.** A stale `next_free_hint` costs a longer scan.
A `name_room` entry is either exact or "unknown", and unknown means "read the
block and see", which is what happened for every block before the table
existed.

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

### One walker per layer

A bound and a corruption check are only as good as the weakest copy of them.
The FAT layer therefore has exactly **one** loop that follows a chain to its
end: `chain_walk()` in `exofs_fat.c`, which reports the last block and the
block count from a single pass. `exofs_chain_last()` and `exofs_chain_len()`
are thin front ends over it (SCRUM-226) — they used to be two hand-copied
loops whose three checks (the `total_blocks` bound, a link into a free block,
a link off the end of the volume) had to be kept identical by care alone.

The contract that walker holds, and that
`test_chain_last_and_len_agree_on_every_shape` /
`test_chain_through_every_block_is_the_longest_legal_one` pin:

- a chain through **every** block of the volume is the longest walk that may
  succeed, and it must succeed — a file or directory that genuinely fills the
  volume has to stay readable;
- anything longer has no end to find, and is `-EXO_EIO`;
- a link into a free block is `-EXO_EIO`; a link off the volume is
  `exofs_fat_get()`'s `-EXO_EINVAL`, passed through;
- on any failure neither output is written.

`exofs_chain_nth()` stays separate on purpose: running out of chain is an
*error* there (`-EXO_EINVAL`, an offset past EOF) rather than the success
condition, so it is a different walk, not a third copy of this one.

The directory layer has the same shape (SCRUM-224). `dir_cursor_next()` in
`exofs_dirent.c` is the only loop that follows a *directory's* chain: it
yields every slot in chain order, live or not, and carries the one cycle
bound and the one "link into a free block" check a directory walk has.
Everything else is a filter over it:

| Caller | Takes |
|---|---|
| `exofs_dir_iter_next()` — the public iterator, and through it `readdir` and the emptiness check | the live slots |
| `dir_scan_for_name()` — behind `exofs_dir_lookup()` and `exofs_dir_add()` | every slot: the live ones for their names, and the first that is not, for a create to use (SCRUM-225) |

Before that, the iterator and the free-slot search were separate
hand-written walks with differently written bounds (`steps > total_blocks`
checked before a read, versus `steps <= total_blocks` wrapped around the
whole per-block body). They happened to agree; nothing made them.
`test_directory_walk_is_bounded_by_the_volume` pins the bound the same way
the FAT tests do — a directory through every block of a (deliberately tiny)
volume must be walkable to its end, and closing it into a loop must be
`-EXO_EIO` from both the iterator and `exofs_dir_add()` after a bounded
number of reads — and `test_free_slots_are_taken_in_chain_order` pins that
the two filters describe the same directory across block boundaries.

Two things about that walker worth knowing before changing it:

- **It reads a block once for as long as its caller keeps the buffer.** The
  cursor records whether the buffer already holds its block.
  `dir_scan_for_name()` owns the buffer for its whole scan. The *public*
  iterator cannot: `exofs_dir_iter_t` is a plain struct on the caller's
  stack with nowhere to keep 512 bytes between calls (and a disk buffer has
  to come from the LibOS window anyway), so it re-reads its block on every
  call — one read per entry returned, not one per 16. Scans that live
  inside `exofs_dirent.c` should hold the cursor themselves rather than go
  through the public iterator, which is what lookup and create now do.
- **There is deliberately no cached tail** for finding a free slot, though
  the FAT allocator and the name area both have a resume point. It could not
  make a create cheaper: `exofs_dir_add()` must check the new name against
  every existing entry, which reads every block of the directory regardless.
  The avoidable cost was making that pass twice — §6b.

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

### What storing a name costs

Placement is **first fit in chain order**: a name goes into the first block
of the name chain that has room for it, and the chain grows only when no
existing block does. That rule has not changed since SCRUM-189. What changed
in SCRUM-223 is what it costs to apply.

The original `exofs_name_alloc()` restarted at the head of the chain on every
call and *read each block from disk* on the way to one with room. The Mth
name stored therefore read every full block before it — O(M²) block reads to
create M files, each read a syscall with interrupts off (§4).

A single "resume here" hint, like the FAT's, does not fix this without
changing the rule, because names are not one size: a block with no room for
a 40-byte name may still have room for a 7-byte one, so "the first block with
room" is a different block for every length. A hint either strands the space
it has moved past or has to go back and rescan it.

So the volume keeps `name_room[]` instead — 2 bytes per name block — and the
allocation loop steps over any block whose entry says the name cannot fit,
without reading it. The walk itself is over the in-RAM FAT. In the steady
state **storing a name reads exactly one block: the one it goes into.**

| | block reads to store 480 mixed-length names (a 21-block chain) |
|---|---|
| before | 5,022 — and the worst single call read all 21 blocks |
| after | 480 |

Three properties make the table safe to consult, and each has a test:

- **It never changes where a name goes.** An entry is written only from a
  block that has just been scanned end to end, by the two functions that are
  the only writers of name blocks, so it is exact. The suite runs one
  scripted workload twice — normally, and with the table wiped before every
  call, which is the old algorithm exactly — and requires every placement to
  match (`the name-room table never changes placement`).
- **A free reaches it.** `exofs_name_free()` updates the entry after its
  write succeeds. Without that, a freed record in a block marked "full" would
  never be looked at again — a leak under save-file rotation
  (`freeing a name reopens a full block`).
- **Corruption is not remembered as "full".** A scan that stops on a bad
  header records nothing, so the block is read and reported `-EXO_EIO` again
  next time (`a corrupt name block is not remembered as full`).

It is per-mount and rebuilt lazily: after a remount the first allocation
reads its way down the chain once, and the second is back to one read. If
the heap cannot supply the table, or a bigger one, allocation carries on
without it — slower, otherwise identical.

One behaviour does differ: a block already known to be too small is no
longer re-read, so damage that appears in it later goes unnoticed until
something needs that block.

---

## 6b. Directories

`.` and `..` are **real entries**, not synthesised during iteration. That is
what lets path resolution handle them with no special case at all: it looks
them up in the directory like any other name, so they mean whatever the
directory says they mean. The cost is two slots and two name records per
directory, plus the obligation that anything which ever *moves* a directory
must fix its `..` — nothing moves directories yet, and a future rename owns
that.

The root's `..` points at the root. There is nowhere above it, and a
self-loop makes `/..` resolve to `/` the way every other filesystem behaves,
without the path code needing to know the root is special.

This is why **`exofs_format()` mounts the volume it has just written**:
creating an entry allocates a name record, which needs the FAT cache and the
superblock's `name_head`, so the root's `.`/`..` cannot be laid down by the
same code that writes raw sectors. Format writes the superblock, mounts,
bootstraps the root, syncs and unmounts. cfat's `createfs()` called
`createRootDirectory()` at the same point for the same reason. A consequence
worth knowing: **a freshly formatted volume has two blocks in use**, the root
and one name block.

`exofs_readdir()` reports `.` and `..`, as POSIX does. They are ordinary
entries here, a shell listing wants them, and filtering them would cost a
name comparison on every iteration to hide something the caller can skip for
free.

### What a create and a lookup cost

A directory is a linear list, so finding a name in it — or establishing that
it is not there, which is what every create has to do first — means reading
the directory. That much is inherent in the format. What is not inherent is
reading it more than once, and until SCRUM-225 a create read it many times
over:

- `exofs_dir_add()` ran a whole `exofs_dir_lookup()` to rule out a
  duplicate, then walked the chain a second time from the head to find a
  free slot.
- That lookup went through the public iterator, which re-reads its block on
  every entry it returns (§5) — so the "one" pass was a read per *entry*,
  not per block.

Both are now a single pass, `dir_scan_for_name()`, which holds the walker
and its buffer: **each directory block is read exactly once**, the first
free slot is noted on the way past at no I/O cost, and a duplicate ends the
scan where it is found. `exofs_dir_lookup()` is the same scan with nothing
to add, so path resolution gets the same saving on every component.

| In one directory of mixed-length names | before | after |
|---|---|---|
| block reads to create 100 files | 6,320 | 1,076 |
| one more create at that size (7 blocks, 102 entries) | 112 | 9 |
| one lookup of a name that is not there | 104 | 7 |

The only reads besides directory blocks are name blocks, one for each entry
whose `name_len` matches — the cheap rejection that keeps most candidates
free.

**The scan does not stop at the first free slot.** That is the one mistake a
merged pass can make that two separate ones could not: a hole that comes
before the name in chain order is met first, and taking it there would put
the same name in the directory twice. The slot is remembered and the scan
runs on. `a create looks past a hole for a duplicate` is that case.

What this does *not* change: a create is still O(entries), so filling one
directory with M files is still O(M²) directory reads — several times fewer
than before, as the table shows, but quadratic. Removing that would take an
index, not a better walk.

**`rmdir` unlinks from the parent before freeing the directory's blocks** —
§1's rule at its sharpest. This order leaks a block chain nothing references
if interrupted; the other leaves the parent naming blocks that are back on
the free list, so the next allocation hands them to another file while a live
directory entry still points there. One is a wasted block, the other is two
files sharing storage.

---

## 6c. Files

A file is a FAT chain of blocks plus a byte count in its directory entry.
Three properties are worth knowing before editing `exofs_file.c`:

**An empty file has no chain.** `first_block` stays `EXOFS_NO_BLOCK` until the
first byte is written, so a volume full of empty files costs no data blocks.
Every write path has to cope with that, which is why writing goes through
`ensure_block()` rather than `exofs_chain_nth()`.

**The chain position is cached in the handle.** Finding the block holding
offset N means walking N/512 links, and re-walking from the head on every
call makes sequential I/O O(blocks²) — for SCRUM-190's ~28 MB IWAD that is
~57000 steps per call by the end of the file. The handle remembers the last
block it touched and its chain index, so a forward step costs one link and
only a backwards seek pays for a re-walk.

**Whole-block appends are batched (SCRUM-233).** When a write reaches a
block-aligned position exactly one past the end of the chain, `append_run()`
claims up to `WRITE_BATCH_BLOCKS` (128) consecutive free blocks with
`exofs_fat_alloc_run()` (§1.1). It writes them with one `exofs_bdev_write()`
through an in-window scratch buffer, and links them on from the cached tail.
That is the write-side mirror of `exofs_read()`'s `READ_BATCH_BLOCKS`. A run
stops at the first occupied block, and the next one picks up in the next gap.
Anything else falls back to one block at a time: an overwrite of existing
blocks, a write past the end that leaves a hole, or a leading or trailing
partial block. `tests/kernel/test_exofs_k.c` pins this: fragmented free space,
overwrite, holes, a full volume, and one disk write per appended block.

**Holes are free.** Seeking past the end and writing leaves a gap that reads
as zeros, with no hole tracking anywhere — every block is zeroed when
allocated (§1.1), so unwritten bytes inside an allocated block are already
zero. The append path's unzeroed runs never contain a hole, because each of
their blocks is written in full, and a write past the end does not take that
path.

### Durability

A file's size and chain head live in its directory entry, rewritten on
`exofs_close()` or `exofs_fflush()` — **not on every write**, which for a
28 MB sequential write would mean one extra whole-block rewrite per call. So
a handle must be closed for its writes to be findable again; an unmounted
volume with an unclosed handle keeps the data blocks but not the size, so
they read as a shorter file. That is the bargain stdio makes with `fclose()`,
and it is worth stating because the failure is silent.

The one exception is `EXOFS_O_TRUNC`, which flushes immediately. Deferring it
would leave the entry naming a chain that is already back on the free list —
the dangling case §1 exists to avoid, and the only place in the file layer
where a deferred metadata write would be unsafe rather than merely lossy.

`unlink` unlinks from the parent before freeing the data chain, the same
ordering and for the same reason as `rmdir` (§6b). It refuses a directory
with `-EXO_EISDIR`: that is `rmdir`'s job, and quietly removing a tree here
is help nobody asked for.

---

## 6d. Descriptors (`exo_file_*`)

`exofs.h` hands out caller-allocated `exofs_file_t` handles and has no
descriptor table on purpose. `exo_file.c` (SCRUM-44) is that table, the layer
a libc-style caller sits on. The `FILE*` shim (SCRUM-42, `src/libos_fs/exo_file_stdio.c`) is built on it:
`exo_file_stdio_install()` points `fopen`/`fread`/`fwrite`/`fseek`/`mkdir` at it,
which is how Doom reads the IWAD (`docs/architecture.md` §7). Like the rest of this directory it is **LibOS code, not a syscall**:
dispatcher numbers #9–16 were once reserved for kernel file syscalls and are
now permanently unbound (`docs/syscall_spec.md` §3.2, §4).

It is deliberately thin. A descriptor is an index into a static array of
`EXO_FILE_MAX_FDS` (8) handles, and each call is the matching `exofs_*` call
on that slot. A slot is free exactly when its handle's `open` is 0, so there
is no second in-use flag to fall out of step. The flag and whence constants
are aliases of `EXOFS_O_*`/`EXOFS_SEEK_*`, so they pass straight through.

**Two "table full" errors, from two tables.** `-EXO_EMFILE` means this
program's descriptor table is full. `-EXO_ENFILE` comes from ExoFS's own
open-entry table (`EXOFS_MAX_OPEN_HANDLES`, also 8) and is passed through
unchanged. The descriptor table is the same size as ExoFS's, so in practice
`-EXO_EMFILE` is the one a caller sees. Making it bigger would only move the
failure to the other table.

**Close before unmount.** Durability is ExoFS's, unchanged (§6c): a size
reaches the disk on close. The descriptor table outlives a mount, so a caller
that unmounts with descriptors open loses those files' sizes, and its next
mount starts with stale slots occupied. `exo_file_close_all()` exists for
exactly that point. Call it before `exofs_unmount()`.

**No mount policy.** `exo_file_*` uses whatever volume the program mounted.
With nothing mounted, ExoFS answers `-EXO_EINVAL` and this layer passes that
through.

**stdio writes are buffered a block at a time.** ExoFS caches no data blocks
(§4), so `src/stdio.c` holds up to 512 bytes per stream (`WBUF_SIZE`, one
`EXOFS_BLOCK_SIZE`) and calls `exo_file_write` when it fills, on `fflush`,
`fseek`, and before an `fread`; a write of a block or more with an empty buffer
goes straight through so ExoFS can batch it (§6c). Doom's savegame code writes
one byte per `fwrite`, which unbuffered would be a sector read-modify-write per
byte. **`fclose` is the durability point**: `fflush` pushes the bytes out but
the directory entry (size, chain head) is only rewritten by `exo_file_close`
(§6c Durability), so close every `FILE` before `exofs_unmount()`.

`remove`/`rename` are not here yet. They are SCRUM-106, over `exofs_unlink`
and a rename ExoFS does not have yet.

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

- **One volume at a time, per program.** The mounted volume is a single
  static state *inside one program's copy of ExoFS*. Since SCRUM-237 that is
  no longer the whole story for the machine: the disk binding is a table of
  non-overlapping sector extents (`docs/syscall_spec.md` §3.5a), so several
  programs can each hold their own extent and mount their own volume at once.
  Each links its own `src/libos_fs/`, so the statics are theirs alone — which
  is the point. A volume must fit inside the extent its holder acquired
  (`exo_disk_extent`, #37, says what that is for a program with an assigned
  volume); every transfer outside it is refused by the kernel, not by ExoFS.
  The reference consumer is `src/libos_voldemo/`: one program, packaged twice
  with different slots, that formats on first launch, mounts thereafter, and
  probes the sectors it must not reach.
- **One volume at a time, even across devices.** A program may hold an extent
  on each of several devices at once (SCRUM-244), but it still mounts one
  volume, so copying a file between two volumes means: hold both, mount the
  source, read the file into a `libos_heap_alloc()` buffer, unmount, mount
  the destination, write. That is what `tests/kernel/test_multidisk_k.c`'s
  copy test asserts, and what `install` (below) does a chunk at a time.
  Making the mount state a handle instead of a static is the change that
  would lift this (SCRUM-265). The shell's `mount` (SCRUM-266, "Mounts"
  below) works around it by swapping the one mounted volume per command.
- **Unmount before releasing the binding.** `exofs_bdev_release(dev)`
  (`exo_disk_release`, #27) drops the binding on that device immediately; it cannot know a
  filesystem was mounted on top of it. The FAT is resident and written back
  a sector at a time as it changes (§1), so releasing with dirty FAT
  sectors still cached loses them silently, and the next holder of the disk
  sees an allocation table that disagrees with the directory entries. Call
  `exofs_unmount()` (which flushes) or at least `exofs_sync()` first. This
  matters for any caller that hands the disk to somebody else and keeps
  running — the shell launching a child that needs the disk is the case
  that motivated #27.
- **No threading.** There is no preemption inside a LibOS today, and a lock
  here would not help if there were — the FAT cache would need one too.
- **`EXOFS_MAX_BLOCKS` = 262144** (a 128 MiB volume, 1 MiB of FAT). The cap
  is really on LibOS heap spent on the resident FAT; 1 MiB is already a
  noticeable share of a 256 MiB QEMU VM. Raising it is one line; paying for
  it in resident memory is the part to think about.
- **Timestamps are `exo_get_ticks()`** — monotonic milliseconds since boot,
  not a wall-clock date. There is no clock to ask. They order writes within
  one boot and nothing more, so nothing should compare them across a mount.

---

### First boot (SCRUM-234)

The shell checks the system volume (the program volume at
`LIBOS_WAD_DISK_BASE_LBA`) once at startup: acquire, `exofs_mount()`, unmount,
release. Only `-EXO_EINVAL` (no superblock) means "unformatted" and gets the
`no system volume -- format? [y/N]` prompt; a busy disk, `-EXO_ENOMEM` or an
I/O error is reported and **never** offers a format. `y` runs `exofs_format()`
over `[LIBOS_WAD_DISK_BASE_LBA, EXO_VOLUME_REGION_BASE_LBA)`, re-mounts to
check, and the shell carries on without a reboot; anything else declines. The
decisions are in `src/shell/shell_firstboot.h` (tested by
`tests/kernel/test_shell_firstboot_k.c`); the prompt itself is manual-test
only. `programs` lists the `*.exo` files on the volume and says `no programs
installed` on a fresh one. A first-boot check must not assert "one block
used": a fresh volume has two (§6b).

To try it, `make docker-run-blank` boots on a zero-filled image the size of
the real one (regenerated every run, `build/exodoom_disk.img` untouched):
expect the prompt, `y`, then `programs` reporting `no programs installed`.

### Install (SCRUM-233)

`install <name>` copies one file from the install disk onto the system
volume; `install` alone lists the install disk. The install disk is block
device 1 (the second IDE drive until USB mass storage, SCRUM-247), with an
ExoFS volume at LBA 0 that `docker/scripts/build.sh` rewrites on every build:
the IWAD plus every `.exo`. A name with no `.` means `<name>.exo`, so
`install doom` is `/doom.exo`; the IWAD is just another file
(`install freedoom2.wad`), not pulled in by the package that needs it. After
a copy, `programs` lists it and `run` launches it, no reboot.

A `*` in the name makes it a pattern over the files at the top of the install
disk. `*` stands for any run of characters, matching is case-sensitive, and
directories never match. The no-extension rule applies first, so
`install *` means `*.exo`, i.e. every package. `install *.wad` brings the
IWAD, and `install *.*` copies everything. The matches are listed in full
before the first copy, up to 64. More than that installs nothing rather than
a truncated list. A file that fails is reported, the rest still copy, and a
summary line closes the run. Matching and listing
(`shell_install_glob_match()`/`shell_install_matches()`) are in
`src/shell/shell_install.c`, tested with the rest of the suite.

The shell holds both disks for the whole copy (device 1 whole, the program
volume's extent on device 0, `docs/syscall_spec.md` §3.5a). Because only one
volume can be mounted (§8), `src/shell/shell_install.c` alternates: mount the
source, read one chunk at its offset, unmount, mount the destination, write it
at the same offset, unmount. Every mount re-reads the resident FAT, so the
shell uses a 1 MiB `libos_heap_alloc()` buffer (64 KiB if the heap cannot
spare that) to keep the IWAD to a few dozen mount pairs. With
`exofs_write()`'s batched appends (§6c) the whole IWAD installs in a few
seconds under QEMU. Before them, it took over three minutes. Two rules a change
here must keep:

- **Stat the source before touching the destination.** A name the install
  disk does not have is `-EXO_ENOENT` with the system volume untouched;
  opening the destination with `EXOFS_O_TRUNC` first would wipe an installed
  package of that name on a typo.
- **A failed copy unlinks the destination.** A half-written package would
  otherwise sit on the volume for `run` to trip over. An existing file of the
  same name is replaced, so a failed *reinstall* leaves no copy at all, not
  the old one.

The copy returns with nothing mounted, so the releases that follow never drop
a binding under a mounted volume. `tests/kernel/test_shell_install_k.c` covers
name resolution and the copy (multi-chunk, exact-multiple and empty files,
reinstall, missing source, `-EXO_ENOSPC` cleanup, directories). A test boot
reformats device 1, so a QEMU line run after `make docker-test` without a
rebuild finds an empty install disk.

### Mounts (SCRUM-266)

`mount diskN:volM <dir>` makes a volume reachable under an existing, empty
directory, so `ls`, `cat`, `cd`, `cp` and the rest work on a volume other than
the shell's scratch one. `umount <dir>` takes it away again, and `mount` alone
lists what is mounted. Mounts are RAM-only: gone at reboot, like the working
directory.

**It is a table in the shell, not an ExoFS feature.** ExoFS still mounts one
volume at a time (§8), so `src/shell/shell_mount.c` keeps a table of
`(directory, volume)` pairs and every path-taking command asks it which volume
its path is on. `shell_route_arg()` in `src/shell/shell_main.c` resolves the
argument against the working directory, normalizes it (`shell_mount_normalize()`
collapses `.`, `..` and `//`), and picks the longest mount point that contains
it. The command then acquires that volume's disk extent, mounts it, runs
against the path *within* the volume, unmounts and releases, as every shell
command already did for the scratch volume. Normalizing first is what makes `..`
work across a mount: ExoFS would resolve `..` through the mounted volume's own
root, which is its own parent, never the directory the volume is mounted on.
The cost is one FAT read per command per volume touched (§4), which is
nothing for `ls` and `cat`. SCRUM-265 is the handle-based ExoFS that would let
volumes stay mounted.

**Volume names.** `diskN` is the block device index (`disk` prints it; disk 0 is
the system disk). `volM` is a fixed position on that disk, counted from 1:

| Disk | volM |
|---|---|
| 0 | 1 = the shell's scratch volume (`mkfs`), 2 = the system volume (programs and the IWAD), 3-10 = per-program volume slots 0-7 (`src/exo_volume.h`) |
| 1 and up | 1 = the volume at LBA 0 (the install disk's) |

A position counts only if a sector at the volume's base holds an ExoFS
superblock (`shell_mount_sb_valid()`, one sector read rather than a mount). A
blank or unformatted position is absent from `disk` and cannot be mounted.
Because numbers are positions, not a running count, `mkfs` creating the shell
volume does not renumber the system volume. A slot held by a running program
is listed as in use and refuses to mount, because its extent is exclusively that
program's (`docs/syscall_spec.md` §3.5a).

**Rules.**

- The mount point must exist, be a directory and hold only `.` and `..`;
  `/` and an existing mount point are refused. So is mounting the shell's own
  volume (`disk0:vol1`, which is `/`): it would put the volume under one of
  its own directories and make every path to it ambiguous. Mounts nest: a directory on a
  mounted volume can itself be a mount point.
- `umount` is refused (`EBUSY`) while the working directory is at or below the
  mount point, or another mount lies beneath it. `shell_cwd` therefore always
  holds the normalized absolute path.
- `rm` and `mv` refuse a mount point itself.
- Read/write, with no read-only mode.
- `cp` and `mv` within a volume are as before. Between volumes they use
  `shell_copy_between()` (the two-path form of `install`'s copy, §8 "Install"):
  chunked, alternating mounts, destination removed on failure. This needs both
  volumes bound at once, and the binding is one extent per owner per device, so
  **a copy between two volumes of the same disk is refused**; between
  different disks it works.
- `mkfs`, `run`, `programs`, `install`, `dd` and first boot keep their fixed
  volumes and ignore the mount table.

Routing, normalization, the table and the superblock probe are in
`tests/kernel/test_shell_mount_k.c`, together with tests that route a path to
the second disk's volume and read a file, the probe on formatted and blanked
volumes, and the two-path copy.

## 9. Source map

| File | What it holds |
|---|---|
| `exofs_layout.h` | The on-disk format. POD only, no dependency beyond `<stdint.h>`, so SCRUM-190's image builder compiles against this exact header rather than restating it. |
| `exofs.h` | The public API and its error contract. |
| `exofs_internal.h` | The mounted volume's in-memory state, shared between ExoFS's own translation units. Not for the image builder. |
| `exofs_blockdev.c/.h` | The only file that knows a syscall exists: acquire/release, chunked sector read/write, and the tick source. ExoFS's single `#ifdef EXO_KERNEL`. |
| `exofs_volume.c` | Superblock, geometry, `format`/`mount`/`sync`/`unmount`, the FAT cache and its writeback. |
| `exofs_fat.c/.h` | Block allocation and chain traversal. |
| `exofs_name.c/.h` | The variable-length name area (§6). |
| `exofs_dirent.c/.h` | Entry storage, the positional iterator, lookup/add/remove. |
| `exofs_path.c/.h` | Path parsing and resolution (§7). |
| `exofs_dir.c/.h` | `mkdir`/`rmdir`/`opendir`/`readdir`, and the `.`/`..` bootstrap (§6b). |
| `exofs_file.c` | `open`/`read`/`write`/`seek`/`stat`/`unlink` (§6c). |
| `exo_file.c/.h` | The descriptor table over `exofs_file.c`: `exo_file_open`/`close`/`read`/`write`/`seek`/`stat`/`close_all` (§6d, SCRUM-44). |
| `tests/kernel/test_exofs_k.c` | The whole suite, 66 tests, ending in the ticket's end-to-end acceptance path. |
| `tests/kernel/test_exo_file_k.c` | The `exo_file` suite: descriptor errors, the access/seek/truncate/append pass-throughs, durability across a remount, and SCRUM-44's acceptance path. |

### Building it

`src/libos_fs/` is a **subdirectory**, which is what keeps it out of
`docker/scripts/build.sh`'s `src/*.c` glob — the same opt-out
`tests/kernel/libos_c_probe/` uses. Its kernel-view compile (`-DEXO_KERNEL`)
is step `[3a/7]`, **unconditional** since SCRUM-190 — `src/syscall_launch.c`'s
`LAUNCH_NEEDS_WAD` path mounts it and reads `/freedoom2.wad` off it on every
normal boot, not just under `TESTING=1` — and a `TESTING=1`-only gate at
step `[3b5/7]` compiles it a second time without `-DEXO_KERNEL` to assert it
still builds as a ring-3 target, with no shipped kernel linking that second
compile.

SCRUM-202 is now the first real ring-3 consumer: the shell LibOS
(`src/shell/shell_main.c`, step `[2c/7]`) links `src/libos_fs/*.c` plus the
small slice of the libc shim it needs (`src/string.c`, `src/libos_heap.c`,
`src/libos_page_alloc.c`) as its `mkfs`/`mkdir`/`ls`/`cat`/`rm` commands,
over its own scratch volume — not the one holding the IWAD.
`exo_file.c` (SCRUM-44, §6d) is in this directory too, so both compiles pick
it up through the `src/libos_fs/*.c` glob and the gate below covers it. The
shell's explicit source list does not include it, because the shell has no
use for descriptors. The sources needed no change to support any of this: the whole point of step `[3b5/7]`'s gate was that
porting ExoFS to a ring-3 target would be "a `build_ring3_link_target` call
and nothing more," and that held.

That claim is enforced rather than asserted. Step `[3b5/7]` compiles the same
sources with the ring-3 `probe_cflags` and no `-DEXO_KERNEL`, then runs
`nm -u` over the objects and **fails the build if any undefined symbol is
neither an `exofs_*` function (which resolves within the library) nor
something a ring-3 target provides** — today `memcpy`/`memset`/`memcmp`/
`strlen` and `libos_heap_alloc`/`_free`. Without the gate ExoFS could pick up
a kernel-only dependency and nobody would find out until the first real ring-3
consumer.

### Pitfalls when writing code against it

- **`exofs_blockdev.c` is the only file with an `#ifdef EXO_KERNEL`**, and it
  is the same dual-compile seam `src/libos_page_alloc.c` documents at length:
  the kernel build calls `exo_syscall_dispatch()` directly so a ring-0 KUnit
  suite can drive the whole filesystem, and the ring-3 build uses the inline
  `syscall` stubs. It is a necessity and not a convenience: a stub's `sysretq`
  forces CPL 3 on return, so a ring-0 caller executing one would drop the rest
  of the boot to ring 3.
- **Disk buffers come from `libos_heap_alloc()`, never `malloc()`.**
  `exo_disk_read` rejects any buffer outside
  `[EXO_USER_VA_BASE, EXO_USER_VA_END)` with `-EXO_EFAULT`, and it does so
  under `EXO_KERNEL` too, because it is the same handler. Under the kernel
  build `malloc()` is `kmalloc()` and returns a kernel address from outside
  that window. This is the single most likely thing to go wrong when adding a
  test.
- **A fresh volume has two blocks in use, not one:** the root directory and
  one name block (§6b — `exofs_format()` mounts what it wrote so it can create
  the root's `.`/`..`, and creating an entry allocates a name record). Keep
  that in mind when asserting on FAT contents or free-block counts.
- **Costs are asserted, not assumed (SCRUM-223).** `exofs_volume_t` counts its
  own data-block transfers (`stat_block_reads`/`stat_block_writes`, bumped in
  `exofs_read_block()`/`exofs_write_block()`), and the suite states costs as
  numbers — "storing a name reads one block" — because every disk read is a
  syscall with interrupts off and a chain re-walk is invisible to a test that
  only checks results. The name allocator is what that first caught (§6).
- **Write ordering is load-bearing** (§1). Allocation zeroes a block before
  claiming it (the append path writes its data before linking instead, §1.1), chain teardown validates before it frees, `format` writes the
  superblock last, and `rmdir`/`unlink` unlink from the parent before freeing
  blocks. Reversing any of them turns a harmless leak into two files sharing
  storage; do not "tidy" them into a more natural-looking order.
