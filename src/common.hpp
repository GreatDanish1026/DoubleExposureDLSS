// Shared between the capture tool (main.cpp), the TSR hook (upscaler.cpp) and the NGX wrapper
// (dlss.cpp).
#pragma once

#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include <reshade.hpp>

#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace rd {

// DoubleExposureDLSS.log next to the game exe.
void Log(const char* fmt, ...);

// The folder of the game exe, with a trailing backslash.
const std::wstring& ExeDir();

// FNV-1a of a compute pipeline's shader bytecode (0 if unknown): stable across runs.
uint32_t PipeId(reshade::api::pipeline pipe);

// TSR's compute shaders in this game build 16505703 (UE 5.2.1; capture 2026-10-26,
// docs/FINDINGS.md), in the order they run, all in one command list on the graphics queue.
constexpr bool kTsrMapped = true;
constexpr uint32_t kTsrClearPrevTextures = 0x42e09aac;
constexpr uint32_t kTsrForwardScatterDepth = 0x02016352;
constexpr uint32_t kTsrDilateVelocity = 0xcf6ec20a;     // t0 depth, t1 velocity -> u0 dilated velocity
constexpr uint32_t kTsrDecimateHistory = 0x67aa2e5d;    // t0 = DilateVelocity's u0 (links the chain)
constexpr uint32_t kTsrRejectShading = 0xbd30efd9;
constexpr uint32_t kTsrSpatialAntiAliasing = 0xd9c8c910;
constexpr uint32_t kTsrFilterAntiAliasing = 0xd77fb051;
constexpr uint32_t kTsrUpdateHistory = 0x23742aef;      // t0 scene colour -> u0 7680x4320 history
constexpr uint32_t kTsrResolveHistory = 0xd14da7eb;     // t0 history -> u0 output, u1 half-res copy

// upscaler.cpp
void RegisterUpscaler();
void UnregisterUpscaler();
// Overlay: the Settings tab and the Status tab.
void DrawUpscalerSettings();
void DrawUpscalerStatus();

// Overlay helpers (main.cpp), in the Tweaks add-ons' style.
float ControlWidth();
void Help(const char* text);  // a "(?)" after the last item, with <text> as its tooltip
extern const ImVec4 kWarn;
// Puts the game's pipeline, root signatures, descriptor heaps and bindings back on <cl> after
// the add-on recorded work of its own on it.
void RestoreListState(reshade::api::command_list* cl);

// framegen.cpp: DLSS Frame Generation through Streamline.
void RegisterFrameGen();
void UnregisterFrameGen();
void DrawFrameGenSettings();
void DrawFrameGenStatus();
// The View uniform buffer (first 16 KB) of this frame's main view, with View.ClipToPrevClip's offset.
void FrameGenSetCamera(const float* view, int clipToPrevClipOffset);
// This frame's depth (UE's, state depth read + shader resource) and our motion vectors (render
// pixels, previous minus current; state non-pixel shader resource), tagged on <list>.
void FrameGenTag(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, unsigned renderWidth,
                 unsigned renderHeight, float jitterX, float jitterY, bool reset);

}  // namespace rd
