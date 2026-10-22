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

## 2. Analyse in Ghidra

1. Install Ghidra and the
   [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded)
   extension.
2. Create the Ghidra project under `recomp/ghidra/` (git-ignored) and import
   `iso/extracted/SCUS_971.33`.
3. Run auto-analysis, then
   `external/PS2Recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.java`.
4. Save the exported TOML and CSV over `recomp/config/SCUS-97133.toml` and
   `recomp/config/SCUS-97133.functions.csv`, keeping the paths in `[general]`
   as they are in the template.

Function maps, stub lists and patch addresses are our own analysis work and
are committed. The disassembly and the Ghidra database are not.

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
into `tgr_recompiled` and links PS2Recomp's runtime.

> PS2Recomp's runtime currently brings its own raylib-based window and
> debug UI. Moving its host side onto our SDL3 + Vulkan layer is one of the
> first integration tasks.
