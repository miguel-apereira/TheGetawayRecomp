# TheGetawayRecomp

A native Windows port of **The Getaway** (PlayStation 2, 2002) built by
static recompilation. The game's MIPS R5900 code is translated ahead of time
to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp), and the PS2
hardware it talked to (GS, VU, IOP, SPU2, IPU) is replaced with native code
on SDL3 and Vulkan instead of being emulated.

> **Status:** early boot, not playable. The recompiled game code compiles,
> links and, on the in-progress `first-boot` branch, boots through the legal
> notice and the Team Soho logo and plays the intro movie. Next goal: the
> main menu. See [Progress](#progress).

## Progress

Done, on `main`:

- Host shell: asks for your ISO, checks it is SCUS-97133, extracts it once
  into a `game` folder, and opens an SDL3 window with a Vulkan swapchain.
- Disc tools: ISO9660 reader and `tgr-disc`, which also lists, verifies and
  extracts the game's `PACK_EE.PAK` and `PACK_IOP.PAK` archives.
- Analysis: the stripped boot ELF analysed in Ghidra with the Emotion Engine
  extension, and the function map committed in `recomp/config/`.
- Recompilation: `ps2_recomp` (our PS2Recomp fork) turns the ELF into C++,
  about 46,000 functions, which compile and link into the exe with MSVC.

In progress, on the [`first-boot`](https://github.com/miguel-apereira/TheGetawayRecomp/tree/first-boot)
branch (it comes to `main` by pull request once the game reaches its main
menu):

- The recompiled game runs on its own thread inside our exe, with a
  `--headless` mode for machines without a working Vulkan window.
- Sony SDK functions are named from PS2Recomp's symbol database, so the
  runtime replaces the game's own SIF, CD, pad, memory card, DMA and GS
  library code.
- Native replacements for the game's own IOP modules: `PFS.IRX` (file
  server, reading from the extracted data and the PAK archives) and
  `STREAM.IRX` (sound engine, silent for now).
- A native IPU with an MPEG-2 intra decoder written from the standard: the
  intro movie plays.
- The game's picture shows in the window (read back from PS2Recomp's
  software GS for now), with gamepad and keyboard input.
- The legal notice and the Team Soho logo render.
- Players no longer need the Vulkan SDK; validation is opt-in with
  `--validation`.

Still to come: a native GS renderer on Vulkan, VU1 microcode, sound (SPU2),
memory card saves, then enhancements. The milestones are listed in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#milestones).

## You need your own copy of the game

This repository contains **no game files**: no ISO, no ELF, no assets, and no
code generated from them. You must supply your own dump of the supported
release, made from a disc you own:

| Title | Region | Serial | Boot ELF |
|---|---|---|---|
| The Getaway | NTSC-U | SCUS-97133 | `SCUS_971.33` |

The PAL releases (SCES-51159, SCES-51426) are not supported: every address
in the recompilation config belongs to one exact executable.

**Players:** on first run, `TheGetawayRecomp.exe` asks you to pick your
`.iso`, checks it, and extracts it once into a `game` folder next to the exe.
Run it with `--reinstall` to extract again, or `--iso <path>` to skip the
file dialog.

**Developers:** also put your dump in `iso/` (for example
`iso/The Getaway.iso`) for the recompilation pipeline. Everything in that
folder is ignored by git. See [iso/README.md](iso/README.md).

## Requirements

- Windows 10 or 11, x64
- Visual Studio 2022 or newer with **Desktop development with C++**
  (includes MSVC, CMake and Ninja)
- Git
- A GPU with Vulkan 1.2 drivers
- Developers only, optional: the [Vulkan SDK](https://vulkan.lunarg.com/)
  for validation layers. Players only need a GPU driver.
- For analysis: [Ghidra](https://ghidra-sre.org/) with the
  [Emotion Engine extension](https://github.com/chaoticgd/ghidra-emotionengine-reloaded),
  and PCSX2 as a reference

## Getting started

From a PowerShell prompt in the repository root:

```powershell
# 1. Fetch the third-party code in external/ at the pinned versions
powershell -ExecutionPolicy Bypass -File tools\scripts\bootstrap.ps1

# 2. Build the host shell, tgr-disc and ps2_recomp
#    (Debug by default; -Preset msvc-release for an optimised build)
powershell -ExecutionPolicy Bypass -File tools\scripts\build.ps1

# 3. Check your disc and extract the boot ELF and IRX modules to iso\extracted
powershell -ExecutionPolicy Bypass -File tools\scripts\extract-disc.ps1

# 4. Turn the boot ELF into C++ in recomp\generated, using the committed config
powershell -ExecutionPolicy Bypass -File tools\scripts\recompile.ps1

# 5. Build again with the generated code compiled and linked in
powershell -ExecutionPolicy Bypass -File tools\scripts\build.ps1 -WithRecompiledCode

# 6. Start the exe (the first run asks for your ISO and extracts it)
powershell -ExecutionPolicy Bypass -File tools\scripts\run.ps1
```

Steps 1 to 3 and 6 alone give the host shell without any game code. The
generated code is large: a clean build with it takes a long while, and the
game runs too slowly in Debug, so use `-Preset msvc-release` to run it
(pass the same `-Preset` to `run.ps1`).

The exe takes `--iso <path>` to skip the file dialog, `--reinstall` to
extract again, `--data <dir>` for another game data folder, and
`--validation` for Vulkan validation layers.

Visual Studio can also open the folder directly and pick up `CMakePresets.json`.

The full recompilation pipeline, including how to redo the Ghidra analysis
that produced `recomp/config/`, is described in [docs/PIPELINE.md](docs/PIPELINE.md).

## Repository layout

| Path | What it holds | In git |
|---|---|---|
| `src/` | Our code: host (SDL3, Vulkan), disc access, hardware replacements (`src/hw`, from `first-boot`), game glue | yes |
| `tools/disc/` | `tgr-disc`: inspect and extract your ISO and its PAK archives | yes |
| `tools/scripts/` | PowerShell scripts for the pipeline | yes |
| `recomp/config/` | PS2Recomp config and function map for SCUS-97133 | yes |
| `recomp/generated/` | C++ generated from your ELF | **no** |
| `external/` | Submodules: PS2Recomp (our fork), SDL, Vulkan-Headers, volk | pinned |
| `iso/` | Your disc image and files extracted from it | **no** |
| `game/` (next to the exe) | The full extraction made on first run | **no** |
| `builds/` | CMake build trees | **no** |
| `docs/` | Architecture and pipeline notes | yes |

## License

The project as a whole, including any build of it, is licensed under the
**GNU General Public License v3.0** ([LICENSE](LICENSE)), because it links
PS2Recomp, which is GPL-3.0.

Files we write ourselves are additionally available under the **MIT License**
([LICENSES/MIT.txt](LICENSES/MIT.txt)) and are marked
`SPDX-License-Identifier: MIT`, so they can be reused on their own.

The Getaway is a trademark of Sony Interactive Entertainment. This project is
not affiliated with or endorsed by Sony or Team Soho.
