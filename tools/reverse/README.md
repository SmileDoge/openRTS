# Reverse-engineering scripts

openRTS's own scripts for IDA 9 (Hex-Rays), run without its window, and
two that read what they write. They hold no code of the programs they
look at; their output (decompiled text, `.i64` databases) stays out of
the repository. Findings go to `docs/reverse/` as offsets and behaviour.
Used for `docs/reverse/brakes-dll.md`.

Run a script on a copy of the DLL (never in the installation):

```
idat.exe -A -S"tools/reverse/ida/<script>.py" -L"<dir>/ida.log" "<dir>/<name>.dll"
```

(`idat.exe` from IDA's folder; the second run can take the `.i64` the
first made). The scripts take their settings from the environment:

| Script | Environment | Writes |
|---|---|---|
| `ida/helpers.py` | `OUTD`, `OUTNAME`; `FIRST`, `LAST` (hex range), `LARGEST` (bytes) | the small functions decompiled: find the wrappers of RTS's API |
| `ida/annotate.py` | `OUTD`, `OUTNAME`, `FIRST`, `LAST`; `RENAMES` (`sub_X=name,...`) | every function decompiled, its UTF-16 strings (parameter names) put in place of IDA's labels, the wrappers renamed |
| `ida/wide_xref.py` | `OUTD`, `WORDS` (comma list) | `wide.c`: where each UTF-16 word is used, those functions decompiled |
| `ida/brakes_users.py` | `OUTD` | `brakes_users.txt`: in a DLL that looks `BRAKES` up by name, every line using that library or its variable stack |
| `ida/decompile_eas.py` | `OUTD`, `OUTNAME`, `EAS` (hex list) | the functions at those addresses |
| `ida/grep_decompiled.py` | `OUTD`, `OUTNAME`, `PATTERN` (regex), `LAST` | each decompiled line matching, with its context |

Then, without IDA:
- `python varmap.py annotated.c varmap.txt`: an auxiliary library's
  variable stack (`AuxLibrary +20`) offset by offset (who reads and
  writes it, the masks, the values stored), the `Engine` fields, the
  parameters by function, the SMS triggers by function.
- `python users_summary.py <dir of DLL folders>`: from each folder's
  `brakes_users.txt`, which `BRAKES` offsets each DLL touches and how.
- `python golden_report.py run.tsv [--car N] [--floats 24,40] [--flags 4,8,32]`:
  a golden run (`openrts-train-check --dll ... --record`) phase by phase.
- `python golden_compare.py golden.tsv openrts.tsv [--car N] [--displays 2,3,21]`:
  a run with openRTS's cab brake devices (`--brakes openrts`) against a
  golden run of Brakes.dll, phase by phase: the УР, the brake asked (the
  independent brake or the ЭПТ), the pipe, the cylinder and the displays.
- `openrts-train-check ... --script tests/data/golden/ed9m_klub.script --klub shadow`
  (no Python): openRTS's КЛУБ-У beside Brakes.dll's in the same run, every
  step set against Brakes.dll's variables; prints the steps that differ.
