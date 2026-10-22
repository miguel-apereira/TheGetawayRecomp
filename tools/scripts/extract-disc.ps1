# SPDX-License-Identifier: MIT
#
# Checks your ISO and extracts SYSTEM.CNF, the boot ELF and the IRX modules
# into iso\extracted (git-ignored). Use -All to extract every file.

param(
    [string]$Iso,
    [string]$OutDir = (Join-Path (Resolve-Path "$PSScriptRoot\..\..").Path 'iso\extracted'),
    [string]$Preset = 'msvc-debug',
    [switch]$All
)

. "$PSScriptRoot\common.ps1"

$isoPath = Find-GameIso $Iso
$disc = Find-BuiltTool 'tgr-disc' $Preset

Invoke-Native $disc @('info', $isoPath)
$extractArgs = @('extract', $isoPath, $OutDir)
if ($All) { $extractArgs += '--all' }
Invoke-Native $disc $extractArgs
