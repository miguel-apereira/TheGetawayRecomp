# TheGetawayRecomp

A native Windows port of **The Getaway** (PlayStation 2, 2002) built by
static recompilation. The game's MIPS R5900 code is translated ahead of time
to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp), and the PS2
hardware it talked to (GS, VU, IOP, SPU2, IPU) is replaced with native code
on SDL3 and Vulkan instead of being emulated.

> **Status:** exploring. The project builds a host shell that asks for your
> ISO, checks it, extracts it once, and opens a Vulkan window. No game code
> runs yet.

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
- Optional: the [Vulkan SDK](https://vulkan.lunarg.com/) for validation layers
- For analysis: [Ghidra](https://ghidra-sre.org/) with the
  [Emotion Engine extension](https://github.com/chaoticgd/ghidra-emotionengine-reloaded),
  and PCSX2 as a reference

## Getting started

From a PowerShell prompt in the repository root:

```powershell
# 1. Fetch the third-party code in external/ at the pinned versions
powershell -ExecutionPolicy Bypass -File tools\scripts\bootstrap.ps1

# 2. Build (Debug by default; -Preset msvc-release for an optimised build)
powershell -ExecutionPolicy Bypass -File tools\scripts\build.ps1

# 3. Check your disc and extract the boot ELF and IRX modules to iso\extracted
powershell -ExecutionPolicy Bypass -File tools\scripts\extract-disc.ps1

# 4. Start the host shell (the first run asks for your ISO and extracts it)
powershell -ExecutionPolicy Bypass -File tools\scripts\run.ps1
```

Visual Studio can also open the folder directly and pick up `CMakePresets.json`.

The full recompilation pipeline (Ghidra, `ps2_recomp`, building with the
generated code) is described in [docs/PIPELINE.md](docs/PIPELINE.md).

## Repository layout

| Path | What it holds | In git |
|---|---|---|
| `src/` | Our code: host (SDL3, Vulkan), disc access, hardware replacements, game overrides | yes |
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
