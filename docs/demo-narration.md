# Client demo: narration

SCRUM-239. What to say over each beat of the demo in `docs/demo.md`, for an
audience that has never heard the word *exokernel*. Every beat makes one point,
and the point is the contrast with the operating systems the audience already
uses.

Each beat has:

- **Say**: read aloud, or paraphrase. 30–45 seconds each.
- **Caption**: one line, for a slide or a lower third if the demo is recorded.
- **Point at**: what on screen proves what was just said.
- **If asked**: honest answers to the obvious follow-up questions. Most of
  them are about what the system does *not* do yet. Give them straight.

The spoken parts total about 870 words: just under 6 minutes at 150 words
a minute. Beat numbers match `docs/demo.md`, and the typing for each beat is
listed there.

---

## Opening (before booting)

**Say.** "Every operating system you have used has a kernel that does two jobs.
It protects programs from each other, and it makes decisions for them: what a
file is, how much memory a program gets, what goes on the screen. ExoDoom's
kernel does the first job and as little of the second as it can. It drives
the hardware and hands out raw pieces of the machine (memory pages, disk
sectors, the screen, sound voices), records who owns each one, and checks that
record on every request. The decisions are library code inside the programs
themselves. That design is called an *exokernel*. Here is
what it looks like, starting from a blank disk."

**Caption.** The kernel protects. The programs decide.

---

## Beat 1: New install

**Say.** "This machine's hard disk is blank: every byte on it is zero. The
kernel booted from the CD, and the first thing it started is the shell. The
shell ships in the same boot image, but it is not part of the kernel: it runs
as an ordinary program, in user mode, like every other one. It found no filesystem on the disk, so it offers to
make one. When I press Y, the shell builds that filesystem itself, with library
code linked into it. The kernel's only part is checking that every sector the
shell writes is inside the range the shell was given."

**Caption.** The filesystem is the shell's code, not the kernel's.

**Point at.** `no system volume -- format? [y/N]`, then `system volume
formatted`.

**If asked.**
- *Then what does the kernel know about the disk?* Which program holds which
  range of sectors, and whether it may write them. It does not know about
  files or directories.
- *Is there any filesystem code in the kernel at all?* Yes, one copy of the
  same library. The kernel uses it only to preload the game data for the WAD
  viewer, a program this demo does not run. It enforces nothing.

---

## Beat 2: Install Doom from USB

**Say.** "This second drive is our install stick. `install doom` copies one
file, Doom's *package*, onto the disk we just formatted. It is about half a
megabyte. A package is the program plus a short manifest: the minimum memory
it needs, the memory it would like, and which devices it wants. Next comes
the game data, about 29 megabytes. There is no installer program and no
registry. Installing is a file copy, because the system keeps no list of
programs."

**Caption.** A program arrives as a file. Its needs are written on the label.

**Point at.** `installed /doom.exo`, then `installed /freedoom2.wad (28787748
bytes)`.

**If asked.**
- *Is that a real USB stick?* Not yet. It is a second disk drive standing in
  for one, because the USB storage driver is still to come (SCRUM-247). The
  install path is the same either way: read a file from one disk, write it to
  the other.
- *Is Doom built into the kernel?* The kernel image still carries an older
  built-in copy of Doom from before packages existed. The demo never uses it:
  it runs the package you just watched being installed. Snake, next, has no
  copy in the kernel at all.

---

## Beat 3: Install Snake from USB

**Say.** "Now a second program, and this is the point of the beat. Snake is
not part of the kernel. The kernel has never seen this file and has no list to
add it to. The set of programs on this machine is whatever is on this disk.
Installing one more program needs no change to the system."

**Caption.** The kernel has no list of programs.

**Point at.** `programs` now lists `doom.exo` and `snake.exo`.

---

## Beat 4: Program specs, checked against what is free

**Say.** "When I type `run snake.exo`, the shell reads Snake's manifest. Snake
needs at least 24 pages of memory, a page being 4 kilobytes, and would like
40. It also wants the screen and the keyboard. The shell asks the kernel what
is free and compares the two. If there is too little memory, it refuses. If
there is enough for the minimum but not the recommendation, it starts Snake and
says so. That rule is a small piece of library code the shell links in. It
is not in the kernel, and a different launcher could apply a different rule."

**Caption.** The program states its needs. A library decides, not the kernel.

**Point at.** The serial window:

```
granted: context 4
  pages  20 (asked min 24, rec 40)
  fb     binding free, program may take it
  disk   none
  sound  none asked for
```

**If asked.**
- *Why 20 pages when it asked for 24?* The 20 pages are what Snake holds the
  moment it is built. 24 is what it needs to run, and the shell checked that
  much is free. Snake takes the rest itself as it goes.
- *Does the kernel check anything?* It keeps one last check of its own: it
  refuses a launch it cannot actually build. Whether a program *should* run is
  the shell's call.

---

## Beat 5: Run it

**Say.** "Snake now runs in its own address space, with its own page tables
and its own private copy of the screen to draw into. Every page it holds is
tagged with its owner, and the kernel checks that tag on every request. Snake
cannot touch the shell's memory, and the shell cannot touch Snake's. When I
quit, the kernel takes back everything Snake held."

**Caption.** Raw resources, each with a named owner.

