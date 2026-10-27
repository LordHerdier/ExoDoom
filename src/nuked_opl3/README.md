# Nuked-OPL3 (vendored)

`opl3.c`, `opl3.h` and `LICENSE` are [Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3)
by Nuke.YKT, version 1.8, **unmodified**. Licence: LGPL-2.1-or-later (`LICENSE`).

| | |
|---|---|
| Upstream commit | `765ec962e473aeb767e4cba74ffdc8f588ffbfe8` (upstream has no release tags) |
| `opl3.c` sha256 | `59eb873fdb6d52bc7977a0fcbb97c1bae03dd71cc88e3acddbcc01b7121bfe03` |
| `opl3.h` sha256 | `a84266b8d71a4929f15f573afbe407fd24310b77757d855835dacabf68763679` |
| `LICENSE` sha256 | `20c17d8b8c48a600800dfd14f95d5cb9ff47066a9641ddeab48dc54aec96e331` |

It is the YMF262 emulator chocolate-doom uses. It was chosen (SCRUM-217) because
it is integer-only with its default `OPL_ENABLE_STEREOEXT 0`, the only
configuration that pulls in `<math.h>`. That matters because a `TESTING` kernel
compiles it with the kernel's `-mno-sse` CFLAGS. Leave `OPL_ENABLE_STEREOEXT`
alone.

Its one consumer is `src/libos_doom/doom_opl.c`. That file uses `OPL3_Reset`,
`OPL3_WriteReg` and `OPL3_Generate` at the native 49716 Hz and does its own
resampling, and it reads `opl3_chip`'s slot state to detect a silent chip. See
`docs/drivers/hda.md` §12.10.

Do not edit these files. If a change is ever unavoidable, record it here with
the reason, as `docs/drivers/hda.md` §12.8 does for `src/doom/mus2mid.c`.
