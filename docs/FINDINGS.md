# DoubleExposureDLSS: findings

Game facts (engine, paths, cvar layout): see DoubleExposureTweaks' `docs/FINDINGS.md`. Built from
ReunionDLSS 1.0.1 (Life is Strange: Reunion, UE 5.6); its `docs/FINDINGS.md` and `docs/FRAMEGEN.md`
explain the method (TSR hook, own motion vectors, NGX state restore, frame generation).

## Upscaler today

Only TSR (`r.AntiAliasingMethod=4`); no DLSS, NGX, Streamline or XeSS in the exe. The menu's FSR
switch names `r.FidelityFX.FSR3.Enabled`, which isn't a registered cvar (does nothing).

TSR settings (probe, 2026-10-26): `r.TSR.AsyncCompute=0` (graphics queue), `r.TSR.History.ScreenPercentage=200`,
`r.TSR.History.UpdateQuality=3`, `r.TSR.History.R11G11B10=1`, `r.TSR.RejectionAntiAliasingQuality=2`,
`r.TSR.ShadingRejection.Flickering=1`, `r.TemporalAASamples=8`. The user plays at 3840x2160, 70%
resolution scale, HDR10.

UE 5.2's TSR shader files (from the exe): ClearPrevTextures, ForwardScatterDepth, DilateVelocity,
DecimateHistory, ComputeMoireLuma, RejectShading, SpatialAntiAliasing, FilterAntiAliasing,
UpdateHistory, ResolveHistory (plus CompareTranslucency, DebugHistory). Reunion's 5.6 build ran six
passes. Named TSR textures include `TSR.Velocity.Dilated`, `TSR.ClosestDepthTexture`,
`TSR.History.ColorArray`, `TSR.Output`, `TSR.HalfResOutput`, `TSR.QuarterResOutput`.

## Differences from Reunion that the code already handles

- cvars: UE 5.2 layout (DoubleExposureTweaks' `cvars.hpp`); the DLSS mode's `r.ScreenPercentage`
  write is copied to the render thread by the add-on.

## TSR map (capture 2026-10-26, gameplay, 3840x2160 at 100% resolution scale)

`DoubleExposureDLSS-frame-1.txt`: two frames, 518 dispatches. TSR is nine compute passes in one
command list on the graphics queue, found by TSR's common parameter block (root CBV param 2, history
size 7680x4320 at byte 112). Render textures are allocated at the full 3840x2160 whatever the
resolution scale; the view size comes from the parameters.

| shader id | groups (at 100%) | pass | key bindings |
|---|---|---|---|
| `42e09aac` | 240x135 | ClearPrevTextures | u0-u2 r32uint |
| `02016352` | 480x270 | ForwardScatterDepth | t0 r32uint -> u0 r32uint |
| `cf6ec20a` | 480x270 | **DilateVelocity** | t0 depth `r32g8x24_typeless`, t1 velocity `rgba16unorm` -> u0 dilated velocity `rg16unorm`, u1 closest depth r16f |
| `67aa2e5d` | 480x270 | **DecimateHistory** | t0 = DilateVelocity's u0, t1 closest depth |
| `bd30efd9` | 320x180 | RejectShading | t0 scene colour rgba16f, t2 rgba16f |
| `d9c8c910` | 480x270 | SpatialAntiAliasing | |
| `d77fb051` | 480x270 | FilterAntiAliasing | |
| `23742aef` | 960x540 | **UpdateHistory** | t0 scene colour rgba16f, t1 rgba16f -> u0 history 7680x4320 rgba16f |
| `d14da7eb` | 480x270 | **ResolveHistory** | t0 history -> u0 output 3840x2160 r11g11b10f, u1 1920x1080 half-res copy |

ComputeMoireLuma didn't run (no pass between DilateVelocity and DecimateHistory).

TSR's common parameters (param 2) have the same layout as UE 5.6's: 0 extent, 32 ViewportMin/Max,
48 ViewportSize, 112 history size, 256 jitter in render pixels (`-0.375, -0.0556` in the capture),
272 ScreenVelocityToInputPixelVelocity (1920, -1080 at 100%). The View uniform buffer is root CBV
param 3 (as in 5.6); ClipToPrevClip's offset is searched at run time (Reunion's 2176 is skipped).

Differences from 5.6 handled in 0.1.0: the chain is linked at DecimateHistory's t0 (5.6:
RejectShading's t8); the scene colour feed is UpdateHistory's t0 as in 5.6 (5.2 has no separate
RejectShading feed). The output is r11g11b10f even with HDR output (DLSS accepts it).

