# Client demo: the scripted sequence

SCRUM-121. The demo the client asked for, as a sequence a presenter can run
the same way every time:

**new install → install from USB → specs → run → per-program filesystems →
dashboard.**

Each beat lists what to type, what appears and how long it takes.
`make demo-rehearse` runs the whole sequence unattended and checks it, so run
that before every presentation. What to *say* at each beat (beat 8 of the
client's list) is `docs/demo-narration.md`.

---

## Before you present

1. **Build:** `make docker-build`.
2. **Rehearse:** `make demo-rehearse`. It boots on a blank disk, headless,
   types every beat below over QEMU's QMP socket, and checks the serial log.
   It must end with `rehearsal PASSED`. Screenshots of every beat land in
   `build/demo/`. It takes about 2½ minutes. On 26 October 2026 it passed
   3 runs out of 3, with 11/11 checks and 147 s each time. The last run was
   on `main` at `23380ef`.
3. **Present with `make run-blank`.** This gives a QEMU window plus the serial
   console in the terminal. Arrange the window and the terminal **side by
   side if you want the serial log too; beat 4's resource grant is also
   written to the shell's screen (§"Rules"). Every run starts from a freshly zeroed disk. To start
   over, close QEMU and run it again: it rebuilds, then boots to the format
   prompt in about 8 s.

The "USB stick" is the second IDE drive, `build/exodoom_disk2.img`. The build
writes it on every build with the IWAD and every package. It stands in for a
USB stick until USB mass storage lands (SCRUM-247); say so if asked.

---

## The sequence

| # | Beat | Type | Expect on screen | Expect on serial | Time |
|---|---|---|---|---|---|
| 1 | **New install** | (boot) | `no system volume -- format? [y/N]` | | 8 s |
| | | `y`, then `programs` | `system volume formatted; no programs installed`; `programs` answers `no programs installed` | | 3 s |
| 2 | **Install Doom from USB** | `install` | the install disk: `freedoom2.wad` and every `.exo` the build packaged (`doom.exo`, `snake.exo`, `top.exo`, `voldemoa.exo`, …) | | |
| | | `install doom` | `installing /doom.exo`, then `installed /doom.exo (N bytes)`, about 540 KB | | 5 s |
| | | `install freedoom2.wad` | `installed /freedoom2.wad (28787748 bytes)` | | 10–30 s |
| 3 | **Install Snake from USB** | `install snake`, then `programs` | `installed /snake.exo (N bytes)`, about 9 KB; `programs` lists `doom.exo`, `snake.exo` | | 3 s |
| | *(prep, unnarrated)* | `install voldemoa`, `install voldemob`, `install top` | three `installed` lines | | 5 s |
| 4 | **Program specs vs. what is free** | `run snake.exo` | Snake starts | `granted: context N` / `pages 20 (asked min 24, rec 40)` / `fb …` / `disk none` / `sound none asked for` | 2 s |
| 5 | **Run it** | play, then `q` | Snake idles on `Press any arrow key to start` until an arrow is pressed (arrows steer). If it ends: `GAME OVER`; `R` restarts, any other key returns to the shell | | ~30 s |
| 6 | **Two programs, two filesystems, one disk** | `run voldemoa` | `volume A`, extent `LBA 262144 +8192 sectors`, `first launch -- formatted a new volume`, root lists `launches` and `hello-a.txt`; under `reaching for what is not mine:` volumes B–H, `shell-scratch` and `program-volume` each `refused (-22)`, and `whole disk: refused (-1)` | `voldemo[A] …` lines; grant shows `disk volume slot 0` | 5 s |
| | | `q`, `run voldemob` | the same for `volume B`, `LBA 270336`, `hello-b.txt` | `voldemo[B] …` | 5 s |
| | | `q`, `run voldemoa` | `mounted existing volume`, `launches 2`: A's files survived B, and B never saw them | `voldemo[A] launches=2` | 5 s |
| 7 | **Live dashboard alongside** | `q`, `run top` | the resource dashboard: pages, free contexts and voices, one row per program | | 4 s |
| | | Ctrl+Tab, `run doom` | Doom's title, then its demo | grant: `pages 242 (asked min 246, rec 262)`, `disk volume slot 0, 245760 sectors from 16384`, `sound 8 of 8 voices available` | 20 s |
| | | play; Ctrl+Tab, Ctrl+Tab | `top` again, now with a `doom` row: about 3,100 pages, disk extent `16384+245760`, 2–4 voices (the count moves with the sound effects playing), music `yes` | | |
| | | Ctrl+Tab back to Doom | Doom resumes where it was | | |

---

## Rules the rehearsal taught us

These are why the sequence is in this order. Breaking one makes a beat fail
in front of the client.

1. **Start `top` before Doom.** A running Doom holds the program volume, read
   only, and `run` needs that same volume to read any package. So `run`
   *anything* while Doom is up answers `another program is using the disk -- Ctrl+Tab to it, or quit it first`. Launch the
   dashboard first, then use Ctrl+Tab to reach it. (Extents of different
   owners may not overlap, read-only or not; see `docs/syscall_spec.md` §3.5a.)
2. **The grant is on the shell's screen and on serial.** `run` builds the program, prints what it
   was granted next to what its package asked for (on COM1 and in the shell), then starts it.
   The program takes the screen immediately, so the shell's copy is read when
   the shell is next shown (quit the program, or Ctrl+Tab back); it stays in the
   shell's scrollback. There is no pause.
3. **`pages` in the grant can be below `asked min`, and that is expected.**
   `pages` is what the program holds the moment it is built. `asked min` and
   `rec` are what its package says it needs to run, which the shell checked
   against free memory before launching (`src/exo_admit.h`). The program takes
   the rest itself as it runs: Doom starts with 242 and is near 3,100 by the
   time `top` shows it.
4. **`run` and `install` take the same name.** `run doom` finds `doom.exo`,
   as `install doom` installs it, and case is ignored (`run SNAKE`). A file
   named exactly as typed wins, so `run snake.exo` still works. An unknown
   name prints `run: no such program: <name>` and the installed programs.
5. **Install `freedoom2.wad` as well as `doom.exo`.** The package does not
   bring the game data.
6. **The shell clears its screen when a program starts.** Coming back with
   Ctrl+Tab or Q shows an empty prompt, so earlier output is gone.
7. **Ctrl+Tab cycles through every live program**, shell included, in launch
   order. With `top` and Doom running, two presses get from Doom to `top`.
   `statusbar on` shows a strip along the bottom that lists them, brackets the
   one on screen and says `Ctrl+Tab: next`, so the audience can see what is
   running (SCRUM-278). It is off by default; its 16 rows are reserved either
   way, so `statusbar off` leaves a black strip rather than a taller picture.
8. **Snake waits for the first arrow key.** It draws the board and idles
   (`Press any arrow key to start`) as long as you like, so it is safe to talk
   over the grant output first. `q` or Esc quits from that screen.
9. **Q or Esc quits the small programs.** Doom quits from its own menu
   (Esc → Quit Game), but it never needs to quit during the demo.

## If something goes wrong

| Symptom | Cause | Recovery |
|---|---|---|
| `another program is using the disk` on `run` | Doom is running (rule 1) | Ctrl+Tab to Doom and carry on |
| `run: no such program (try 'programs')` | not installed (rule 4) | `install <name>`, then `run <name>` |
| Doom: `IWAD not found` | `freedoom2.wad` not installed (rule 5) | Ctrl+Tab to the shell, `install freedoom2.wad`, run Doom again |
| A program crashes: `program X stopped: illegal memory access` | The program faulted and the kernel ended it (SCRUM-283); nothing else was affected | Carry on at the prompt. `run crash` does this on purpose if you want to show it (it waits three seconds, so Ctrl+Tab to the shell and back is a good variation): the machine stays up and the prompt returns. It is not in the rehearsal script, which asserts that no page fault ever appears on serial |
| Anything else | | Close QEMU and `make run-blank` again: a rebuild, then the format prompt about 8 s after boot |

---

## The rehearsal itself

`tools/demo/demo.script` is this sequence in the form `tools/demo/qmp_drive.py`
reads:
- `wait`, `type`, `key ctrl-tab` and `shot <name>` drive the sequence;
- `expect <regex>` and `expect-not <regex>` lines are checked against the
  serial log at the end.

`tools/demo/rehearse.sh` boots the ISO headless on a fresh blank disk and runs
it. `make demo-rehearse` passes it the Makefile's own disk-2 and USB device
lists, so it boots the same machine as `make run-blank`. It needs
`qemu-system-x86_64` and `python3` on the host, like `make run`.
The script's checks:

- a resource grant was printed, with min/recommended pages;
- both volume-demo copies formatted their own volume;
- copy A's second launch counted `launches=2`;
- copy B's probe of A's volume and copy A's claim on the whole disk were
  refused;
- Doom found its WAD;
- nothing printed `GRANTED -- isolation broken`, `PAGE FAULT` or `FATAL`.

Change a beat here and in `demo.script` together.
