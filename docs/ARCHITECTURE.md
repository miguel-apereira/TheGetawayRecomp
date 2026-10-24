# Architecture

## Principles

- **One supported build:** The Getaway NTSC-U, SCUS-97133, boot ELF
  `SCUS_971.33`. All addresses, stubs and overrides target that executable.
- **Recompiled, not interpreted:** the EE (R5900) code is translated ahead of
  time to C++ by PS2Recomp. VU1 microprograms, which the game uploads at run
  time, are captured once and compiled ahead of time as well.
- **Native hardware replacements:** the GS, VU, IOP, SPU2 and IPU are
  reimplemented for the PC on SDL3 and Vulkan. No PCSX2 code is linked in;
  PCSX2 is only an external reference to compare against.
- **No game data in the repo or in releases:** we ship a prebuilt exe. On
  first run it asks the player for their own ISO, checks it is SCUS-97133,
  and extracts it once into a `game` folder next to the exe. That folder
  also gets `disc_layout.tsv` (LBA, size and path of every file), so the
  game's sector-based reads map back to the extracted files.
- **Port first, enhancements later:** resolution, widescreen, frame rate and
  mouse support come after the game is playable.

## Layers

```
┌──────────────────────────────────────────────┐
│ recomp/generated  (EE code → C++, local only) │
├──────────────────────────────────────────────┤
│ src/game          per-build overrides, fixes │
├──────────────────────────────────────────────┤
│ PS2Recomp runtime memory, context, kernel,   │
│                   syscalls, IOP execution    │
├──────────────────────────────────────────────┤
│ src/hw (planned)  GS · VU0/VU1 · DMAC/VIF/GIF│
│                   SPU2 · IPU · pad · memcard │
├──────────────────────────────────────────────┤
│ src/host          SDL3 window/input/audio,   │
│                   Vulkan renderer            │
│ src/disc          ISO9660 reader, disc check │
└──────────────────────────────────────────────┘
```

## Source tree

| Path | Contents |
|---|---|
| `src/main.cpp` | Entry point: first-run ISO prompt and install, window, main loop |
| `src/app/` | Logging and other app-wide utilities |
| `src/disc/` | `IsoImage` (ISO9660), `SystemCnf`, `checkGameDisc`, `GameData` (one-time extraction), CRC-32 |
| `src/host/` | `VulkanRenderer` (instance, device, swapchain). Audio and input to follow |
| `src/game/` | `GameBuild.h` (serial, ELF name, CRC pin); game overrides will live here |
| `src/hw/` | Native hardware: IPU, VIF0/VIF1, VU microprograms compiled to C++; planned: SPU2, pad and memory card |
| `src/hw/gs/` | Our GS frontend (see docs/GS_RENDERER.md): GIF parser, GS register state, GS inventory, GS dumps |
| `tools/disc/` | `tgr-disc` command line tool |
| `tools/gsreplay/` | `tgr-gsreplay`: replays GS dumps headless through a GS renderer |
| `tests/` | `ctest` tests on hand-written data |

## Milestones

1. Host shell: ISO prompt, disc check, one-time extraction, window, Vulkan swapchain. **(this scaffold)**
2. Boot ELF extracted, analysed in Ghidra, recompiled, and linking.
3. Boots to the first screen.
4. Menus with input.
5. In game with the world rendering.
6. Native VU1, sound and FMV.
7. Saves; full playthrough.
8. Enhancements: resolution, widescreen, frame rate, mouse.