**Point at.** Snake running; Q returns to the shell.

**If asked.**
- *How is that different from Windows or Linux?* The isolation is the same
  goal. The difference is what Snake receives: raw pages and a raw screen
  buffer, managed by Snake's own library code, not files, windows and
  processes the kernel manages for it.

---

## Beat 6: Two programs, two filesystems, one disk

**Say.** "These two are the same program, packaged twice. The only difference
is that one manifest asks for volume slot 0 and the other for slot 1. Each gets
8,192 sectors (4 megabytes) of the same disk and formats its own filesystem
there. Look at the bottom half of the screen. The program tries to reach every
other volume, the shell's area, the program area and the whole disk, and the
kernel refuses every one. Now I run the first copy again. It counts its
second launch: its files survived the other program, and the other program
never saw them. The kernel never knew there were files. It only knows which
sectors belong to whom."

**Caption.** Two filesystems on one disk; the kernel knows only sectors.

**Point at.**
- the `extent` line: `LBA 262144` for A and `LBA 270336` for B;
- the `refused (-22)` column;
- `whole disk: refused (-1)`;
- on the second run of A, `mounted existing volume` and `launches 2`.

**If asked.**
- *On a normal OS?* Both programs would share one filesystem inside the
  kernel, and they would be kept apart by permissions on individual files.
  Here each program has a filesystem of its own, and the kernel keeps them
  apart one level down, at the sectors.

---

## Beat 7: Live dashboard alongside

**Say.** "`top` is just another program. It uses the same calls that any
program can use. Each row is a running program and what it holds: pages, its
disk range, its sound voices, and whether it owns the music. Doom started with
242 pages and now holds about 3,100, roughly 12 megabytes. Doom's own library
asked for every page past those first 242, one at a time, as it needed them,
and every one is on record with its owner. When a program exits, the kernel takes back everything in its
row. The kernel also has the machinery to take a resource back from a
program that is still running: it asks first, and takes the resource only if
the program does not hand it back."

On Ctrl+Tab: "Each program keeps drawing into its own screen buffer. The kernel
shows whichever one is in front, and Ctrl+Tab changes which one that is."

**Caption.** Every grant is visible, and all of it comes back.

**Point at.** The `doom` row: `3086` pages, `16384+245760`, `music yes`.

**If asked.**
- *What if a program crashes?* Today a crash in any program halts the whole
  machine. The clean-up path for a crashed program is not built yet. A program
  that exits normally, as Snake did, is fully reclaimed.
- *What if a program hangs?* Switching is cooperative. A program that never
  calls the kernel keeps the processor, and Ctrl+Tab takes effect at its next
  call. Pre-emptive switching is planned (SCRUM-127).
- *Can `top` take something away from Doom?* No. Nothing in today's system
  takes a resource from a running program; the mechanism exists and is
  exercised by the kernel's own tests (`tests/kernel/test_revoke_k.c`), but
  only a program's exit triggers reclamation in practice.

---

## Closing

**Say.** "So that's the whole system. The kernel did three things: it handed
out pages, sectors, the screen and sound voices, it recorded who owns each
one, and it took them back. Every decision you saw was made by a program.
Formatting the disk, installing, choosing whether to run something, the two
filesystems, and even the dashboard were all programs. That is what makes it
an exokernel, and it is enough to run Doom, an unmodified 1993 game engine, on
a kernel of under a megabyte."

---

## Where each claim comes from

When the system changes, check these before the next presentation.

| Claim | Source |
|---|---|
| The shell formats the disk with its own linked-in ExoFS. The kernel only checks the sector range. | `shell_firstboot_check()` in `src/shell/shell_main.c`; `docs/filesystem.md` §1 |
| The kernel's only ExoFS use is preloading the IWAD for the WAD viewer (its built-in `launch_apps[]` row and `wadview.exo`, the only package asking for `wad`) | `src/doom_wad_disk.c`; `launch_apps[]` in `src/syscall_launch.c`; `package_ring3_target` lines in `docker/scripts/build.sh` |
| The kernel image embeds the shell, Doom and the WAD viewer, and not Snake, `top` or the volume demo | the `_binary_*_start` symbols in `build/exodoom`; `launch_apps[]` in `src/syscall_launch.c` |
| Admission is the shell's decision, and the kernel keeps a backstop | `src/exo_admit.h` header comment |
| Grant read-out figures | `src/exo_grant.h`; the rehearsal serial log (`docs/demo.md`) |
| A program with an assigned volume may bind only that volume | `disk_binding_acquire_extent()` in `src/disk_binding.c` |
| The voldemo copies differ only in volume slot | `package_ring3_target libos_voldemo` lines in `docker/scripts/build.sh` |
| Private framebuffer per program, composited about 60 times a second | `docs/exokernel-architecture.md` §6–§7 |
| Exit reclaims everything; revocation is mark, return, take, and nothing outside the tests triggers it on a running program | `docs/syscall_spec.md` §3.3 "Revocation", §3.6; `docs/exokernel-architecture.md` §6 |
| A ring-3 fault halts the machine | `page_fault_handler()` in `src/fault.c` |
| No pre-emption | `docs/exokernel-architecture.md` §6 "What is not guaranteed" |
| Kernel image is about 935 KiB | `docs/exokernel-architecture.md` §1 |
