# SPDX-License-Identifier: MIT
#
# Runs PS2Recomp's ps2_recomp on the extracted boot ELF using
# recomp\config\SCUS-97133.toml, then tgr-vurecomp on the VU microprograms
# in its .vutext section. Output goes to recomp\generated
# and recomp\generated\vu (git-ignored).
#
#   tools\scripts\recompile.ps1           # everything
#   tools\scripts\recompile.ps1 -VuOnly   # only the VU microprograms

param(
    [string]$Config = 'recomp\config\SCUS-97133.toml',
    [string]$Preset = 'msvc-debug',
    [switch]$VuOnly,
    # Delete the generated C++ first. Not needed normally: see below.
    [switch]$Clean
)

. "$PSScriptRoot\common.ps1"
Set-Location $RepoRoot

if (-not (Test-Path 'iso\extracted\SCUS_971.33')) {
    throw 'iso\extracted\SCUS_971.33 is missing. Run tools\scripts\extract-disc.ps1 first.'
}

if (-not $VuOnly) {
    $recomp = Find-BuiltTool 'ps2_recomp' $Preset
    New-Item -ItemType Directory -Force -Path 'recomp\generated' | Out-Null
    # With incremental_output (set in the config), ps2_recomp leaves files whose
    # content did not change untouched, so the next build only recompiles the
    # functions that did, and it deletes files of functions that no longer exist.
    if ($Clean) {
        Get-ChildItem 'recomp\generated\*' -File -Include '*.cpp', '*.h' | Remove-Item
    }
    Invoke-Native $recomp @($Config)
}

$vurecomp = Find-BuiltTool 'tgr-vurecomp' $Preset
New-Item -ItemType Directory -Force -Path 'recomp\generated\vu' | Out-Null
Get-ChildItem 'recomp\generated\vu\*' -File -Include '*.cpp' | Remove-Item
Invoke-Native $vurecomp @('iso\extracted\SCUS_971.33', 'recomp\generated\vu')
