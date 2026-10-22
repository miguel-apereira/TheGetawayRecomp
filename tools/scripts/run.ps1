# SPDX-License-Identifier: MIT
# Starts the game. On first run it asks for your ISO (or uses -Iso <path>)
# and extracts it into a game folder next to the exe.

param(
    [string]$Iso,
    [string]$Preset = 'msvc-debug'
)

. "$PSScriptRoot\common.ps1"

$game = Find-BuiltTool 'TheGetawayRecomp' $Preset
$gameArgs = @()
if ($Iso) { $gameArgs += @('--iso', (Resolve-Path $Iso).Path) }
Invoke-Native $game $gameArgs
