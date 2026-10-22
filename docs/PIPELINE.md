# Recompilation pipeline

Each developer runs this locally from their own disc. Nothing produced here
is committed except the hand-maintained config in `recomp/config/`.

```
your ISO ──tgr-disc──▶ iso/extracted/SCUS_971.33 (+ IRX modules)
                              │
                        Ghidra + EE extension
                              │  ExportPS2Functions.java
                              ▼
               recomp/config/SCUS-97133.toml + .functions.csv
                              │
                         ps2_recomp
                              ▼
                     recomp/generated/*.cpp
                              │
              build.ps1 -WithRecompiledCode
                              ▼
               builds/<preset>/bin/TheGetawayRecomp.exe
```

## 1. Extract

```powershell
tools\scripts\extract-disc.ps1
```

Runs `tgr-disc info` (prints the boot ELF's CRC32) and then
`tgr-disc extract`. The first time, put the printed CRC32 into
`kBootElfCrc32` in `src/game/GameBuild.h` so other dumps are checked against
it. `tgr-disc ls <iso>` lists every file with its sector (LBA), which is the
starting point for the disc inventory.

Almost all game data is in two archives, `PACK_EE.PAK` and `PACK_IOP.PAK`
(format in [`src/disc/PakArchive.h`](../src/disc/PakArchive.h)). They hold
data only: no ELF, IRX module or code overlay. The VU microcode is in the
boot ELF's `.vutext` and `.DVP.overlay` sections.

```powershell
builds\msvc-debug\bin\tgr-disc.exe pak-ls      "iso\The Getaway.iso"           # every file, sector, size
builds\msvc-debug\bin\tgr-disc.exe pak-verify  "iso\The Getaway.iso"           # decompress + checksum all
builds\msvc-debug\bin\tgr-disc.exe pak-extract "iso\The Getaway.iso" iso\pak   # about 4.9 GB
```

Add `ee` or `iop` to work on one archive, or pass a `.pak` file instead of
the ISO.

## 2. Analyse in Ghidra

1. Install Ghidra and the
   [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded)
   extension. Releases ship a zip per Ghidra version; take the one whose
   name matches yours exactly (for 12.1.4,
   `ghidra_12.1.4_PUBLIC_..._ghidra-emotionengine-reloaded.zip`) and unzip it
   into `%APPDATA%\ghidra\ghidra_<version>_PUBLIC\Extensions`.
2. Run

   ```powershell
   tools\scripts\analyze-ghidra.ps1 -GhidraDir C:\path\to\ghidra_12.1.4_PUBLIC
   ```

   It imports `iso/extracted/SCUS_971.33` into a headless project under
   `recomp/ghidra/` (git-ignored) with the `r5900:LE:32:default` language,
   runs auto-analysis (a few minutes), then runs
   `external/PS2Recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.java` and
   writes `recomp/config/SCUS-97133.toml` and
   `recomp/config/SCUS-97133.functions.csv` with repo-relative paths.
3. When the project already exists the script only re-exports, so renames
   and fixes made in the Ghidra GUI carry into the config. `-Reanalyze`
   throws the project away and starts again.

The ELF is stripped, so almost every function is `FUN_<address>`. The
exporter recognises the EE kernel syscall wrappers (`CreateThread`,
`sceSifSetDma`, ...) and lists them as `stubs`. `.vutext` and the
`.DVP.overlay.*` sections are VU microcode, not EE code, and are not in the
function map.

Function maps, stub lists and patch addresses are our own analysis work and
are committed. The disassembly and the Ghidra database are not.

The script also adds our own settings to the exported TOML, so they survive
a re-export:

- `jalr_indirect_fallback = false`. Without it, `ps2_recomp` turns every
  instruction of a function that calls through a function pointer (`jalr`)
  into a resumable entry, as if the call could land anywhere. An indirect
  call returns to its own resume entry, so only unresolved `jr` jumps get
  that treatment. For SCUS-97133 this takes the fallback from 2,937
  functions (337,695 entries) to 85 (1,796).
- Everything in `recomp/config/SCUS-97133.manual.toml`, appended as is:
  jump tables `ps2_recomp` cannot find on its own, and later patches.

The 85 functions still on the fallback are switch heads that Ghidra split
from their cases with a label (every case is a registered entry, so the jump
is dispatched at run time), one tail call through a global function pointer
(`0x30c264`), and three functions at `0x328968`-`0x329520` that restore
`sp`/`fp` and jump to a saved address. Those look like the C++ exception
unwinder; their landing pads need to be entry points if the game throws.

## 3. Recompile

```powershell
tools\scripts\recompile.ps1
```

Writes C++ to `recomp/generated/`.

## 4. Build with the generated code

```powershell
tools\scripts\build.ps1 -WithRecompiledCode
```

This turns on `TGR_WITH_RECOMPILED_CODE`, which compiles `recomp/generated/`
into `tgr_recompiled` and links PS2Recomp's runtime. The runtime is built
without FFmpeg (FMV will go through our own IPU decoder), without its ImGui
debug panel, and without per-function entry logging. On startup the exe logs
how many guest entry points the generated function table holds.

> PS2Recomp's runtime currently brings its own raylib-based window and
> debug UI. Moving its host side onto our SDL3 + Vulkan layer is one of the
> first integration tasks.
