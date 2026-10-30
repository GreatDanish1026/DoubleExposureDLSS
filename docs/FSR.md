# Adding AMD FSR next to DLSS (design, 2026-10-30)

**Status (1.1.0, 2026-10-30): FSR upscaling built and installed, untested in game.** `src/fsr.cpp`
(context per output size, retired 8 frames later), Upscaler setting Auto/DLSS/FSR (ini `Upscaler`
0/1/2, `FsrSharpness`), bias mask passed as FSR's reactive mask, camera near/FOV from View.ViewToClip
(byte 512), frame time from presents. Advanced has "Flip FSR's jitter" to check the jitter sign.
DLSS frame generation still tags inputs with FSR upscaling. FSR frame generation not started.

**1.1.1–1.1.3 (2026-10-30, tested by the user):** FSR 3.1.6 showed noise on trees and the satchel.
Jitter flip and AMD's tuning keys (velocity factor, min disocclusion, shading change) made no
difference. Cause: the translucency part of the bias mask (compose.hlsl's additive-light term:
translucency brightness / scene brightness × 0.5) marks faint haze over dark surfaces, and FSR
treats its reactive mask as "drop history". DLSS tolerates it. With the translucency mask off FSR
is clean and the flashlight beam, particles and light shafts show no trails. So the translucency
mask is kept per upscaler: DLSS Partial (`TranslucencyMask`), FSR Off (`FsrTranslucencyMask`).
The animation mask stays on for both (this game sets no animation flag). Camera check:
ViewToClip [2][2] = 0, so UE's far plane is infinite (FFX_UPSCALE_ENABLE_DEPTH_INFINITE right).
Advanced also has AMD's FSR debug view and the three tuning sliders (AMD's defaults).
Jitter sign: no visible difference either way; kept as DLSS's.

Goal: one add-on where the player picks **DLSS** or **FSR** as the upscaler. Start with Double
Exposure, then port to ReunionDLSS and TrueColorsDLSS.

## Why it fits

Everything game-specific is already upscaler-neutral. `upscaler.cpp` finds TSR's passes, builds
our own colour (with translucency blended in), depth, motion vectors (previous minus current, render
pixels), jitter, render/output size and a "trust this frame" mask, then calls `dlss::Evaluate`
right after the game's ResolveHistory dispatch and overwrites TSR's output. FSR needs exactly the
same inputs, so it becomes a second backend beside `dlss.cpp`; the TSR hook, motion shader,
translucency compose, cvar code and frame-generation tagging stay as they are.

| Input | DLSS today | FSR (FidelityFX SDK 2.x, `ffxDispatchDescUpscale`) |
|---|---|---|
| Colour | linear HDR, IsHDR flag | same, `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE` |
| Depth | inverted flag | `FFX_UPSCALE_ENABLE_DEPTH_INVERTED` |
| Motion | render px, prev minus current, scale 1 | same convention, `motionVectorScale` 1 |
| Exposure | AutoExposure flag | `FFX_UPSCALE_ENABLE_AUTO_EXPOSURE` |
| Jitter | render px from TSR param +256 | `jitterOffset`, render px (sign to verify once in game) |
| Bias mask | `pInBiasCurrentColorMask` | FSR 3.1 reactive mask; translucency coverage as transparency & composition mask (FSR 4 ignores both) |
| Output | UAV, r11g11b10f | UAV; typed UAV store of r11g11b10f is allowed in D3D12 |
| States | SRV / UAV | inputs NON_PIXEL_SHADER_RESOURCE, output UAV |
| Heaps | NGX sets its own; `Restore` puts the game's back | FFX sets its own; same `Restore` |
| Extra | — | `frameTimeDelta` (ms), `sharpness` (RCAS), `cameraNear/Far/FovY` (from View UB) |

FSR's context is created with the maximum render and output size, so render-size changes (mode
switches) don't need a new context the way DLSS features do; only an output-size change does.

## Which FSR

AMD FSR SDK 2.4.0 (28 Oct 2026). Headers are MIT; the signed DLLs (loader 39 KB + upscaler 57.7 MB,
plus frame generation if wanted) are under AMD's binary licence (redistribution allowed with its
notice: `third_party/fidelityfx/license.md`, packaged as licenses/AMD-FSR-LICENSE.txt). The app asks
for the newest upscaler and the runtime picks:
- **FSR 4.1 (ML)** on Radeon RX 9000, RX 7000 and RX 6000 discrete GPUs and some Ryzen AI APUs.
- **FSR 3.1.6** (analytic) on everything else, including NVIDIA and Intel GPUs.

Needs DirectX 12 (shader model 6.6). True Colors' DX11 mode stays unsupported, as with DLSS.

## Overlay / settings

- `Upscaler`: Off / DLSS / FSR. Default: DLSS if NGX starts (RTX), else FSR. Ini key `Upscaler`;
  old `Enabled` maps to DLSS.
- `Render resolution`: same list, generic names: Game's / Native AA (100%) / Quality 67% /
  Balanced 58% / Performance 50% / Ultra Performance 33%.
- DLSS only: Model (preset). FSR only: Sharpness slider, and Status shows "FSR 4.1" or "FSR 3.1.6".
- Animation mask / translucency options apply to both.
- F9 cycles Off → DLSS → FSR (or toggles the chosen one).

## Frame generation (separate phase)

