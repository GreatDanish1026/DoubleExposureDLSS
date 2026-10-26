# DoubleExposureDLSS

A ReShade add-on that will put NVIDIA DLSS (and DLSS Frame Generation) in place of TSR in
**Life is Strange: Double Exposure** (Unreal Engine 5.2, DirectX 12). Built from ReunionDLSS.
Separate from DoubleExposureTweaks.

**0.0.1 is a mapping build:** DLSS and frame generation are locked off until TSR's passes in this
game are known. It captures frames for that (overlay, Status tab, **Capture frame (F10)**).

## Install

ReShade 6.8 or newer with add-on support for `Chronos\Binaries\Win64\Chronos-Win64-Shipping.exe`.
`.\build.ps1 -Install` (game closed) copies the add-on, `nvngx_dlss.dll` and the Streamline runtime
(`streamline\`) next to the exe.

## Build

Visual Studio 2022+ with the C++ x64 tools. NVIDIA's DLLs come from the Streamline SDK 2.14.16
release (`third_party\streamline\bin\x64`, not in the repository). See `docs/FINDINGS.md`.
