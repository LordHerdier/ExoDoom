# ExoDoom diagrams

Seven diagrams of how ExoDoom is put together. Each is a [D2](https://d2lang.com)
source (`*.d2`) with a rendered vector (`*.svg`, use this one in documents) and a
2x raster (`*.png`, for slides). Every number, name and ordering in them was
checked against the source tree, not against the prose docs.

Checked against commit `85b769f` (`main`, 26 Oct 2026). If you change the
subsystem a diagram shows, change the diagram in the same PR.

| # | Diagram | Answers |
|---|---|---|
| 1 | [System architecture](01-system-architecture.svg) | What runs in which ring, and what each layer owns |
| 2 | [Memory layout](02-memory-layout.svg) | Where things sit in physical RAM and in one LibOS's address space |
| 3 | [Syscall flow](03-syscall-flow.svg) | What happens between `syscall` and the result coming back |
| 4 | [Interrupt path](04-interrupt-path.svg) | How a device event or a CPU exception reaches C code and returns |
| 5 | [Secure binding](05-secure-binding.svg) | How the kernel tracks, checks and reclaims every resource |
| 6 | [Boot sequence](06-boot-sequence.svg) | The order of `kernel_main`, from GRUB to the shell |
| 7 | [Context switch](07-context-switch.svg) | How programs take turns, and why a switch waits for a syscall |

![System architecture](01-system-architecture.svg)

## Regenerating

```bash
docs/diagrams/render.sh              # all seven
docs/diagrams/render.sh 03-*.d2      # some
```

On NixOS, or anywhere with Nix, the script pulls in `d2`, `librsvg` and the
Source Sans / Source Code fonts on demand and installs nothing. Elsewhere it
needs `d2` and `rsvg-convert` on `PATH` (built with d2 0.9.0 and rsvg-convert 2.62). The SVGs are
byte-for-byte reproducible; the PNGs depend on the installed fonts.

The script lays diagrams out with the ELK engine, which every d2 build bundles.
TALA, d2's other engine, is not used. The PNG is rendered from a copy of the SVG
whose embedded fonts are swapped for the system Source Sans, because rsvg
ignores the SVG's `@font-face` rules.

## Conventions

- **One colour per layer, the same in all seven.** Teal: ring 3 LibOS. Amber:
  Doom. Indigo: ring 0 kernel. Slate: hardware. Dark: a boundary (syscall gate,
  IRQ/port I/O). Red: something that refuses, or a reclaim. `style.d2` defines
  them as classes; every diagram starts with `...@style`.
- **Not to scale** wherever a diagram shows sizes (memory layout).

## Working on the D2 sources

These cost time to find out, so they are written down:

- **Set `grid-rows` and `grid-columns` together** on any grid whose cells must
  line up (tables). With only `grid-columns`, this d2 packs by column and
  evens the heights out, so a cell can land in the wrong row.
- **Every table cell needs a label.** A shape with an empty label is dropped
  from the grid and shifts everything after it; use `"—"`.
- `top`, `left`, `right`, `bottom`, `near`, `width` and `height` are d2
  keywords. A shape named `top` fails with "non-integer top".
- Edges between shapes inside nested grids route fine in a container with
  `direction: right`; across a top-level `grid-columns: 1` stack they do not,
  which is why the layers in diagram 1 are separated by bands instead of arrows.
- Keep box text to about 40 characters a line. d2 does not wrap labels, and a
  line longer than its box runs over the border.
