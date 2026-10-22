# SPDX-License-Identifier: MIT
# Shared helpers, dot-sourced by the other scripts.

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Invoke-Native {
    param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments = @())
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}

# Load the MSVC x64 environment (cl, link, and the CMake/Ninja that ship with
# Visual Studio) into this PowerShell session, unless it is already loaded.
function Enter-VsDevEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'Visual Studio was not found. Install Visual Studio 2022 or newer with "Desktop development with C++".'
    }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) {
        throw 'Visual Studio has no C++ x64 toolset. Add the "Desktop development with C++" workload.'
    }

    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

function Find-BuiltTool {
    param([Parameter(Mandatory)][string]$Name, [string]$Preset = 'msvc-debug')
    $buildDir = Join-Path $RepoRoot "builds\$Preset"
    $tool = Get-ChildItem -Path $buildDir -Recurse -Filter "$Name.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $tool) { throw "$Name.exe not found under builds\$Preset. Run tools\scripts\build.ps1 -Preset $Preset first." }
    return $tool.FullName
}

function Find-GameIso {
    param([string]$Iso)
    if ($Iso) { return (Resolve-Path $Iso).Path }
    $found = Get-ChildItem -Path (Join-Path $RepoRoot 'iso') -Filter '*.iso' -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) { throw 'No .iso found in the iso folder. Put your own dump of The Getaway (SCUS-97133) there, or pass -Iso <path>.' }
    return $found.FullName
}
