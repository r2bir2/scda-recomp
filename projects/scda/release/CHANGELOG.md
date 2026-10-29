# SCDA Recomp - changes per release

Windows builds. Every release is a complete package: unpack it into a new
folder, copy `gamefiles\` and `user\` over from the previous version to keep
the game and your saves. Option names refer to `scda_options.txt`.

## v1.63 (2026-10-29)

- Vulkan: screen clears are recorded inside the running render pass instead
  of ending it, which saves several render passes per frame (developer
  switch `RECOMP_VK_CLEAR_IN_PASS=0` restores the old way).
- Vulkan display without GL interop (the fallback path from v1.6): the copy
  of the picture now prefers cached host memory, so videos no longer stutter
  on that path.
- Graphics cards with little memory and integrated graphics: the inline
  vertex ring scales with the video memory (VRAM/16, at least 128 MiB), and
  integrated GPUs size their conversion buffers from a quarter of the shared
  memory.
- Older or limited drivers: OpenGL falls back from 4.0 to 3.3 ... 3.0
  contexts, and Vulkan features that are missing (geometry shaders,
  wireframe fill, memory export) are worked around instead of stopping
  the game. Nothing changes on hardware that has them.
- Experimental, off by default and not meant for playing yet: the
  groundwork for a high-level D3D renderer (`RECOMP_HLE_D3D=1`), using code
  from SCDA-XBERecomp by TGP482 and Joshhhuaaa (MIT, licence in
  `licenses\`). With it switched on, the game currently draws nothing.
- Source only: an experimental native ARM64 Linux port (tested on a Khadas
  Edge 2); it is not part of the Windows package.

## v1.62 (2026-10-19)

- Laptops with two graphics chips: OpenGL now always runs on the dedicated
  GPU, independent of the folder the game is in. If the picture still cannot
  be shared between OpenGL and Vulkan, the game switches to the CPU copy path
  instead of stopping.

## v1.61 (2026-10-19)

- Vulkan on 2 GB graphics cards: the depth/stencil conversion buffers are
  now sized from the video memory (1/8, 128 to 800 MiB) instead of a fixed
  2 x 800 MiB, which filled such cards completely.

## v1.6 (2026-10-19)

- Vulkan: if the picture cannot be handed to OpenGL, it is copied through
  the CPU instead (slower, but works).

## v1.5 (2026-10-19)

- Vulkan: the display picture uses a dedicated memory allocation where the
  driver requires one.

## v1.4 (2026-10-19)

- Vulkan picks the dedicated graphics card instead of the first one found
  (laptops with Intel + NVIDIA). New option `GraphicsCard` to pick a card by
  name.

## v1.3 (2026-10-18)

- No more blurry, blocky picture with Windows display scaling (e.g. 150 %).
- New option `TextureFiltering`: 0 = like the console, 1 = blended mipmaps,
  2 = 1 plus 16x anisotropic filtering (default). The console's texture LOD
  bias is now applied, so walls no longer turn blurry too early.

## v1.2 (2026-10-18)

- RenderScale above 1: small effect render targets (glow edges, e.g. around
  the ice block on Iceland) are averaged from the high resolution picture,
  so they no longer jitter.
- Edge smoothing (FXAA) only with RenderScale above 1; at 1 the HUD and text
  stay sharp.
- Shrinking a large picture to the window is evenly averaged (thin HUD lines
  no longer flicker at RenderScale 3 on Full HD).

## v1.1 (2026-10-18)

- `AudioDevice` applies to every sound mode; new keywords `hdmi` and
  `spdif`.
- New option `NightVisionInterference`: switches off the night-vision
  interference (picture and sound) in both JBA headquarters levels.

## v1.0 (2026-10-13)

- First release.
