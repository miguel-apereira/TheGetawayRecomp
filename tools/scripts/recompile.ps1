# SPDX-License-Identifier: MIT
#
# Runs PS2Recomp's ps2_recomp on the extracted boot ELF using
# recomp\config\SCUS-97133.toml. Output goes to recomp\generated (git-ignored).

param(
    [string]$Config = 'recomp\config\SCUS-97133.toml',
    [string]$Preset = 'msvc-debug'
)

. "$PSScriptRoot\common.ps1"
Set-Location $RepoRoot

if (-not (Test-Path 'iso\extracted\SCUS_971.33')) {
    throw 'iso\extracted\SCUS_971.33 is missing. Run tools\scripts\extract-disc.ps1 first.'
}

$recomp = Find-BuiltTool 'ps2_recomp' $Preset
New-Item -ItemType Directory -Force -Path 'recomp\generated' | Out-Null
# ps2_recomp only writes files, so clear the last run's output: a function that
# was renamed or merged in the new function map would otherwise linger.
Get-ChildItem 'recomp\generated' -File -Include '*.cpp', '*.h' -Recurse | Remove-Item
Invoke-Native $recomp @($Config)
