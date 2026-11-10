# ACE textures

The layout of MSTS / RTS `.ace` textures as RTS's content has them, read by
`formats/src/ace_file.cpp` and written by `formats/src/ace_writer.cpp`
(`openrts-ace`, 2026-11-10). Survey: the headers of 6000 `.ace` files of
this installation (`GLOBAL`, `ROUTES`, `TRAINS`); the layouts checked
byte by byte on `GLOBAL\TEXTURES\7bridge1.ace`, `7bridge2.ace`,
`A1t27mTurntable.ace`. CONFIRMED (files) unless said otherwise.

## Envelope

- Plain: `SIMISA@@@@@@@@@@` (16 bytes), then the body.
- Packed: `SIMISA@F`, a u32 with the body's size, `@@@@`, then a zlib stream
  (with its header) of the body. Most of RTS's files are packed.

Offsets inside the body count from the body's start (the header), in a
plain file 16 bytes into it.

## Body

- Header, six u32: version (1), options, width, height, format, channels.
- 128 bytes: the tool's text (`compace` writes "Unknown" and "ACE file
  created by compace"); not read.
- Channels: each two u64, its bits (8, or 1 for the mask) and its kind:
  2 mask, 3 red, 4 green, 5 blue, 6 alpha.

Options: bit 0 a mipmap chain, bit 4 (0x10) raw data (DXT1), bit 2 (0x04)
set by the tools on files with a chain. RTS takes the file's levels for a
square texture with options exactly 5 (structured) or 17 / 21 (DXT1),
else builds its own (`0x44F554`, see `ace_file.hpp`).

| Kind | Format | Channels | Options seen |
|---|---|---|---|
| RGB | 0x0E | R G B | 5, 0, 1 |
| RGB + mask | 0x10 | R G B mask | 5, 0 |
| RGBA | 0x11 | R G B mask alpha | 5, 0, 1 |
| DXT1 | 0x12 | R G B, + mask with transparency | 21, 17 |

### Structured (RGB, RGB + mask, RGBA)

A table of u32 row offsets: one per row of the first level, or with a chain
one per row of every level (the levels' heights halving down to 1; the
width halves too, at least 1). Then the rows: each its channels' planes in
the channels' order, an 8-bit one `width` bytes, the mask `(width + 7) / 8`
bytes, bit 7 first, 1 opaque. Some tools write a table that points past
the file (Pavlovsk's `terrain.ace`); the rows follow the table in order
there, which is how the games read them.

### DXT1 (raw)

A table of u32 level offsets, one per level down to 1 x 1. A level of 4 x 4
or more: a u32 byte count, then the 8-byte DXT1 blocks row by row. The 2 x 2
and 1 x 1 levels are not blocks: they are rows as in a structured file
(R G B planes, and the mask when the file has one): 12 and 3 bytes, with a
mask 14 and 4. With a mask channel the three-colour blocks' fourth entry is
transparent (1-bit alpha).

**On the GPU** (openRTS, 2026-11-10): a DXT1 ACE with its own levels
(square, options 17 / 21) goes to the GPU as BC1, its file's blocks as they
are, levels down to 4 x 4 (8x less than RGBA8; RTS gives Direct3D its DXT1
the same way). The GPU's BC1 makes the fourth entry of a three-colour block
transparent black whatever the file says; in the 2026-11 content only the
textures with a mask use it (1325 of 4143 DXT1 files in TRAINSET, Kuban and
Novokuznetsk; the maskless ones never), and a maskless file that did would
go to the GPU as RGBA8. Other ACEs (83 % of the texels there, uncompressed)
and devices without BC textures stay RGBA8. Novokuznetsk's "Скорый поезд 77"
at its start: textures on the GPU 5.6 GB to 3.6 GB.

## openrts-ace

Writes what the table above shows: RGB 0x0E, RGB + mask 0x10, RGBA 0x11
(the mask from alpha >= 128 and the 8-bit alpha), DXT1 0x12 (a mask channel
when any texel's alpha is under 128); a power-of-two image's chain from
RTS's 2 x 2 box filter with options 5 / 21, else none (options 0 / 16:
RTS builds the levels itself); packed by default. Its DXT1 blocks: the
colours' main axis, then a least-squares fit of the ends to the chosen
entries (three rounds). `7bridge2.ace` (DXT1 with a mask) written again
reads back within 0.01 of 255 a channel on average
(`tests/ace_png_test.cpp`). UNKNOWN: whether RTS reads options 16 (raw
without a chain); every DXT1 file in the content has one.
