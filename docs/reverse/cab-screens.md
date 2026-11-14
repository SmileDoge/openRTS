# Cab screens (SCREEN displays)

КЛУБ, САВПЭ, ИТП and similar displays of 3-D cabs are `Display` blocks with
`DisplayType ( SCREEN )`: a flat quad in the cab with sub-objects (pictures,
text, bars, sectors) that a DLL switches and fills. All addresses are
rts1.exe; findings from the decompiled code, on a copy of the database.

## Loading

- **CONFIRMED** `0x51CD88` (cab loader): a display's `InitialState` goes to
  `ActivatorObject::State` (+44). Lamp and screen displays (`Flags` bits
  0x200 / 0x800) take `Colour ( AARRGGBB )` into `ValuesRange` as RGBA
  (white without it). For a screen it allocates an `ActScreen` (76 bytes),
  keeps the pointer in the display's `Granularity` slot (+68) and sets bit 0
  of the cab's flags.
- **CONFIRMED** `0x529F00` (`DisplayScreen`): `TopLeft`, `TopRight`,
  `BottomRight` with x negated (RTS model axes); `Pos` = centre and radius,
  `Pivot` = top left, `Width` = |TL − TR|, `Height` = |TR − BR|, `Up` from
  bottom right to top right, `Norm` = right × up. `ReferenceScreen ( id )`
  (or `ReferenceCloseup`) shows another screen's objects instead of its own.
  `SubObjects ( n` allocates n objects of 204 bytes (ts.h
  `CloseUpSubObject`); `CopySubObjects ( ScreenID | SceneID StartFrom
  NumObjects InsertBefore IncrementID )` copies others. openRTS (2026-11-13,
  `formats/src/cab_description.cpp`): ScreenID copies, NumObjects 0 as all
  from StartFrom, InsertBefore (< 0 or none: at the end), parents and links
  moved with the objects; SceneID and IncrementID are not taken yet. The
  ЭП20's second cab copies all eight of the first cab's screens so (its
  screens were dark in openRTS before).
