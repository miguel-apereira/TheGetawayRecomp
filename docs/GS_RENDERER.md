# Native GS renderer: design

Status: **design agreed** (2026-10-23). Nothing here is built yet. Miguel's
answers to the open questions are recorded in [Decisions](#13-decisions).
The work starts in its own thread once the New Game hang is fixed.

## 1. Summary

The GS (Graphics Synthesizer) is replaced by a **render-target renderer on raw
Vulkan**: the game's draws become real GPU draws into GPU render targets, and
its textures are decoded once into a GPU texture cache. This is the approach
behind PCSX2's hardware renderer and behind the God of War native port, which
renders that game at 1080p and 16:9 with it. Unlike an emulator, we only need to
cover what *The Getaway* (SCUS-97133) actually does, and we can measure that
exactly.

The main choices:

1. **Own the whole GS path.** Our code in `src/hw/gs` parses the GIF stream,
   keeps the GS registers and assembles primitives. A small hook in our
   PS2Recomp fork hands it the GIF packets (like the IPU hook), so PS2Recomp's
   per-primitive frontend is bypassed. PS2Recomp's software GS stays available
   as a reference for tests.
2. **A GS thread.** The game thread copies each GIF/VIF1 transfer into a ring
   buffer and carries on. A GS thread turns it into Vulkan work. The game
   waits only where it can observe the GS (readbacks, CSR).
3. **Batching first.** One vertex buffer per frame, per-draw state in a
   storage buffer, bindless textures, few pipelines. Thousands of GS
   primitives become tens of Vulkan draws.
4. **Measure before building.** Step one is a GS inventory and a GS
   dump/replay tool on today's software GS, so every feature and heuristic is
   justified by what the game does.
5. **Native resolution first, then quality.** Correct at 1x, then upscaling
   (2x-4x or window size), a half-pixel fix for post-processing, an
   anti-blur/deinterlace choice, later widescreen and 60 FPS through game-side
   patches.

## 2. What we have today

The game boots to the main menu on PS2Recomp's temporary software GS. In
Release it runs at about one fifth of full speed, and about 60% of the time is
spent in that renderer. The reasons are structural:

| Where | What happens | Cost |
|---|---|---|
| `GS::processGIFPacket` (`gs_frontend.cpp`) | Parses every GIF tag and register write on the game thread, under a recursive mutex | runs inline with the game |
| `GS::vertexKick` → `buildDrawBatch` | Builds a `GSPrimitiveBatch` per primitive, copying the full `GSDrawState` (both contexts, ~300 bytes) | per primitive |
| `GSRasterBackend::Submit` | One virtual call per primitive | per primitive |
| `GSCpuBackend::DrawTriangle` | Float barycentric test for every pixel of the bounding box, then `WritePixel` / `SampleTexture` through per-format function pointers into swizzled VRAM | per pixel |
| `TexturePageCache` | Caches one 8 KiB page and copies a whole page on every miss | per texel miss |
| Presentation | Reads the display buffer back to RGBA8, which `main.cpp` uploads to a Vulkan image | per frame |

Useful for us: the swizzle tables (`ps2_gs_memory.h`, `ps2_gs_psm*.h`), the
GIF arbiter with PATH1/2/3 ordering and PATH3 masking (`ps2_gif_arbiter`,
`ps2_memory.cpp`), and the CPU backend as a pixel-level reference in tests.

Data flow today:

```
game code ──sceDmaSend──► PS2Memory DMA ──► VIF1 interpreter ──MSCAL──► VU1 interpreter ──XGKICK──┐
          └──────────────► GIF channel (PATH3) ─────────────────────────────────────────────────────┤
                           VIF1 DIRECT/DIRECTHL (PATH2) ────────────────────────────────────────────┤
                                                                                         GifArbiter ▼
                                                    GS frontend (game thread) ──► GSCpuBackend (VRAM)
                                                                                         │ readback
                                                              main.cpp ──► VulkanRenderer::drawFrame
```

## 3. How The Getaway drives the GS

What we know:

- It uses the Sony SDK: `sceGsResetGraph`, `sceGsSetDefDispEnv`,
  `sceGsPutDispEnv`, `sceGsPutDrawEnv`, `sceGsExecStoreImage` (a GS→EE
  readback, present in the binary, use unknown), `sceDmaSend/SendN/Sync`,
  `sceDevVu1*` and the `sceVu0*` math helpers.
- All three GIF paths are live: PATH3 (GIF DMA) for texture and movie uploads,
  PATH2 (VIF1 DIRECT) and PATH1 (VU1 `XGKICK`). 3D geometry will mostly come
  through VU1 microcode, which is static in the ELF (`.vutext` and
  `.DVP.overlay` sections), so it can be compiled ahead of time later.
- The movies are decoded by our native IPU and uploaded back through the GIF
  as IMAGE transfers, so they are just texture uploads to the renderer.
- PCSX2's GameIndex lists three hardware-renderer fixes for SCUS-97133 and no
  EE or VU fixes:
  - `textureInsideRT`: the game samples textures from *inside* a render
    target (a sub-rectangle of a larger buffer). Typical of post-processing
    and of off-screen buffers packed together. Our texture lookup must find
    them.
  - `texturePreloading`: changes how much of a texture PCSX2 uploads and
    hashes up front. It suggests large or partial texture uploads that the
    texture cache must handle cheaply.
  - `halfPixelOffset: 2`: upscaling misaligns some passes (usually blur,
    bloom or screen copies drawn with sprites), so it needs a sub-pixel
    correction when the resolution is raised. Native 1x is not affected.

What we don't know yet, and will measure (milestone R0): primitive and
texture formats, blend equations, alpha/destination-alpha tests, fog, mipmaps,
how often render targets are read as textures (and in which format),
whether `sceGsExecStoreImage` or other readbacks happen, draws and uploads
per frame, and the display mode (frame/field, interlace, one or two circuits).
The menus can be measured now; gameplay once New Game loads.

## 4. Lessons from other ports

| Project | GS approach | Result | Lesson for us |
|---|---|---|---|
| [Waffle565/God-Of-War-Native-Port](https://github.com/Waffle565/God-Of-War-Native-Port) | Own OpenGL **render-target renderer** (`GSHwBackend`, ~2.3k lines) at 2x+; texture/CLUT store instead of a VRAM model; reinterpretation passes for the game's few framebuffer tricks | 1080p, 16:9, 60 FPS target; the CPU rasteriser was deleted | Measuring the game first (a "GS inventory") made a small, fast renderer possible. ~5k draws per frame, 98% plain uploaded textures, rare RT-as-texture |
| same | GS worker thread (VIF1 → VU1 → GS off the game thread) | 14.5 → 20 game FPS | Thread the GS early; wake the game when FINISH is set or it sleeps on it |
| same | VU1 microprograms compiled ahead of time | VU1 was the biggest cost (14-36 ms/frame) | Once in game, VU1 is likely to cost more than the GS. Separate work item |
| [KIexster/god-of-war-recomp](https://github.com/KIexster/god-of-war-recomp) and [LightVelox/sotc-vibe-pc](https://github.com/LightVelox/sotc-vibe-pc) | OpenGL 4.6 backend keeping the 4 MiB **VRAM on the GPU**, drawing with fragment-shader interlock or compute | About +36% over the CPU GS in God of War; textured blended draws cost 50-56 ms/frame | Exact VRAM emulation on the GPU is safe but slow, and it can't simply upscale. Not our default |
| [paraLLEl-GS](https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation) (Vulkan compute, used by LRPS2) | Accurate compute rasteriser over a VRAM buffer, tile binning, super-sampling | Very accurate; light on a modern GPU even at 16x SSAA; CPU-bound on geometry in heavy scenes; render targets round-trip through VRAM | Good ideas to borrow: bindless uber-shader, per-256-byte-block hazard tracking, palettes as a ring of CLUT snapshots, batching barriers. Higher resolution comes mainly as SSAA (with an optional 2x scan-out), not as larger render targets |

PCSX2's hardware renderer is the best-known public reference for the
render-target approach (texture cache, target cache, half-pixel offsets,
blending tiers). We follow the publicly documented ideas, not its code (see
[Licensing](#11-licensing-and-references)).

## 5. Approaches considered

| | A. Render-target renderer (**recommended**) | B. VRAM on the GPU (compute or interlock) | C. Faster CPU rasteriser |
|---|---|---|---|
| Speed | Fastest: fixed-function raster, few draws | Medium; barriers and per-pixel VRAM addressing | Slowest; CPU-bound at 1x |
| Higher resolution | Native (scale the targets) | Only by super-sampling | No |
| Widescreen / 60 FPS | Yes (game patches + wider targets) | Hard | No |
| Accuracy risk | Framebuffer tricks need handling (format reinterpretation, RT inside RT, readbacks) | Lowest | Low |
| Work | Medium-high, but bounded to one game | High | Medium |

A is the only one that meets "good performance and good visuals". Its risk is
bounded because we target one game and can measure every case it uses.

## 6. Architecture

```
 game thread                         GS thread                                   GPU
 ───────────                         ─────────                                   ───
 sceDmaSend / VIF1 / VU1 XGKICK      GIF parser (fast paths per tag shape)
   │ copy bytes                        │
   ▼                                   ▼
 GIF ring (SPSC, ~32 MiB) ───────►   GS state + vertex queue (both contexts)
 privileged regs (PMODE, DISPFB…)      │
 CSR / IMR (FINISH, SIGNAL, LABEL) ◄─  Draw batcher ──► frame vertex ring ─────► vertex shader
                                       │                draw-state SSBO          (scale, offset,
                                       │                                          half-pixel fix)
                                       ├─ Local memory shadow (4 MiB, swizzled)   fragment uber-shader
                                       │    dirty bitmap per 256 B block           (TFX, fog, alpha test,
                                       ├─ Texture cache ───── decode compute ────► bindless textures
                                       │    (index + palette for T4/T8)             blend: HW / dual-src /
                                       ├─ Target cache (colour + depth, scaled)     shader+feedback)
                                       ├─ Transfers (upload, local→local, readback)
                                       └─ Presenter at vsync: PCRTC merge ──────► swapchain (SDL3 window)
```

### 6.1 Intake and threading

- **Fork hook.** Add `GsHost` to our PS2Recomp fork (branch `getaway`), as for
  the IPU: when installed, the GIF arbiter's output, GS privileged register
  access (`0x12000000`), `sceGsExecLoadImage/StoreImage` and the vsync
  callback go to our implementation instead of PS2Recomp's `GS`.
  A command-line switch keeps the software GS for A/B runs.
- **Ring buffer.** At each transfer the game thread copies the GIF bytes into
  a single-producer single-consumer ring and returns. Packets keep their
  arbitrated PATH1/2/3 order.
- **Synchronisation points** (the only places the game waits):
  - readbacks (local→host transfers, `sceGsExecStoreImage`): drain, render,
    copy back;
  - CSR reads for FINISH/SIGNAL/LABEL: the GS thread sets the bits when it
    *parses* the register, not when the GPU finishes, and wakes the game's
    scheduler (the God of War port lost most of its threading gain until it
    did this). An option to raise FINISH at enqueue time is measured later;
  - reset and shutdown.
- **Later**: move VIF1 and VU1 onto the same worker, as the God of War port
  did, so the cut is at the DMA kick rather than at the GIF. The ring format
  allows it (a job is "channel + bytes").

### 6.2 GS frontend (our code)

- Register file for both contexts, PRIM/PRMODE, vertex queue and kick rules
  (XYZ2 vs XYZ3, strips, fans, sprites, lines, points), XYOFFSET, SCISSOR.
- **Fast GIF parsing.** Most packets repeat one register list (for example
  `ST RGBAQ XYZ2`, or `UV RGBAQ XYZF2`). The parser recognises the tag's
  register list once and runs a specialised loop for it, writing vertices
  straight into the frame's vertex ring. No per-register virtual calls, no
  state copies per primitive.
- Primitives are emitted as triangle lists. Flat shading (IIP=0) writes the
  last vertex's colour to all three vertices. Sprites are stored once and
  expanded in the vertex shader from `gl_VertexIndex` (6 vertices).

### 6.3 Draw batching

- A **draw key** holds only what changes Vulkan objects: target (colour +
  depth), pipeline variant, and barrier needs. Everything else (texture index,
  CLUT slot, TEXA, fog colour, alpha reference, FBA, scissor rectangle,
  texture clamp) goes into a per-draw entry in a storage buffer, indexed per
  vertex. Consecutive primitives with the same key merge into one
  `vkCmdDraw`.
- **Bindless textures** (descriptor indexing, Vulkan 1.2 core): changing
  texture doesn't split a batch.
- **Pipelines** come from one uber-shader with specialisation constants for
  the rarely changing switches (texture on/off, palette, fog, alpha test mode,
  blend tier). Depth test, depth write, colour write mask and stencil use
  dynamic state. Pipelines are compiled on a worker thread, stored in a
  `VkPipelineCache` on disk, and the keys seen are saved and pre-built at the
  next start, so stutter happens at most once.
- A batch is split only for a target change, a pipeline change, or a hazard
  (the draw reads what an earlier draw in the batch wrote).

### 6.4 Local memory, uploads and transfers

- The CPU keeps a **shadow of the 4 MiB local memory** in GS layout (swizzled).
  Host→local uploads are written there with the swizzle tables, and a dirty
  bit is set per 256-byte block (16,384 blocks, 2 KiB bitmap). This keeps the
  game's "upload as 32-bit, read as 8-bit" packing correct for free.
- Dirty blocks are copied to a 4 MiB GPU buffer only when a texture or CLUT
  that covers them is needed.
- An upload that lands on a live render target is also drawn into that
  target, so later draws see it.
- Local→local copies go through the same paths (shadow copy, or a GPU copy
  if a target is involved).
- Local→host readbacks find the target, copy it down at 1x, and wait. If the
  inventory shows none, this stays a simple slow path.

### 6.5 Texture cache

- Key: TBP0, TBW, PSM, TW/TH, mip levels, and the clamp mode's region. An
  entry is decoded by a **compute shader** from the GPU copy of local memory
  into a `VkImage`.
- **Paletted formats (T8, T4, T8H, T4HL/HH)** decode to an `R8_UINT` index
  image. Palettes live in a CLUT ring (one 256-entry snapshot per CLUT load,
  following the CLD/CSM/CSA rules), and the shader looks the index up. A
  palette change then costs a few hundred bytes instead of re-decoding the
  texture. Bilinear filtering of paletted textures is done in the shader on
  the four looked-up colours, as the GS does.
- Invalidation per 256-byte block from the dirty bitmap. No hashing of texture
  contents unless the inventory shows it pays off.
- **Textures inside targets** (`textureInsideRT`): the lookup first checks the
  target cache. If TBP falls inside a live target, the draw samples that
  target (at its scaled resolution) with the right offset. If the format
  differs (for example a CT32 target read as T8, or a Z buffer read as colour),
  a **reinterpretation** compute pass rebuilds the requested format from the
  target's GS layout. Each such case seen in the inventory gets a test.
- GS mipmapping (MXL, MIPTBP1/2, LCM, L, K) uses the GS LOD formula in the
  shader with `textureLod`, not the GPU's derivative-based LOD.

### 6.6 Render targets and depth

- Key: FBP, FBW, PSM (and ZBP/PSM for depth). Created at the **scaled
  resolution** (scale factor per axis, so non-integer and anamorphic widths
  work later).
- Colour as `R8G8B8A8_UNORM` with alpha stored raw (0x80 = 1.0, so values up to
  2.0 survive). CT16/CT16S targets are drawn in RGBA8 and quantised to 5:5:5:1
  in the shader when written, so later reads as 16-bit data match. FBMSK maps to
  the colour write mask when it is per-channel and to a shader mask otherwise.
- Depth as `D32_SFLOAT`, with the GS's integer Z mapped to [0,1]. Z24 and Z16
  values are exact; full 32-bit Z loses the bottom bits (rare; measured).
- DATE (destination alpha test) uses the stencil buffer, filled from the
  target's alpha bit before the draw.
- Targets track which local memory pages they cover, so uploads, texture
  lookups and readbacks can find them. Overlapping targets of different
  widths or formats (common with packed off-screen buffers) are resolved by
  the rules the inventory justifies, starting with "newest write wins".

### 6.7 Shading and blending

- **Vertex shader:** GS 12.4 fixed-point X/Y minus XYOFFSET, times the scale
  factor, plus the half-pixel correction when upscaling. Z to depth. S, T, Q
  passed with `noperspective`, because the GS interpolates them linearly on
  screen and divides per pixel; UV (fixed point) the same way. Fog factor and
  colour as `noperspective` too.
- **Fragment shader:** TFX (modulate, decal, highlight, highlight2) and TCC,
  TEXA expansion for 16/24-bit textures, fog, alpha test (with the four AFAIL
  modes: KEEP, FB_ONLY, ZB_ONLY, RGB_ONLY), FBA, COLCLAMP (clamp or wrap),
  dithering (DTHE) only for 16-bit targets.
- **Blending** `((A - B) * C >> 7) + D`, A/B/D ∈ {Cs, Cd, 0}, C ∈ {As, Ad, FIX}.
  Three tiers chosen per draw:
  1. **Fixed-function** when the equation maps directly (the common
     `Cs*As + Cd*(1-As)` style cases and additive ones).
  2. **Dual-source blending** when C needs a value the fixed function can't
     produce (alpha above 1.0, FIX): the shader outputs the factor as the
     second source.
  3. **Shader blending** for the rest (Ad as a factor above 1.0, wrap mode,
     PABE): read the destination through a feedback loop
     (`VK_EXT_attachment_feedback_loop_layout`, or
     `VK_EXT_rasterization_order_attachment_access` where present) with as
     few barriers as possible, and only for the draws that need it.
  The inventory tells us which tiers The Getaway needs. If tier 3 is rare, it
  can stay simple.

### 6.8 Presentation

- At vsync, the presenter reads PMODE, SMODE2, DISPFB1/2, DISPLAY1/2 and
  BGCOLOR, finds the target(s) shown, and merges the two read circuits
  (ALP, MMOD, SLBG) in a fragment pass at scaled resolution. Then it scales
  to the swapchain with the right aspect ratio, letterboxed.
- **Anti-blur:** many games show the same frame on both circuits one line
  apart to soften interlace flicker. Detect it and show one circuit at full
  quality. On by default; a player setting, exposed later in the launcher or
  an options menu.
- **Interlace/field mode:** if the game renders fields, use a simple
  bob/weave choice first; better deinterlacing later.
- Presentation mode FIFO (vsync) by default. The game keeps its own timing
  (59.94 Hz vsync from the scheduler); the host never drops or repeats game
  frames on its own.
- The current readback path (`captureFrame`, `--dump-frames`) stays for
  headless tests: one GPU copy back at 1x when requested.

### 6.9 Vulkan specifics

- **Baseline:** Vulkan 1.3 (dynamic rendering, synchronization2, timeline
  semaphores, descriptor indexing core), plus `dualSrcBlend`. Optional:
  `VK_EXT_attachment_feedback_loop_layout`,
  `VK_EXT_rasterization_order_attachment_access`,
  `VK_EXT_extended_dynamic_state3`. (Decided: Vulkan 1.3.)
- **Memory:** a few large allocations: per-frame host-visible rings
  (vertices, draw entries, uploads; device-local host-visible when Resizable
  BAR exists), a pool for target images and one for texture-cache images,
  sub-allocated by our own small allocator. No allocation per draw.
- **Frames in flight:** 2, with a timeline semaphore per submission so the
  CPU knows when ring space is free.
- **Shaders:** GLSL compiled to SPIR-V at build time by glslang, built from
  a submodule as part of our build (no Vulkan SDK needed), and embedded in
  the exe.
- **Debugging:** debug labels per GS draw batch, GPU timestamps per frame
  section, validation with `--validation` as today.
- Platform code stays in `src/host` (window, swapchain, surface). The GS
  renderer itself only needs a `VkDevice` and queues, so it also runs headless
  for tests.

## 7. Performance plan

Reference machine: Miguel's main PC, Ryzen 5 5600X and Radeon RX 6700 XT.
Targets on it:

| Budget per game frame | Target |
|---|---|
| Game thread time spent on the GS (copy into the ring) | < 0.5 ms |
| GS thread (parse, batch, record) | < 4 ms, so 60 FPS stays possible |
| GPU at 1440p (and headroom for 4K) | < 4 ms |
| Vulkan draws per frame | tens to a few hundred, not thousands |
| Pipeline compiles during play | 0 after the first run |

Techniques, in order of expected gain: the GS thread; specialised GIF loops
writing straight into mapped memory; batching with bindless textures and a
draw-state buffer; CLUT ring instead of re-decoding paletted textures; GPU
texture decoding; parsing-time FINISH. Every step is measured with
`tgr-profile` and GPU timestamps on the same replayed GS dump, so changes are
compared on identical input.

Outside the renderer but decisive: once in game, VU1 microcode will likely
cost more than the GS (it was the biggest cost in God of War). Compiling The
Getaway's VU1 programs ahead of time is a separate work item that should
follow soon after the renderer starts.

## 8. Visual quality

In order, after the 1x renderer is correct:

1. **Internal resolution:** 1x, 2x, 3x, 4x or "match window", per axis.
   Default: match window. It's a player setting from the start, exposed
   later in the launcher or an options menu.
2. **Upscaling fixes:** the half-pixel correction (`halfPixelOffset: 2`),
   rounding of sprite coordinates for 1:1 screen copies, and keeping post-
   processing chains aligned. Passes that only work at 1x can be kept at 1x.
3. **Texture filtering:** honour the game's filters; optional anisotropic
   filtering where the game uses fixed LOD.
4. **Anti-blur and deinterlacing** as in 6.8.
5. **Output scaling:** sharp bilinear by default; FSR1 or similar optional.
6. **Enhancements (later, need game patches):** widescreen (a game patch for
   the projection and culling, wider targets in the renderer, and HUD
   fixes), and 60 FPS (game timing patches; the renderer only needs the
   throughput). The planned modern camera is game-side too.

## 9. Code layout

```
src/hw/gs/          GS frontend: GifParser, GsState, VertexAssembler, LocalMemory (shadow),
                    TransferEngine, GsThread, Inventory (statistics), GsDump (record/replay)
src/hw/gs/vk/       Renderer: DrawBatcher, TextureCache, TargetCache, Pipelines, Presenter
src/hw/gs/shaders/  GLSL: gs.vert, gs.frag, decode_*.comp, reinterpret_*.comp, present.frag
src/host/           VulkanRenderer becomes device + swapchain owner; hands the GS a device and a queue
tools/gsreplay/     tgr-gsreplay: replays a GS dump headless, writes images and timings,
                    can compare against PS2Recomp's CPU backend
external/PS2Recomp  fork: GsHost hook (small), nothing else
```

All new files carry `SPDX-License-Identifier: MIT` headers.

## 10. Testing and validation

- **GS dumps.** Record the GIF stream, privileged register writes and a local
  memory snapshot per frame (headless, works on the VM). Files hold game data,
  so they stay in a gitignored folder and are never published.
- **Replay tool.** `tgr-gsreplay` renders a dump with our renderer, and with
  PS2Recomp's CPU backend as a reference at 1x. Image differences above a
  threshold fail. Small synthetic GIF tests (written by hand, no game data)
  cover formats, swizzles, blending and texture modes in the repo's tests.
- **Reference images.** Screenshots from PCSX2's software renderer on
  Miguel's main PC at the same moments, for scenes where the CPU backend is
  wrong.
- **Performance.** The same dumps timed in Release with GPU timestamps.
- Running tests needs a Vulkan device. Images and timings come from Miguel's
  main PC. A software Vulkan driver on the VM for headless replay tests is
  tried only if it's easy.

## 11. Licensing and references

- No PCSX2 code is linked or copied. Our files are MIT; PCSX2 is GPL, so
  copying from it would also conflict with the MIT headers.
- References used for behaviour: public GS documentation (register layouts,
  formats, swizzling), PS2Recomp's sources (GPL-3.0, already linked: we can
  call its swizzle tables but not copy them into MIT files), PCSX2 as a
  running program to compare against, and the design write-ups listed in
  section 4.
- PCSX2's and the GPL ports' renderer source may be read to understand
  behaviour (Miguel, 2026-10-23). Our code is written by us; nothing is
  copied.

## 12. Milestones

| | Milestone | Done when |
|---|---|---|
| R0 | **Measure.** GS inventory counters and GS dump/replay on today's software GS | Inventory report for menus and the first in-game scene |
| R1 | **Own frontend.** Fork hook, our GIF parser and GS state feeding PS2Recomp's CPU backend, then the GS thread | Same images as today, game thread no longer runs the GS |
| R2 | **Vulkan at 1x for 2D.** Targets, uploads, texture decode, sprites, simple blending, presenter | Legal notice, logo, movie and menu match the CPU backend |
| R3 | **Vulkan at 1x for 3D.** Depth, alpha test, fog, palettes, DATE, FBMSK, mipmaps, RT-as-texture, reinterpretation, readbacks | In-game frames match the reference; software GS no longer the default |
| R4 | **Performance.** Batching, bindless, pipeline cache, sync tuning | GS within the budget in section 7 |
| R5 | **Quality.** Upscaling and its fixes, anti-blur, output scaling | Clean 2x-4x image in the measured scenes |
| later | Widescreen, 60 FPS (game patches) | After the port is playable |

All of it, R0 included, starts after the New Game hang is fixed, in a new
thread dedicated to the renderer, so the VM serves one thread at a time and
the work is tested on real 3D scenes. Ahead-of-time compiled VU1 follows R2.

## 13. Decisions

Miguel answered the open questions on 2026-10-23.

| # | Question | Decision |
|---|---|---|
| 1 | Approach | Render-target renderer (A) only. Reconsider only if the inventory shows many framebuffer tricks |
| 2 | Where to hook | Our own GS frontend through a small `GsHost` hook in the fork |
| 3 | Minimum Vulkan | 1.3 (the README's requirement changes when the renderer lands) |
| 4 | Shader compiler | glslang built from a submodule |
| 5 | GS thread | Added in R1, once our frontend matches the CPU backend single-threaded |
| 6 | Default internal resolution | Match window. Must be a player setting, later in the launcher or an options menu |
| 7 | Anti-blur | On by default. Must be a player setting too |
| 8 | Reference hardware | Ryzen 5 5600X, Radeon RX 6700 XT |
| 9 | Where GPU tests run | Miguel's main PC for images and timings; software Vulkan on the VM only if easy |
| 10 | Reading references | Read PCSX2 and the GPL ports' source for understanding; never copy |
| 11 | VU1 | Ahead-of-time compiled VU1 is the next item after R2 |
| 12 | When to start | After the New Game hang fix, in a new thread dedicated to the renderer |
