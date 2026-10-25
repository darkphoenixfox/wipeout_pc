---
name: ps4-port-display-and-stereo3d
description: Techniques for native PS4 game ports that run the original x86-64 code with a shadPS4-derived Vulkan video core (bbport-style recomps), covering new render resolutions and aspect ratios via binary patches (Hor+/Vert+ camera fixes, culling), keeping the HUD at its designed shape on other screen shapes, depth-based stereo 3D (side by side), and LeiaSR / Simulated Reality display output (Samsung Odyssey 3D, Acer SpatialLabs) through an MSVC bridge DLL and Vulkan/D3D11 texture sharing. Use when adding widescreen, ultrawide or 4:3 support, HUD scaling, stereo 3D output or LeiaSR weaving to such a port.
---

# Display features and stereo 3D for native PS4 ports

These ports load the PS4 executable, replace the system libraries with a host runtime, and
translate the GPU command stream with a shadPS4-style Vulkan backend (texture cache, rasterizer,
presenter with host passes). The game itself cannot be recompiled: everything is done by
binary patches (signature + offset + bytes) and by hooks in the renderer.

Work in this order, and check each step from evidence (logs, frame dumps), not assumptions.

## 1. Render resolutions and aspect ratios (binary patches)

**Find what to patch.** Typical sites:
- The output-size selection (min/cap/compare of width and height), and stores into globals the
  buffer setup reads.
- Hardcoded half-resolution buffer sizes. They must stay exactly half of the output, or variant
  tables keyed by `2*half < full` hit null entries and crash.
- The camera's aspect-ratio float (often `16/9` sitting next to `pi/360` in a literal pool). If
  there are several copies, change all of them together; changing one alone smears the picture
  through motion-blur reprojection.

**Understand the camera before changing the field of view.** Disassemble the projection
(capstone, rip-relative operand resolution) and identify the pattern: commonly
`t = tan(fov_deg * pi/360); y = 1/t; x = 1/(t*A)`, with A the aspect ratio. That means the
vertical FOV is fixed.
- **Wider than 16:9 (Hor+):** setting `A = w/h` alone widens the view. No code change is needed.
- **Taller than 16:9 (4:3 Vert+):** `A` alone crops the sides. Scale the `pi/360` constant so that
  `tan(f*c') = tan(f*c) * (16/9)/A` at the game's usual FOV. Proportions stay correct; only the
  amount of view varies slightly with dynamic FOV (speed effects).
- **Culling:** look for a horizontal culling half-angle derived as `vfov/2 * 16/9` (a separate
  constant fed to sin/cos for frustum planes). Compare it with the true half-angle
  `atan(tan(vfov/2)*A)`. Ultrawide (32:9) can exceed it and objects vanish at the edges: raise it
  to about 1.1x the true angle, and keep it under 90 degrees at the widest dynamic FOV.

**Generate entries with a tool, not by hand.** Take the sites from an existing tested entry. For
each new size, emit Pattern/At lines with values packed to the original operand width. Verify
that every signature is unique in the eboot. Prove the tool by regenerating the existing tested
entries byte for byte before trusting new ones.

Other details:
- **4K threshold:** widths of 3840 and above may flip a "4K mode" path (checkerboard). Keep the
  patch that disables it.
- **Test names:** many tests and launchers map built-in names ("1440p", "4K") to sizes. Support a
  `--name` option instead of always using `WxH`.
- **Limit the lists:** show only sizes that have a patch for the running game, in both the
  launcher and the in-game menu, via an allow-list in the per-game profile.

## 2. HUD on other screen shapes

Games lay the HUD out on a fixed 16:9 canvas and draw it into the display buffer after the scene.
In the rasterizer:
- **Count draws per display buffer:** the first draw into a newly bound display buffer is the
  scene composite, and later ones are HUD. This assumes the game rotates display buffers.
- **Remap HUD draws to a target area:** for HUD draws, map window x by `x*sx + ox` (narrower
  target than the buffer: centred horizontally). Do the same for y when the buffer is taller
  (letterbox). Apply it consistently to the viewport, scissor rect and the screen-space push
  constants of clip-disabled draws.
- **Expose a "HUD shape" setting** (16:9, 4:3, 16:10, 21:9, 32:9, stretch). Treat shapes within
  about 2% of the buffer as "fill", so 2560x1080 and 3440x1440 both count as 21:9.

## 3. Depth-based stereo 3D (side by side)

Prefer this over the game's VR mode. Faking a PSVR (HMD status READY, FOV tangents, camera
attached, VrTracker results) does switch games into VR rendering, but you inherit low per-eye
resolution, headset UI and fragile emulation. Rendering every draw twice is harder still:
passes have to be classified, and screen-space effects break.

Key lessons:
- **Snapshot the depth when the scene is composited, not at present time.** By the time a
  non-EOP flip reaches the presenter, the game has often already queued the next frame's depth
  clear, and the depth reads as all 1.0. Copy at the first draw into a new display buffer:
  - the scene depth: the last full-size, depth-written target with the display's shape (this
    excludes square shadow maps);
  - sample 0 of a multisampled depth goes into an R32F image via a tiny compute shader.
- **Snapshot the picture before the HUD,** with a copy at the second draw into the display
  buffer. At present time, pixels that differ from the snapshot are HUD: keep them unshifted (on
  the screen plane), and shift the scene from the snapshot. That avoids warped HUD text.
- **No matrices are needed for disparity.** With far much larger than near, standard depth gives
  `1 - d ≈ near/z` ("nearness"; for reversed-Z, use `d`). Parallax as a fraction of eye width is
  `separation * (1 - nearness/convergence)`, clamped by a pop-out limit.
