# ExoDoom performance report

SCRUM-117. Frame times, syscall latency, launch latency and memory overhead,
measured on `main` at `11a455d` (2026-10-26). Every number below comes from a
run that can be repeated with the steps in §6. The raw data is in
`docs/perf/data.json` and the charts are generated from it by
`tools/perf/make_charts.py`.

> **Read the environment first.** These runs used QEMU 8.2 with **TCG, without
> KVM**, on a cloud Xeon host. TCG translates guest code, so absolute times and
> cycle counts depend on the host and are not hardware numbers. What holds
> across hosts is the *shape*: which stage dominates, and by how much. Where
> SCRUM-87 measured the same thing under **KVM**, both are shown side by side.
> Guest milliseconds come from the PIT through `DG_GetTicksMs()`/`exo_get_ticks`.
> Under TCG, QEMU keeps the PIT on host real time.

---

## Summary

| Measure | Result | Target / note |
|---|---|---|
| Doom average frame time | **15.0 ms** (≈66 fps) under TCG; 14.7 ms under KVM | < 30 ms (33 fps) ✅ |
| Doom worst frame per 1 s window | median **29 ms**, p95 **38 ms** | 32% of windows have at least one frame over 30 ms |
| Share of Doom's simulation time spent busy-waiting | **97%** TCG, **99.4%** KVM | The real simulation (`G_Ticker`) costs 0.03–0.26 ms per frame |
| Syscall floor (dispatch + `sysretq`) | **351 cycles** median | `exo_get_ticks` 412 |
| Most expensive syscall | `exo_page_alloc`+`free`, **46,834 cycles** | ~130× the floor: linear bitmap scan |
| Shell launch to banner, slowed CPU | **1350 → 960 ms** guest time, no black frame (SCRUM-192) | ~14 ms at full TCG speed |
| Pages per small app | **787–789** (3.1 MiB), 97% of it the shadow framebuffer | 768 pages fixed per drawing context |
| Doom | **2,908 pages** (11.4 MiB) | 6 MiB of it is Doom's own zone heap |
| Kernel image | 935 KiB (text 149, data 634, bss 153) | data is the embedded Doom, shell and wadview images |

---

## 1. Frame time (Doom)

**Method.** The SCRUM-87 profiler (`src/doom_profile.c`, commits `423046a`
and `d17ea91`) is not on `main`; it lives on the `profiling-resize-slowdown`
branch. For this report those two commits were cherry-picked onto `main` in a
scratch tree and never pushed. The profiler brackets each `doomgeneric_Tick()`
call and prints an average/max line per slot every 35 frames (about one
second). The run booted a normal build, typed `doom` at the shell, and let
Freedoom: Phase 2's attract-mode demo play for 150 s, which gave 281 windows.

![Average frame time split into busy-wait, simulation and sound plus render](perf/frame-breakdown.svg)

| Slot (TCG, 281 windows) | Avg ms (mean) | Avg ms (median) | Window max, median | Window max, p95 | Worst |
|---|---:|---:|---:|---:|---:|
| **total** (`doomgeneric_Tick`) | 15.05 | 14.50 | 29 | 38 | 1252 |
| sim (`TryRunTics`) | 5.06 | 5.00 | 17 | 22 | 30 |
| &nbsp;&nbsp;tic_wait (busy-wait) | 4.90 | 4.90 | 17 | 22 | 30 |
| &nbsp;&nbsp;tic_run (`G_Ticker`) | 0.26 | 0.20 | 1 | 3 | 30 |
| sound (`S_UpdateSounds`) | 3.72 | 3.50 | 9 | 18 | 29 |
| render (`D_Display`) | 6.17 | 5.80 | 8 | 13 | 1193 |
| &nbsp;&nbsp;blit (`DG_DrawFrame`) | 2.24 | 2.20 | 4 | 5 | 7 |

The KVM row in the chart is SCRUM-87's own E1M1 playthrough: total 14.7 ms,
sim 13.45 ms, tic_wait 13.37 ms, tic_run 0.03 ms. That run did not record
sound and render separately, so the chart shows them together as the
remainder (1.25 ms).

**What it shows.**

