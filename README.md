# DoubleExposureDLSS

A ReShade add-on that will put NVIDIA DLSS (and DLSS Frame Generation) in place of TSR in
**Life is Strange: Double Exposure** (Unreal Engine 5.2, DirectX 12). Built from ReunionDLSS.
Separate from DoubleExposureTweaks.

**0.1.0: TSR is mapped; DLSS is untested in game.** Frame generation is untested too. (0.0.1 was a mapping build:
frame capture still on the Status tab, F10.)

## Install

ReShade 6.8 or newer with add-on support for `Chronos\Binaries\Win64\Chronos-Win64-Shipping.exe`.
`.\build.ps1 -Install` (game closed) copies the add-on, `nvngx_dlss.dll` and the Streamline runtime
(`streamline\`) next to the exe.

## Build

Visual Studio 2022+ with the C++ x64 tools. NVIDIA's DLLs come from the Streamline SDK 2.14.16
release (`third_party\streamline\bin\x64`, not in the repository). See `docs/FINDINGS.md`.