- **CONFIRMED** `0x519B8C` (sub-object): `Type` FRAME 0, ARROW 1, LINK 2,
  LABEL 3, BAR 4, GRID 5, SECTOR 6; `ParentSub` (+196, stored + 1); `Hint`;
  `Area` / `UV` / `Activation` (x0 y0 x1 y1, 0..1) set flags 0x20 / 0x80 /
  0x40; `Textures ( n files )` relative to the cab file's folder;
  `NamedShader` (+8: Tex 1, TexDiff 2, BlendATex 3, BlendATexDiff 4, Gloss 5,
  AddATex 6, AddATexDiff 7, `0x45B1E0`); `InitialState` → `State` and
  `SetState`; `Flags`: the switch keywords minus 0x20..0x80, plus
  `CONTROLLERY` → 2 and `PRECISE` → 0x200; `AnimFlags`, `AnimTime`;
  `SubFade ( fade blink )` → `Fade[1]`, `Fade[2]` (no SubFade: `Fade[2]` =
  −1); `SubColour` → `Colour` and flag 0x100.
  - ARROW: `Center` → Pt[0..1], `ValuesRange` → Pt[2..5].
  - LINK: `LinkTo` → Pt[0] (−1 without).
  - LABEL: Pt[0] points at a 44-byte font record: `CharacterWidth`,
    `CharacterHeight`, `CharacterSpacing`, `LineSpacing` (screen, texture
    units), `Alignment` (LEFT 0, CENTER 1, RIGHT 2, JUSTIFIED 3, BOTTOM 4,
    TOP 5; `0x45BF44`), characters per texture row = floor((u1 − u0) /
    (width + spacing)), `Alphabet` (the block's first word, `0x4588BC`:
    its end found counting brackets outside quotes, `0x4578C0`, the word up
    to a space or that end, `0x458688`, or a quoted string; brackets inside
    are characters: the EP2D's `0123456789.АБВ…I:-…()+/Ё`), `MaxStringLength`. A texture height ≤ 0
    or no character per row turns the label off. Pt[1] = MaxStringLength + 1
    and the text buffer (`hint`) has that many characters.
  - BAR: `BarStyle` H_LEFT 1, V_UP 2, V_DOWN 3, H_RIGHT 4 → Pt[0];
    ValuesRange → Pt[2..5].
  - GRID: `GridNumRows ( shown texture )` → Pt[0], Pt[1]; `GridNumCols` →
    Pt[2], Pt[3].
  - SECTOR: `SectorShape` ROUND 0 / RECT 1 plus `SectorFillType` STARTPOS
    10 / ENDPOS 20 → Pt[6], `SectorGranularity` → Pt[7], ValuesRange →
    Pt[2..5].
  - `AnimationFrames ( n Frame ( Area Activation UV TexID ) )` (`0x51B6CC`,
    64 bytes each), `AnimTimeScale`.
  - Value starts at min(0, ValuesRange min).

## DLL access

- **CONFIRMED** `_RTSGetCabObject` (`0x536B14`): code 10 → the display's
  `ActScreen` (only for a screen display), code 11 → its sub-object at
  position `sub` (not by ID). ts.h's helpers write `SetState`, `Value`,
  `Area`, `Fade[1..2]`, the label text (`lstrcpynW(hint, text, Pt[1])`) and
  grid bits (`SetState`, bit row × Pt[2] + column).

## Drawing (`0x46B668`)

- **CONFIRMED** A screen display is drawn while its `State` is non-zero (and
  its display blink allows). Depth test on, depth writes off, texturing on.
- Every object, every drawn frame: `State = SetState`. It is drawn when
  `CurrentTex` is a loaded texture, `State ≥ 0` and no parent has a
  negative `State` (`0x52B054`).
- `0x52AAD4`: with animation frames, frame `State` sets the area,
  activation, UV and texture; `PRECISE` takes the frame from `(int) Value`
  and blends two frames' areas by the fraction. A child's area is relative
  to its parent's.
- Shaders Tex, BlendATex, AddATex draw unlit; BlendATex, BlendATexDiff,
  AddATex, AddATexDiff blended.
- Blink: with `Fade[2]` > 0, `Fade[0]` runs modulo 2 × `Fade[2]` and the
  object shows while it is ≤ `Fade[2]`.
- FRAME: the area quad. ARROW: turned about Center by (Pt[4] − v) × Pt[5] /
  (Pt[4] − Pt[2]) degrees, v moving to Value by Pt[5] × dt / AnimTime and
  clamped to the range; turned in the screen's metres (the corners times
  `Width` / `Height` about Center, then `glRotatef`), so a round hub stays
  round on a screen wider than high. BAR: the area cut to (v − min) / (max − min) by
  style; the UV is cut as well unless `STRETCH_UV`. GRID: the State bits;
  the area's cells divided by the texture counts, the texture's by the
  shown counts (as written). SECTOR: a fan from the area's centre over the
  angles the value reaches (STARTPOS: from Pt[4] up to Pt[4] + sweep
  rounded up to the granularity; ENDPOS: down from Pt[5]), 5° steps without
  a granularity; angle 0 is up, clockwise; RECT maps the angle onto the
  square's edge. Nothing at a STARTPOS value ≤ min.
- LABEL: characters found in the alphabet, laid out per line by the
  alignments; with a parent, character sizes scale by the parent's area;
  `\n` (or `\r\n`) starts a line (the line step is not scaled). Unknown
  characters leave a gap.

## openRTS

- `formats::CabScreen` / `ScreenObject` (cab_description) parse it; the
  host builds the `ActScreen` and `CloseUpSubObject`s (`build_screens`),
  answers codes 10 / 11, copies `SetState` to `State` after each step and
  sends states, values, areas and label texts (`CarOutput::screens`).
