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
Invoke-Native $recomp @($Config)