DLSS-G (Streamline) only runs on RTX 40/50. FSR frame generation runs on any DX12 GPU, which is
what most FSR users will want next, and DLSS SR + FSR FG is a common combo on RTX 20/30.
- Same hook point: our CreateSwapChainForHwnd hook would create FFX's frame-generation swapchain
  (`ffxCreateContextDescFrameGenerationSwapChainForHwndDX12`) instead of Streamline's proxy.
  Only one of the two can own the swapchain, so the FG choice is made at launch (restart to change),
  as FG on/off already is.
- Inputs we already make for DLSS-G: depth copy, motion vectors, camera (View UB), hudless copy
  and HUD mask (Reunion/Double Exposure in HDR). FFX wants these per frame via its dispatch
  callback plus camera near/far/FOV.
- Bigger and riskier than the upscaler (pacing, HUD handling, present hooks): do after FSR SR
  ships.

**Status (1.2.0, 2026-10-30): built, untested in game.** `src/fsrfg.cpp` + framegen.cpp: Technology
setting DLSS/FSR (ini `FrameGenTech` 0/1, read at launch), FSR mode Off/2x. FSR path: our factory
hooks create FSR's swapchain (FOR_HWND / NEW), present hooks call configure + prepare before each
Present (prepare in our own command list on the game queue, after the frame's lists), inputs taken
at submission like Streamline's tags. Swapchain Release hooked: when the game's last reference
goes (count probed with AddRef/Release vs FSR's own references counted at creation), the FG
context (disabled first) and the swapchain context are destroyed. HDR is assumed when UE made the
pre-HUD copy (UE's SDR back buffer is 10-bit too). Frame-gen context recreated on back buffer
size/format/HDR change.

### FSR frame generation design (from SDK 2.4.0 docs, 2026-10-30)

Runtime: `amd_fidelityfx_framegeneration_dx12.dll` (29.7 MB) next to the exe, same loader. FSR FG
3.1.7 (analytical) runs on any DX12 GPU; FSR FG 4.0.2 (ML) only on RX 9000 + Windows 11, picked by
the runtime. One generated frame per rendered frame (2x).

API, per AMD's required order each frame:
1. Swapchain: in our CreateSwapChainForHwnd hook, `ffxCreateContext` with
   `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12` (hwnd, desc, fullscreen desc, the real
   factory, the game's queue) returns FFX's proxy `IDXGISwapChain4`; the game gets that. Same place
   Streamline's proxy is made today, so DLSS FG and FSR FG are a launch-time choice.
2. A frame-generation context (`ffxCreateContextDescFrameGeneration`: display size, max render
   size, back buffer format, flags HDR | DEPTH_INVERTED | DEPTH_INFINITE; plus
   `ffxCreateContextDescFrameGenerationHudless` if the pre-HUD copy's format differs).
3. Each frame: `ffxConfigure(ffxConfigureDescFrameGeneration)` with the proxy swapchain,
   `frameGenerationCallback` = a function calling `ffxDispatch` on our context (AMD's recommended
   path; the swapchain calls it at present), `HUDLessColor` = our pre-HUD copy (HDR only), frameID
   +1 per frame, enabled flag.
4. Each frame: `ffxDispatch(ffxDispatchDescFrameGenerationPrepareV2)` on a command list: depth,
   our motion vectors (render px, scale 1), jitter, render size, frame time, reset, near / FOV /
   0.01 m per unit, camera position / up / right / forward (already computed for DLSS-G from the
   View buffer).

Reuse: depth copy, motion vectors, camera matrices, pre-HUD copy, frame bookkeeping in OnExecute.
To tie the prepare pass to the right frame (like Streamline's tags), it's recorded in our own
command list and submitted on the game's queue right after the game's list that holds the inputs.

Thread safety: AMD says prepare-dispatch, context create/destroy and the swapchain's present must
not run at the same time: one mutex around them (the callback runs inside Present).

Shutdown: Configure with frameGenerationEnabled = false (waits for presents), destroy the FG
context, then the swapchain context.

Not available on NVIDIA with FSR FG: Reflex (FFX paces itself; latency is higher than DLSS FG).
SDR: no pre-HUD copy (UE draws the HUD into the back buffer), so the HUD may warp, as with DLSS FG.

Risks to test: ReShade's own swapchain wrapping with FFX's proxy (worked for Streamline's), the
present-time callback running on FFX's thread, HDR10 transfer function, resize/fullscreen
switches, exit (shutdown order; DLSS FG had an exit abort that needed a recursive mutex).

## Work list (Double Exposure)

1. Download FidelityFX SDK 2.4.0 into `third_party/fidelityfx` (headers in git, DLLs gitignored
   like Streamline's).
2. `src/fsr.cpp/.hpp` mirroring `dlss.hpp`: Init (load loader DLL, query versions), Evaluate
   (context per output size, retire like DLSS features), EndFrame, Shutdown, Status.
3. `upscaler.cpp`: `Upscaler` setting, call the chosen backend, generic mode names, FSR
   sharpness, F9; frame time from Present.
4. `build.ps1` -Install/-Package: add the AMD DLLs and their licence; README section.
5. Test on the user's RTX: FSR 3.1 path (jitter sign, mask strength, translucency, mode switches,
   history reset). FSR 4 path needs a Radeon tester.
6. Release (name: keep `DoubleExposureDLSS` or rename, e.g. `DoubleExposureUpscaler`).

Ports: ReunionDLSS (UE 5.6 TSR, same structure) is a near copy; TrueColorsDLSS (UE 4.25
TAA/TAAU, DX12 only) needs the same backend with its TAA hook.
