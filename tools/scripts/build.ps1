# SPDX-License-Identifier: MIT
#
# Configure and build with a preset from CMakePresets.json.
#   tools\scripts\build.ps1                       # msvc-debug
#   tools\scripts\build.ps1 -Preset msvc-release
#   tools\scripts\build.ps1 -WithRecompiledCode   # after recompile.ps1

param(
    [ValidateSet('msvc-debug', 'msvc-release', 'clangcl-release')]
    [string]$Preset = 'msvc-debug',
    [switch]$WithRecompiledCode
)

. "$PSScriptRoot\common.ps1"
Set-Location $RepoRoot
Enter-VsDevEnvironment

$configureArgs = @('--preset', $Preset)
$configureArgs += "-DTGR_WITH_RECOMPILED_CODE=$(if ($WithRecompiledCode) { 'ON' } else { 'OFF' })"

Invoke-Native cmake $configureArgs
Invoke-Native cmake @('--build', '--preset', $Preset)

Write-Host ''
Write-Host "Built into builds\$Preset\bin"
