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