## First DLSS test (0.1.0, 2026-10-26)

Log: View.ClipToPrevClip found at **byte 1872** of the View uniform buffer; NGX 310.9.16 (NVIDIA App
override); DLAA 3840x2160, Quality 2561x1441 and Balanced 2228x1253 all evaluated; presets J/K/L/M
switched; F9 off/on restored the game's 100% scale. No errors.

User saw **noise in the trees and smearing on the character's satchel**. Likely cause: meshes animated
in their vertex shader (wind on foliage, cloth) write no velocity, so our motion vectors give them
the camera's motion only; TSR's rejection hides that, DLSS doesn't.

0.1.1 set `r.Velocity.EnableVertexDeformation=1` while DLSS was on: **the game hung** right after
(log stops inside the Set). Changing it recreates every primitive's render state, which has to run
on the game thread; ReShade's present thread deadlocked. It was pointless anyway: the game's
values (logged) are `r.Velocity.EnableVertexDeformation=2` (auto: on while TSR is the AA method,
which it stays under DLSS), `r.VertexDeformationOutputsVelocity=-1`, `r.BasePassOutputsVelocity=-1`,
`r.Velocity.ForceOutput=0`, `r.VelocityOutputPass=1`, `r.SkinCache.Mode=2`, `r.HairStrands.VelocityType=1`.
0.1.2 only logs them. Lesson: never set cvars with change delegates that touch render state from
ReShade's thread.

So wind-animated foliage should already write velocity. Remaining suspects for the tree noise and
satchel smear: (1) UE 5.2's velocity flag for pixel/vertex animation (TSR treats those pixels
specially; DLSS has a "bias current colour" mask for exactly this), (2) the satchel being
CPU-simulated cloth without previous positions, (3) the object velocity decode in motion.hlsl.

## Animation mask (0.2.0)

motion.hlsl also writes an r8 mask at render resolution, passed to DLSS as the "bias current colour"
mask (trust this frame over history there). Source: UE 5.1+ packs a "pixel animation" flag into the
lowest bit of the encoded velocity's w (rgba16unorm velocity: z and the rest of w hold depth); TSR
uses it to distrust history. Overlay "Animation mask": Off / Animated materials (flag; default) /
Animated materials and moving objects (also every pixel with object velocity). "Show mask (debug)"
tints masked pixels red in the output (tint.hlsl, after DLSS, before post-processing). Untested:
whether Deck Nine's foliage and cloth materials set the flag is what the debug view shows.

### Result (2026-10-26)

"Animated materials": nothing turned red, so the game's materials don't set the pixel-animation
flag. "Animated materials and moving objects": the trees and the satchel turned red, so they write
object velocity (WPO velocity is on). The mask over them changed nothing: no less noise in the trees,
no less smearing on the satchel. So the history isn't the problem; the object velocity DLSS gets for
them likely is (the decode in motion.hlsl came from 5.6). 0.2.1 adds a diagnosis switch, "Object
motion": linear decode (as before), square-root decode (VELOCITY_ENCODE_GAMMA), or ignore object
velocity; and preset E (CNN) to check whether the transformer presets ignore the mask.

## Two TSR views (0.2.2)

The game runs TSR twice a frame: the 3840x2160 screen and a 4096x2160 view (2868x1512 render at
70%; possibly the other timeline). Each output gets its own DLSS feature. A DLSS mode change gives
TSR new output textures, so 0.2.1 reached its four-feature limit and refused the fifth ("too many
TSR views"; that view stayed on TSR). 0.2.2 releases features unused for 3 frames before creating
a new one.

## Open

- Whether UpdateHistory's t0 includes translucency (t1, rgba16f, may be separate translucency).
- View.ClipToPrevClip offset (logged on the first DLSS frame).
- Frame generation: the HUD-less copy and HUD alpha were found for 5.6; untested here.

## Mapping notes (before the capture)

- TSR's shader ids (common.hpp; placeholders and `kTsrMapped = false` until then).
- Which pass binds depth and velocity (DilateVelocity in 5.6: t0, t1), the scene colour feed, and
  the output; their registers.
- TSR's common parameters: render size, jitter (5.6: offset 256), motion vector scale (272).
- View.ClipToPrevClip's offset in the View uniform buffer (5.6: 2176; found at run time anyway).
- Velocity buffer format (5.6: rgba16unorm; motion.hlsl decodes it).
- HUD composite for frame generation (5.6: the HUD alpha was inverted).

0.0.1's capture writes every compute pass in full (bound textures by register, the first 1 KB of
each constant buffer, the View buffer once per frame) because TSR's passes aren't known yet.
