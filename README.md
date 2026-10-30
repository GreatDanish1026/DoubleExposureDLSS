# DoubleExposureDLSS 1.2.0

ReShade add-on for **Life is Strange: Double Exposure** (Unreal Engine 5.2, DirectX 12) that
replaces the game's TSR upscaler with **NVIDIA DLSS** or **AMD FSR**, and adds **DLSS Frame
Generation** or **FSR Frame Generation**. The game has none of these (its menu's FSR switch does
nothing). It's separate from
[DoubleExposureTweaks](https://github.com/GreatDanish1026/DoubleExposureTweaks) (graphics
settings), and the two can be installed side by side.

| | Graphics card |
|---|---|
| DLSS upscaling | NVIDIA RTX |
| FSR upscaling | any DirectX 12 card: FSR 4 on Radeon RX 6000 and newer, FSR 3.1 elsewhere |
| DLSS Frame Generation | NVIDIA RTX 40 or 50 (3x, 4x and Dynamic: RTX 50), hardware-accelerated GPU scheduling |
| FSR Frame Generation | any DirectX 12 card: FSR 4 on Radeon RX 9000 under Windows 11, FSR 3.1 elsewhere |

ReShade 6.8 with add-on support. Upscaler and frame generation can be mixed (for example DLSS
upscaling with FSR Frame Generation on an RTX 30 card).

## What it does

- **Upscaler** (F9 to switch on and off): Auto (DLSS on NVIDIA RTX, FSR elsewhere), DLSS or FSR in
  place of TSR. DLSS Super Resolution 310.9.16; FSR through AMD's FSR SDK 2.4.0, which picks FSR 4
  or FSR 3.1 for the card. Motion vectors are computed by the add-on from the depth buffer and the
  camera, plus the engine's own motion of moving objects (characters, foliage in the wind, cloth).
  The game draws its translucent effects (particles, light beams, light shafts, outlines) on a
  separate layer that its own upscaler blends in; the add-on blends that layer in first, and
  "Translucent effects" (Advanced, kept per upscaler) keeps them from smearing.
- **Render resolution**: Native/DLAA, Quality, Balanced, Performance or Ultra Performance set the
  render resolution while the upscaler is on; "Game's Resolution Scale" leaves it to the game's
  menu. Keep the game's Secondary Resolution Scale at 100%.
- **DLSS model**: K (transformer, default), J, L, M or NVIDIA's default. **FSR sharpness**: 0-1.
- **Frame generation**: Off, DLSS 2x / 3x / 4x / Dynamic (NVIDIA Streamline 2.14.16, with Reflex),
  or FSR 2x (AMD's frame generation swapchain; no Reflex, so a little more latency). It runs in
  game, not in menus (the menus don't use the game's upscaler). In HDR the HUD and subtitles are
  kept from warping with the picture the game draws before its HUD. Switching it on, or between
  DLSS and FSR, takes a game restart; DLSS's multipliers switch right away.

The overlay (ReShade's Add-ons tab) shows a one-line summary of the upscaler and frame generation
at the top, with **Settings**, **Status** and **Advanced** (testing tools, FSR debug view, frame
capture) tabs. Settings are saved in `ReShade.ini` under `[DoubleExposureDLSS]`.

## Install

1. Install ReShade 6.8 or later with full add-on support for `Chronos-Win64-Shipping.exe`, in
   `LifeIsStrangeDoubleExposure\Chronos\Binaries\Win64`.
2. Copy everything from the release zip into that `Win64` folder: `DoubleExposureDLSS.addon64`,
   `nvngx_dlss.dll`, the three `amd_fidelityfx_*_dx12.dll` files and the `streamline` folder (the
   licence files can go along).
3. Start the game, open ReShade (Home key), Add-ons tab, DoubleExposureDLSS: switch the upscaler on
   (or press F9), pick DLSS or FSR and a render resolution and, if you like, frame generation
   (takes effect after a restart).
4. In the NVIDIA App, a DLSS model override for the game takes precedence over DLSS model.

The zip includes NVIDIA's DLSS 310.9.16 and Streamline 2.14.16 runtime files, signed by NVIDIA
(the add-on checks Streamline's signature before loading it), and AMD's FSR SDK 2.4.0 runtime
files, signed by AMD. Their licences are in `licenses`, and attribution is in
`THIRD_PARTY_NOTICES.txt`.

Tested with Steam build 16505703 at 3840x2160 with HDR on, on an NVIDIA RTX card (DLSS, FSR 3.1
upscaling, both frame generations). FSR 4 on Radeon cards goes through the same code but hasn't
been tested on one. Play at your screen's resolution: a higher one left in GameUserSettings.ini
makes the game render an extra view that frame generation can't keep up with. With HDR off, frame
generation runs without the HUD protection (the game then draws its HUD straight onto the screen).

To uninstall, delete those files. Logs: `DoubleExposureDLSS.log` next to the game's exe,
Streamline's in `streamline\sl.log`.

## Build

`.\build.ps1` (Visual Studio 2022 C++ tools, Windows SDK for `fxc`). The runtimes aren't in the
repository: unzip `bin\x64` of NVIDIA's Streamline 2.14.16 release into
`third_party\streamline\bin\x64`, and copy `amd_fidelityfx_loader_dx12.dll`,
`amd_fidelityfx_upscaler_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll` from
`Kits\FidelityFX\signedbin` of AMD's FSR SDK 2.4.0 into `third_party\fidelityfx\bin`.
`.\build.ps1 -Install` copies the add-on and the runtimes into the game folder (game closed);
`.\build.ps1 -Package` makes the release zip in `bin\release`.

## How it works

Built from ReunionDLSS (Life is Strange: Reunion, UE 5.6). `docs/FINDINGS.md` has what differs in
UE 5.2: TSR's nine passes and their shader ids, the chain linked at DecimateHistory, the
square-root encoding of object velocity, the View uniform buffer offsets, and frame generation's
inputs handed over when the game submits each frame. `docs/FSR.md` covers FSR: the upscaler next to
DLSS, why FSR's mask leaves translucency out, and FSR Frame Generation's swapchain, per-frame
configure and prepare, and shutdown order. The F10 frame capture (Advanced tab) writes two frames
of GPU commands to `DoubleExposureDLSS-frame-N.txt`, for finding TSR's passes again after a game
update.
