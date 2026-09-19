# Contributing to ExoDoom

This is the human-facing "how do I contribute" guide: getting set up, branch and
commit conventions, code style, and how to write/run tests. For the full system
design, read `docs/architecture.md` and `docs/syscall_spec.md` first. This guide
assumes you've at least skimmed them.

---

## Getting started

You need Docker (or a Docker-CLI-compatible alternative) and GNU Make. No local
cross-compiler needed. See `README.md`'s "Dependencies" section for the full
rundown of what the Docker images provide.

```bash
make docker-build          # build kernel + ISO
make docker-run-kernel     # build, then boot directly in QEMU (no GRUB) -- fastest dev loop
make docker-test           # build with TESTING=1, boot, stream serial test output
```

If you are testing a timing dependent feature, KVM is recommended. You can run
QEMU directly for this.

```
qemu-system-x86_64 -m 256M -cdrom build/exodoom.iso -no-reboot -serial stdio -enable-kvm
```

`Ctrl+A` then `X` exits QEMU; `Ctrl+A` then `C` opens the QEMU monitor.

---

## Branch and commit conventions

- **One Jira ticket, one branch.** Branches are named `SCRUM-<n>-<short-slug>`
  `<short-slug>` is the ticket summary lowercased, non-alphanumerics collapsed
  to hyphens, trimmed to roughly 5-6 words (e.g.
  `SCRUM-125-contributor-guide-code-style-conventions`).
- **Commit subjects:** `SCRUM-<n>: <imperative summary>`, e.g.
  `SCRUM-41: fixed-point sin/cos/tan/atan and integer floor/ceil`. This is what
  recent history has converged on. Older commits used a bracketed `[SCRUM-n]:`
  form; don't follow that one for new work.
- Intermediate WIP commits on your branch are fine. PRs are squash-merged into
  `main`, so only the final PR title/description need to be clean.
- Open the PR against `main`. CI (below) must be green before merge.

---

## Code style

There's no formatter or linter configured (`.github/workflows/ci.yml` has no
lint step). Style is convention, enforced by review, not tooling.

- **Freestanding C**: `-std=gnu99 -ffreestanding`, no libc beyond this repo's
  own shim (`src/stdlib.c`, `src/stdio.c`, `src/string.c`, `src/ctype.c`, ...).
  Don't reach for a host libc function that isn't already implemented under
  `src/` — check `docs/syscall_spec.md` §2 (the libc dependency audit) and
  `docs/libc_audit.md` first.
- **Kernel CFLAGS matter beyond style**: `-mno-red-zone`, `-mcmodel=small`,
  `-mno-sse -mno-sse2 -mno-mmx` (see `docs/architecture.md` §9 for why each one
  is load-bearing — e.g. dropping `-mno-sse` silently breaks the
  no-FPU-state-to-save invariant documented in `docs/syscall_spec.md` §3.4a).
  Don't add code that assumes floating point, SIMD, or a red zone in anything
  compiled with these flags.
- **Naming**: `snake_case` throughout, one `*.c` / `*.h` pair per subsystem (see
  the subsystem map in `CLAUDE.md`), functions prefixed by their subsystem
  (`vmm_*`, `syscall_*`, `page_*`, ...).
- **Comments explain _why_, not _what_.** Well-named functions and variables
  should make the _what_ obvious; a comment earns its place by recording a
  non-obvious constraint, invariant, or the reason behind a workaround — the
  same rule `CLAUDE.md` holds this project's AI-assisted contributions to, and
  it applies equally to hand-written code.
- **`src/doom/` is vendored, not yours to restyle.** It's doomgeneric's source
  (GPL-2.0), kept as close to upstream as compiles. Don't reformat or "clean up"
  it in passing; changes there should be the minimum needed to fix a real
  compile/link/runtime problem, and should check `docs/libc_audit.md` first for
  whether the gap is already tracked.

---

## Testing

Full guide: `docs/testing.md`. Short version:

1. Add `tests/kernel/test_<name>_k.c` using the KUnit API (`src/kunit.h`,
   CUnit-compatible — `CU_ASSERT_EQUAL`, `CU_ASSERT_STRING_EQUAL`, etc.).
2. Register the suite in `tests/kernel/test_runner.c` via `CU_add_suite(...)` +
   your `suite_*_tests(s)` function.
3. `make docker-test` — no Makefile or build-script changes needed, new
   `tests/kernel/*.c` files are picked up automatically.

Tests run **inside the kernel at boot**, not on the host, so there's no
host/target divergence and no way to run a single test file in isolation —
`make docker-test` always runs the whole suite in one QEMU boot (~6 seconds
currently; see `docs/testing.md`'s "Time budget" section for the hard 30s
ceiling and what happens if a suite blows through it).

---

## What CI checks

`.github/workflows/ci.yml` runs on every push and PR:

1. `make docker-ci` — builds with `TESTING=1`, boots in QEMU, greps serial
   output for `ALL TESTS PASSED`; fails the job if that string is missing or
   `TESTS FAILED` appears.
2. `make docker-build-doom` — compiles all 79 files under `src/doom/` and fails
   if any of them stop compiling. This is a **compile gate only**: nothing under
   `src/doom/` links into `build/exodoom` yet, so a green check here means the
   vendored engine and the libc shim's headers still agree, nothing more.

Both must pass before merge.

---

## Where to read more

- `docs/architecture.md` — full system design, subsystem map, sprint roadmap
  (§10 tells you what's actually in progress vs. planned).
- `docs/syscall_spec.md` — the syscall ABI and libc dependency audit; source of
  truth for anything touching the syscall gate.
- `docs/memory.md` — paging, allocators, address-space layout.
- `docs/debugging.md` — GDB setup and bare-metal-specific debugging tips.
- `docs/testing.md` — the full KUnit guide referenced above.
- `docs/drivers/` — per-driver notes (IDT, PIC, PIT, serial, keyboard,
  framebuffer).

Read the section relevant to what you're touching before making a non-trivial
change — these docs are kept up to date per sprint and usually contain the "why"
that isn't in the code itself.
