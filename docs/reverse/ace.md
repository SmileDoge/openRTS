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
are, levels down to 4 x 4 (8x less than RGBA8; RTS gives OpenGL its DXT1
levels the same way, `glCompressedTexImage2D`, `shape-rendering.md`). The GPU's BC1 makes the fourth entry of a three-colour block
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

## openRTS's block formats (2026-11-17)

Not RTS's: openRTS reads and writes them, RTS cannot open such a file (keep
them out of content meant for RTS). Raw data (options bit 4) like DXT1,
with new format codes:

| Kind | Format | Channels | Block |
|---|---|---|---|
| DXT3 / BC2 | 0x20 | R G B A | 16 bytes: a 4-bit alpha a texel, BC1's colour |
| DXT5 / BC3 | 0x21 | R G B A | 16 bytes: BC4's alpha, BC1's colour |
| BC5 | 0x22 | R G | 16 bytes: two BC4 (a normal map's x and y) |
| BC7 | 0x23 | R G B A | 16 bytes: 8 modes, 1-3 parts a block |
| BC4 | 0x24 | R | 8 bytes: one channel (heights, roughness) |

- The layout as DXT1's: the header, the 128 bytes of text, the channels
  (each 8 bits), a table of u32 level offsets, each level a u32 byte count
  then its blocks row by row (rows of ceil(w / 4)).
- Every level is blocks, down to 1 x 1 (a level under 4 x 4 is one block
  row, its texels past the side repeated): no plain rows as DXT1's 2 x 2
  and 1 x 1 have.
- Options 17 (bit 0: a chain, every level to 1 x 1, any size) or 16 (one
  level). A side need not be a power of two; a GPU takes the blocks as they
  are when level 0's sides are whole blocks (multiples of 4), else the
  decoded texels.
- Reading (`formats/src/ace_file.cpp`): each level's byte count must be its
  blocks' size. The levels are decoded for the CPU (`formats/src/block_compression.cpp`)
  and kept as blocks (`AceRgbaImage::blocks`); the renderer uploads them as
  they are (BC2/BC3/BC4/BC5/BC7 UNORM). A file without a chain gives its
  level 0 decoded, the renderer making the chain (and compressing it as its
  choice of «Несжатые текстуры» says).
- Checked: BC7 decodes to the bit as a GPU does, every mode and partition
  (`tests/bc_gpu_test.cpp`, random blocks read back through a compute
  shader on D3D12). BC1-BC5's levels between the ends are the vendor's:
  measured on NVIDIA, BC1's green up to 5 steps off the ideal
  (2a + b) / 3, BC4's weights floor(k * 64 / 7) and floor(k * 64 / 5) of 64,
  up to 7 steps; the CPU keeps the ideal ones. The testers' 14 PBR cars
  (raw RGBA ACE) written as BC7: the trains' textures on the GPU 795 MB to
  325 MB, a capture beside them 64.8 dB from the originals.

`openrts-ace to-ace --format bc7|dxt5|dxt3|bc5|bc4` writes them (also
`bc1`, `bc2`, `bc3` for DXT1 / DXT3 / DXT5; asked in the console: 5-9), and
says the file is openRTS's only. The encoders: BC7 mode 6 (one RGBA line,
16 levels) or mode 5 where a block's alpha varies (colour and alpha on
lines of their own, the alpha's ends exact, so a cut-out edge stays), BC4
between the block's least and most in 8 levels, BC1's colour as DXT1's.
`to-png` reads them back (a BC4 as grey).