- **Backward warp with a fixed point:** for the left eye, `src.x = uv.x + 0.5*P(src)`; for the
  right eye, minus. Iterate 3 to 4 times. Write a half-width side-by-side image, or full width
  (2W) when a weaver consumes it.
- **Do it as a host pass at the start of the presenter chain** (display image → stereo → FSR/post
  → swapchain), writing RGBA16F so the existing sRGB/HDR handling stays untouched.
- **Guard texture-cache lookups:** check that the image id is non-null before `TryGetImage`
  (`slot_vector` reads out of range otherwise), and keep the image uid to detect reuse.
- **Debug first:** a depth-view mode (nearness as grey, scaled by convergence) quickly shows
  whether you hold the right buffer. All black means a cleared depth or the wrong target. Also
  log every depth target per frame: size, samples, draw count, writing draws.
- **Make settings live:** depth, focus (convergence), pop-out and swap eyes can be atomics read
  every frame and editable from the in-game overlay.

## 4. LeiaSR / Simulated Reality displays

- **The SDK is not public** (provided on request, MIT licensed). The installed SR Platform
  runtime contains only DLLs. The weaver API is C++ (`SR::IDX11Weaver1`, `SR::SRContext`) built
  with MSVC.
- **A MinGW/clang port needs an MSVC bridge DLL with a plain C ABI.** The port loads it with
  `LoadLibrary` only when the output is selected, and keeps working without it.
- **The SDK headers drag in OpenCV** (`srcontext.h` → `transformation.h`). The SDK ships only
  per-module OpenCV libs, but the runtime ships `opencv_world*.dll`. Generate an import library
  from a `.def` that lists just the referenced symbol(s), using `lib /def:... /machine:x64`, so
  the bridge links against the DLL that actually exists.
- **Bridge flow:**
  1. Create the D3D11 device on the adapter whose LUID matches the Vulkan device
     (`VkPhysicalDeviceIDProperties`).
  2. Create the context with `SRContext::create(true)` (lens preference on), retrying while the
     service is unavailable.
  3. Create the weaver with `CreateDX11Weaver(ctx, d3d11Context, hwnd, &weaver)`, then call
     `setShaderSRGBConversion(true, true)` for sRGB-encoded UNORM data.
  4. Call `context->initialize()`, and use `SwitchableLensHint` to toggle the lens.
- **Shared textures:** create both in D3D11 (B8G8R8A8_UNORM, `SHARED | SHARED_NTHANDLE`) and
  export NT handles. In Vulkan, enable `VK_KHR_external_memory_win32` and create the image with
  `ExternalMemoryImageCreateInfo{eD3D11Texture}`. Allocate with
  `ImportMemoryWin32HandleInfoKHR` plus a dedicated allocation. Keep the output image in GENERAL.
- **Weave offscreen at the window's exact client size.** `weave()` writes to the currently bound
  RTV. Copy the result into the frame and present it 1:1 (frame size == swapchain extent). Any
  scaling or offset breaks the lenticular alignment. The window must be borderless or fullscreen
  on the 3D display.
- **Inputs:** both views side by side at full width (`setInputViewTexture(srv, viewW, viewH,
  fmt)`, with `viewW` being one eye's width).
- **First version, CPU synchronisation:** finish the Vulkan work, weave, wait on a D3D11 event
  query, then copy. It's simple and correct but costs throughput; a keyed mutex or shared fence
  is the upgrade.
- **Fall back to side by side** without the bridge, the service, an 8-bit BGRA swapchain, or in
  HDR, and log the reason.
- **Packages include the bridge only when built where the SDK is installed.** Builds without
  the SDK still work, with side-by-side 3D only.

## 5. Settings persistence pitfalls

- **The menu's save drops unknown keys.** If the in-game menu rewrites the settings file with
  only the keys it knows, every launcher-only key is lost on the first save. Teach the game's
  parser and writer every key the launcher writes.
- **Live settings:** keep them as atomics in the shared settings struct, so the renderer reads
  them per frame. Startup-only ones go through the launcher → environment variables.
- **Launcher translation tests** usually require every literal UI string in every table; add
  translations together with new controls.

## 6. Toolchain and build pitfalls (Windows)

- **MSYS2 CLANG64 for the port:** a login shell starts in its home directory, so pass
  `cd <repo> &&` inside `bash -lc`. Mirrors can rate-limit; retry pacman.
- **`core.autocrlf=true` breaks submodule patches** ("patch does not apply"). Set autocrlf false
  in the submodule and re-checkout it.
- **Packaging that freezes the launcher needs a python.org Python.** The Microsoft Store Python
  alias cannot be executed from MSYS2.
- **MSVC for the bridge:** install Visual Studio Build Tools with the C++ workload (VCTools, a
  Windows SDK, the CMake component).
- **Shell-escaping:** `\n` inside generated C strings gets mangled when passed through nested
  shell heredocs. Write edit scripts to files, and build backslashes with `chr(92)` when
  generating source text.

## 7. Verifying without seeing the screen

- **Use the engine's own frame dump** (a trigger file that dumps the display buffer and the
  presented image as raw pixels), converted to PNG. Don't use desktop screenshots: other windows
  cover the game, and they capture private content.
- **Check the format before judging colours:** the display buffer may be 10-bit (A2R10G10B10), so
  decode it accordingly. Swapchain dumps are usually BGRA.
- **Tell real failures from intermittent start-up hangs:** compare log line counts across
  several boots, with and without your change, before blaming it. Run the same check on an
  unmodified build when a test or behaviour looks broken.
- **Resolve crash addresses in host code** with `llvm-addr2line -f -C -e <exe> <ImageBase+offset>`.
