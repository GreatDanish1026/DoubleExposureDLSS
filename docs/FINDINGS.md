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

## To map (needs a capture)

- TSR's shader ids (common.hpp; placeholders and `kTsrMapped = false` until then).
- Which pass binds depth and velocity (DilateVelocity in 5.6: t0, t1), the scene colour feed, and
  the output; their registers.
- TSR's common parameters: render size, jitter (5.6: offset 256), motion vector scale (272).
- View.ClipToPrevClip's offset in the View uniform buffer (5.6: 2176; found at run time anyway).
- Velocity buffer format (5.6: rgba16unorm; motion.hlsl decodes it).
- HUD composite for frame generation (5.6: the HUD alpha was inverted).

0.0.1's capture writes every compute pass in full (bound textures by register, the first 1 KB of
each constant buffer, the View buffer once per frame) because TSR's passes aren't known yet.
