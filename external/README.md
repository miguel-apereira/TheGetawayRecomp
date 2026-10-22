# external/

Third-party code, as git submodules pinned to exact versions.
`tools/scripts/bootstrap.ps1` adds and fetches them.

| Submodule | Pinned at | License | Used for |
|---|---|---|---|
| [PS2Recomp](https://github.com/miguel-apereira/PS2Recomp) (our fork of [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp)) | `08df8f1` on `getaway` (upstream `eb8688b`, 14 Oct 2026) | GPL-3.0 | `ps2_recomp` tool; runtime base for the recompiled code |
| [SDL](https://github.com/libsdl-org/SDL) | `release-3.4.8` | zlib | Window, input, audio |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.4.363.0` | Apache-2.0 / MIT | Vulkan API headers |
| [volk](https://github.com/zeux/volk) | `vulkan-sdk-1.4.363.0` | MIT | Vulkan function loader (no SDK needed to build) |

PS2Recomp is **our own fork**. Changes The Getaway needs are committed on a
`getaway` branch of the fork (one topic per commit, upstreamable where
possible), and the submodule is pinned to a commit on that branch. To pick up
upstream fixes, merge `ran-j/PS2Recomp` into the fork, test, and bump the pin.