- **The average is comfortably inside budget, and it is the same under KVM
  and TCG (~15 ms).** That is not a coincidence. `doomgeneric_Tick()` runs in
  an uncapped loop, and `TryRunTics()` spins in `I_Sleep(1)` →
  `DG_SleepMs()` until the next 1/35 s tic is due. A faster CPU therefore
  spends more of each frame waiting rather than finishing sooner: KVM spends
  13.4 ms of its 14.7 ms busy-waiting, TCG 4.9 ms of 15.0 ms. The engine's
  real simulation is 0.03 ms (KVM) to 0.26 ms (TCG) per frame.
- **The worst frame in a window is the number to watch.** The 30 ms budget
  splits the histogram below: 91 of 281 windows (32%) contain at least one
  frame over 30 ms. Spikes show up in every stage, not one: tic_wait reaches
  30 ms, sound 29 ms, and render 13 ms at p95.
- **Level start stalls for over a second.** The 1193 ms render window, and
  the 1252 ms total, happen once, when the demo loads a level and Doom
  caches graphics lumps lazily. SCRUM-87 saw the same thing under KVM
  ("one-off multi-hundred-ms render stall"). It is a loading pause, not a
  per-frame cost.
- **The blit is cheap.** Scaling and converting 640×400 RGBA to 1024×768 BGRX
  costs 2.2 ms (SCRUM-78's path).

![Histogram of per-window worst frame time against the 30 ms budget](perf/frame-max-hist.svg)

---

## 2. Syscall latency

**Method.** `tests/kernel/test_syscall_bench_k.c` (SCRUM-60) times real
ring-3 `syscall`/`sysretq` round trips with `rdtsc`, from inside a launched
LibOS image, with interrupts masked. `docs/syscall_spec.md` §3.8 has the
method in full. The numbers are cycles per call: 5,000 calls per case, and
2,000 for the alloc/free and map/unmap pairs, which are reported per syscall.
Six separate `make docker-test`-equivalent boots on 2026-10-26 gave six
samples per case. The bars show the median, the whiskers the fastest and
slowest run.

![Syscall round-trip cost in cycles, median with range, log scale](perf/syscall-latency.svg)

| Syscall | Median | Fastest | Slowest | §3.8 baseline |
|---|---:|---:|---:|---:|
| dispatcher floor (`-ENOSYS`, #9) | 351 | 332 | 1,889 | 266 |
| `exo_get_ticks` (#5) | 412 | 377 | 482 | 225 |
| `exo_serial_write(len=0)` (#8) | 694 | 630 | 1,119 | 216 |
| `exo_kbd_poll` (empty) (#6) | 908 | 799 | 1,373 | 266 |
| `exo_page_map`+`unmap` (#2/#3) | 2,036 | 1,875 | 2,809 | 943 |
| `exo_fb_acquire` (#4) | 13,892 | 11,635 | 21,168 | 1,385 |
| `exo_page_alloc`+`free` (#0/#1) | 46,834 | 45,506 | 50,716 | 32,914 |

**What it shows.**

- **The cost of entering and leaving the kernel is a few hundred cycles**,
  and most bound syscalls add less than 2× that on top. The exokernel's
  syscalls do almost nothing beyond checking arguments, and it shows.
- **`exo_page_alloc` is the outlier at ~130× the floor.** `src/page_alloc.c`
  scans the bitmap linearly for a free page on every call. Nothing in a hot
  loop allocates pages today. If something ever does, a "next free" hint is
  the fix.
- **`exo_fb_acquire`'s average is not a re-acquire cost.** Since SCRUM-112,
  the first acquire in the loop allocates and zeroes the 3 MiB shadow
  framebuffer. That one-off cost, spread across 5,000 calls, dominates the
  average. The §3.8 baseline predates SCRUM-112.
- **Every row runs 1.3–3.4× the §3.8 baseline.** That table came from a
  different host, and TCG cycle counts follow the host. The *ordering* is
  unchanged, which is the durable result §3.8 itself calls out. The slow
  outliers (1,889 for the floor, 1,373 for `exo_kbd_poll`) are single runs;
  the other five runs of each sit close to the median.

---

## 3. Launch latency

`kernel_main` → shell banner, measured for SCRUM-192 with temporary serial
probes. The guest CPU was slowed with `-icount shift=8` to reproduce what
`make docker-run-kernel` looks like under Docker emulation. Times are guest ms.

| Stage | Before SCRUM-192 | After | What was on screen before |
|---|---:|---:|---|
| `context_create` + `libos_build_image` (+ seed, after) | 362 | 611 | boot log |
| shell: `libos_fb_map()` (768 × `exo_page_map`) | 100 | 100 | **black** |
| shell: `fbcon_init()` → `fb_clear()` | 850 | 214 | **black** |
| banner + help text | 39 | 30 | |
| **total** | **1,351** | **955** | |

At full TCG speed the same path takes ~14 ms. The 768 single-page
`exo_page_map` calls are the largest remaining slice that is not copying
memory. A batched map would remove most of it, but it is an ABI change.
`docs/architecture.md` ("The boot hand-off") covers the fix itself.

---

## 4. Memory overhead

**Fixed kernel cost.**

| Item | Size | Note |
|---|---:|---|
| kernel `.text` | 149 KiB | |
| kernel `.data` | 634 KiB | Almost all of it is the three embedded ring-3 images: Doom (527 KiB), the shell (94 KiB) and wadview (12 KiB) |
| kernel `.bss` | 153 KiB | |
| PMM metadata | 136 KiB per 256 MiB | 1 bit (bitmap) + 2 bytes (`page_owner_t`) per 4 KiB page, so 0.05% of RAM |

**Per context.** `pslist` pages with the shell, `doom`, `wadview`, `top` and
`clock` running (two boots, 256 MiB VM):

![Pages per context split into shadow framebuffer and the rest](perf/memory-per-context.svg)

| Context | Pages | MiB | Shadow framebuffer | Everything else |
|---|---:|---:|---:|---:|
| doom | 2,908 | 11.4 | 768 | 2,140: the 6 MiB zone heap (1,536), the code and data image (133), and 471 more pages of stack and LibOS heap, not broken down here |
| shell (idle) | 966 | 3.8 | 768 | 198 |
| shell (after launching Doom) | 1,030 | 4.0 | 768 | 262: 64 more than idle, not broken down here |
| wadview | 789 | 3.1 | 768 | 21 |
| top | 788 | 3.1 | 768 | 20 |
| clock | 787 | 3.1 | 768 | 19 |

**What it shows.**

- **A LibOS that draws costs 3 MiB before it does anything.** That is its
  private shadow framebuffer (SCRUM-112), 97% of a small app; everything else
  in one is 19–21 pages. The context table holds up to 15 contexts
  (`CONTEXT_MAX`), so shadows alone could reach 45 MiB, 18% of the VM.
- **Doom's footprint is mostly its own policy.** The 6 MiB zone heap is
  `DEFAULT_RAM` in `src/doom/i_system.c`, and the package asks for it.

---

## 5. Bottlenecks, ranked

| # | Bottleneck | Evidence | Fix | Ticket |
|---|---|---|---|---|
| 1 | Byte- and pixel-at-a-time copy/fill loops | `fb_clear()`'s per-pixel loop was 63% of the slowed shell launch (§3). The compositor's 3 MiB copy and shadow zeroing went through `src/string.c`'s byte-wise `memcpy`/`memset` | The framebuffer paths now use `rep movsq`/`stosq`/`stosl` (SCRUM-192). `src/string.c`'s general `memcpy`/`memset`, used by kernel and ring 3 alike, are still byte loops | new |
| 2 | Doom busy-waits for the next tic | 97–99% of sim time (§1) | `DG_SleepMs` should `exo_yield()`; it matters once a second context wants the CPU | SCRUM-201 |
| 3 | Level-load stall | 1.2 s render window (§1) | Prefetch the level's graphics lumps at load | new |
| 4 | `exo_page_alloc` linear bitmap scan | 46.8k cycles, ~130× the floor (§2) | Keep a next-free hint in `page_alloc.c` | new |
| 5 | One syscall per framebuffer page at map time | 100 ms of the slowed launch (§3) | Batched `exo_page_map` (ABI change) | new |
| 6 | 3 MiB shadow per drawing context | 97% of a small app (§4) | Accept, or share one shadow between contexts that never draw at once | — |

---

## 6. Reproducing

```bash
# Syscall latency: part of the normal test boot.
make docker-test | grep syscall_bench

# Frame time: the SCRUM-87 profiler is not on main; apply it to a scratch tree.
git worktree add --detach /tmp/prof main && cd /tmp/prof
git cherry-pick 423046a d17ea91      # resolve tests/kernel/test_runner.c by keeping main's side
make docker-run-kernel               # type `doom` at the shell; profile lines stream on serial

# Charts, after editing docs/perf/data.json:
python3 tools/perf/make_charts.py
```

`pslist` at the shell prints the pages each context owns. `x86_64-elf-size
build/exodoom` prints the kernel's section sizes.
