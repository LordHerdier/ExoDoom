# WAD files

Place your IWAD here before building. The build script looks for:

1. `wads/freedoom2.wad` (checked first)
2. `assets/freedoom2.wad` (fallback)

If neither is found the build will fail with:

```
ERROR: missing WAD — place freedoom2.wad in wads/ or assets/ (see wads/README.md)
```

## What is expected

- Filename: `freedoom2.wad`
- Format: standard Doom IWAD (magic bytes `IWAD` at offset 0)
- Freedoom Phase 2 is a free, open-source IWAD compatible with Doom II.

## What is committed

`wads/freedoom2.wad` is tracked in git. All other `*.wad` files are ignored by
`.gitignore` to prevent accidentally committing copyrighted IWADs.

## Obtaining Freedoom

Freedoom is free software released under a BSD-like license.  Download
`freedoom2.wad` from the official releases page and place it here:

  https://freedoom.github.io/download.html