- `viewer/src/cab_screens` lays the objects out as above;
  `renderer::ScreenQuadRenderer` draws them before the translucent pass (the
  cab glass covers them), a millimetre towards the eye.
- Fixed 2026-11-11 (the EP2D's screens): an `Alphabet` with brackets was
  read as a nested block and lost, so most labels drew nothing (the
  speedometer's "0.00", the clock, the kilometre post); arrows turned in the
  screen's 0..1 units, which sheared them and made the speedometer's round
  hub an oval swinging with the speed.
- The log's report at a capture (`--capture`) lists the labels with a text:
  display/object, the text the DLL wrote, `(hidden)`, and `[drawn/all]`
  when some characters are not in the alphabet (a space in an alphabet
  without one leaves a gap, as in RTS).
- `CopySubObjects` by `ScreenID` (above). The screens are sampled
  anisotropically (16) and half a mip level sharper than RTS samples them
  (`renderer/src/screen_quads.cpp`; testers found the ЭП20's БЛОК blurry).
- **Supersampled screens** (openRTS, 2026-11-14; on by default, "Sharp cab
  screens" in the Graphics panel; off: every object straight onto the
  panel, as RTS). The ЭП2Д's БЛОК (`block1.ace`, 2048 x 2048) is the
  2ЭС6's (`block.ace`, 1024) at twice the size, the same layout (its
  background frame UV 0..0.625 x 0..0.469), but its yellow lines are one
  texel wide against two: on the panel four times thinner, under a fifth
  of a pixel on a Full HD monitor (the 0.23 m screen some 185 pixels
  across for 1280 texels). Its transparent texels are black (the 2ЭС6's
  light beige), so straight-alpha filtering darkened a line twice (its
  colour mixed with the black, then its alpha): a fifth covered, it showed
  about 4 % of its light. The ACE's own mipmaps are a plain 2 x 2 box
  filter (within 0.0004 of openRTS's), not the cause. Now:
  - each screen drawn this frame (`CabScreens::build`'s `ScreenPlane`) is
    drawn into an image of its own (RGBA8UnormSrgb, 5000 pixels a metre
    of screen, 64 to 2048 a side; widened where an object reaches past the
    screen) before the main pass, its objects blended with premultiplied
    alpha in linear light from textures premultiplied in linear light
    (`premultiplied_linear_levels`: the colour times alpha, the mipmaps
    from that, the file's own left);
  - its mipmaps averaged in linear light, then the image laid on the panel
    as one quad (anisotropic 16, half a mip sharper), turned back to the
    display's values and blended as before (straight alpha).

  A line a fifth of a pixel wide now shows about half its brightness (a
  fifth of its light in linear light); turned needles and hard alpha edges
  lose their steps.
- Not done: LINK (2-D scenes), close-up scenes, `CopySubObjects` by
  `SceneID`, the lighting of the `…Diff` shaders.

## The other displays (`0x525DB0`)

- **CONFIRMED** `DisplayFade ( fade blink )` (`0x51CD88`, display `+76`
  / `+80`): with a blink the display counts its clock (`+72`) to twice the
  blink and is hidden in the second half.
- **CONFIRMED** a LAMP display (type bit 2) shows its node while its State
  is set and not blinked off (`0x4B355C(node, fade, on)`).
- **CONFIRMED** a DISPLAY (type bit 4) has `StateNodes ( n "node" ... )`
  (`+52` names, `+56` nodes, `+68` count): node j is shown for bit j of the
  State, or, with flag `0x40000` (`DISPLAY_SEQUENCE`), when the State is
  j + 1; all hidden while blinked off. 202 DISPLAYs in the installed cabs
  (the ЧС4т's lamp groups among them).
- openRTS: `CabView::adjustments` (show / hide; the fade is not done).
- `openrts-train-check --screens` prints what every screen shows;
  `--screens-image <dir>` writes them as PPM images.
