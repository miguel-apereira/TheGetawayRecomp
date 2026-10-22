# SPDX-License-Identifier: MIT
#
# One-time setup: creates the local git repository (if there is none yet) and
# fetches the third-party code in external/ at the pinned versions.
# Safe to run again; it only adds what is missing.

. "$PSScriptRoot\common.ps1"
Set-Location $RepoRoot

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'Git is not installed or not on PATH. Install it from https://git-scm.com and run this again.'
}

if (-not (Test-Path (Join-Path $RepoRoot '.git'))) {
    Invoke-Native git @('init', '-b', 'main')
}

# Pinned versions. Update a pin deliberately, test, and commit the new gitlink.
$submodules = @(
    @{ Path = 'external/PS2Recomp';      Url = 'https://github.com/miguel-apereira/PS2Recomp.git';  Ref = 'eb8688b2fd191129f103a39543002c005b33409c' },
    @{ Path = 'external/SDL';            Url = 'https://github.com/libsdl-org/SDL.git';              Ref = 'release-3.4.8' },
    @{ Path = 'external/Vulkan-Headers'; Url = 'https://github.com/KhronosGroup/Vulkan-Headers.git'; Ref = 'vulkan-sdk-1.4.363.0' },
    @{ Path = 'external/volk';           Url = 'https://github.com/zeux/volk.git';                   Ref = 'vulkan-sdk-1.4.363.0' }
)

$registered = @()
if (Test-Path (Join-Path $RepoRoot '.gitmodules')) {
    $registered = git config --file .gitmodules --get-regexp 'submodule\..*\.path' | ForEach-Object { ($_ -split ' ', 2)[1] }
}

foreach ($sub in $submodules) {
    if ($registered -contains $sub.Path) { continue }
    Write-Host "Adding $($sub.Path) at $($sub.Ref)"
    Invoke-Native git @('submodule', 'add', $sub.Url, $sub.Path)
    Invoke-Native git @('-C', $sub.Path, 'checkout', '--quiet', $sub.Ref)
    Invoke-Native git @('add', $sub.Path)
}

Invoke-Native git @('submodule', 'update', '--init', '--recursive')

Write-Host ''
Write-Host 'Done. Next: tools\scripts\build.ps1'
