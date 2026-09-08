# DLSSNR-Cost-Scaler

A standalone proxy DLL for NVIDIA DLSS-NR (DirectX 12) that adds resolution scaling and cost control. It runs the neural reconstruction model at a reduced resolution while keeping native 1:1 geometry, fine textures, text, and edges intact using a high-frequency matched residual composite shader.

Designed primarily to work alongside RenoDX addons, this proxy decouples DLSS-NR's GPU performance cost from the display resolution without introducing blur.

Tested specifically with clshortfuse's DLSS addon (`renodx-dlss.addon64`), but architected to work with any game, engine, or injector that calls `nvngx_dlssnr.dll` over DirectX 12.

---

## Features

- Standalone drop-in proxy for `nvngx_dlssnr.dll`.
- **Hardware Bilinear TMU Downsampling:** Accelerates input downsampling through dedicated GPU texture management units (TMU) via static linear clamp samplers.
- **LDS On-Chip Tile Caching:** RCAS sharpening uses 1.2 KB of Local Data Share (LDS) per threadgroup, dropping global VRAM transactions by ~69%.
- **In-Place Resolve & VRAM Optimization:** Eliminates redundant intermediate scratch buffers when the output UAV is writable, saving 66MB–132MB of VRAM.
- **High-Frequency Matched Residual Resolve:** Composites the neural reconstruction delta onto the untouched 1:1 native frame to preserve razor-sharp textures and geometry.
- **Super-Sampling Support (0.25x – 2.00x):** Supports super-sampling up to 2.0x (4K SSAA / DLDSR / Photo Mode) for extreme fidelity capture.
- **Anamorphic / Asymmetric Neural Scaling:** Decouples horizontal and vertical scaling (e.g. 0.65x X / 0.85x Y) with independent per-axis motion vector scaling for ~45% neural workload reduction and flat, stutter-free frame pacing.
- **Depth-Aware Bilateral Silhouette Preservation:** Uses native depth to guard geometry silhouettes and prevent neural bleeding.
- **Luminance-Bounded HDR Composite:** Prevents highlight clipping, fireflies, and shadow float in HDR10 PQ and scRGB scenes.
- **Color / Tint Strength Control:** Separate luminance and chroma sliders to eliminate neural color casts while keeping full detail.
- **Caller Parameter Passthrough by Default:** Never overrides upstream parameters set by OptiScaler, RenoDX, or game menus unless explicitly opted-in (`UseCustomSettings = 1`).
- **Comprehensive D3D12 Format Support:** Tested with SDR (`B8G8R8A8` / `R8G8B8A8`), HDR10 PQ (`R10G10B10A2`), scRGB (`R16G16B16A16_FLOAT`), and 3-channel HDR (`R11G11B10_FLOAT`).
- **Dynamic Subrect Tracking:** Correctly preserves viewport offsets for dynamic resolution scaling (DRS) and mod injectors.
- **In-game hot-reloading:** Changes made to `nvngx_dlssnr.ini` take effect live within one second.
- **In-game hotkeys:** Shortcuts for toggling proxy, switching resolve modes, and adjusting scaling.

---

## Requirements

- Windows 10/11 (64-bit)
- NVIDIA RTX GPU (RTX 20, 30, 40, or 50 series)
- A game or addon utilizing NVIDIA DLSS-NR (`nvngx_dlssnr.dll`) over DirectX 12

---

## Installation

1. Navigate to your game folder where `nvngx_dlssnr.dll` is located.
2. Rename the original `nvngx_dlssnr.dll` to:
   ```text
   nvngx_dlssnr_real.dll
   ```
3. Copy the proxy `nvngx_dlssnr.dll` and `nvngx_dlssnr.ini` from the release into that same folder.
4. *(Optional for ReShade users)*: Copy `dlssnr-companion.addon64` into your game folder to get a live configuration overlay under the ReShade Home menu.
5. Launch the game.

---

## Configuration (`nvngx_dlssnr.ini`)

The configuration file is read at startup and automatically hot-reloaded every second when modified:

```ini
[DLSSNR_Proxy]
; Master toggle for the proxy
; 1 = Proxy enabled (applies ResolutionScale)
; 0 = Proxy disabled (100% native passthrough to real DLSS-NR)
EnableProxy = 1

; Internal model resolution scale (0.25 to 2.00)
; 2.00 = 200% Super-Sample (4K SSAA / DLDSR / Photo Mode: 4x sample density)
; 1.50 = 150% Super-Sample (High-fidelity capture)
; 1.00 = 100% Native Passthrough
; 0.85 = 85% Resolution (~28% faster neural pass, zero visual loss sweet spot)
; 0.80 = 80% Resolution (~35% faster)
; 0.75 = 75% Resolution (~40% faster, recommended performance default)
; 0.67 = 67% Resolution (DLSS Quality ratio)
; 0.50 = 50% Resolution (DLSS Performance ratio)
ResolutionScale = 0.75

; [EXPERIMENTAL] Anamorphic / Asymmetric Neural Scaling (0 = Off, 1 = On, Default: 0)
; Scales horizontal and vertical resolution independently.
; When enabled, uses ResolutionScaleX and ResolutionScaleY instead of uniform ResolutionScale.
; 0.65x Horizontal / 0.85x Vertical yields ~45% neural load reduction with rock-solid frame pacing.
EnableAnamorphic = 0
ResolutionScaleX = 0.65
ResolutionScaleY = 0.85

; Resolve algorithm
; 1 = Matched Residual (1:1 Native Anchor + Neural Detail Transfer, Recommended)
; 0 = Direct Neural Reconstruction Bilinear + RCAS
EnlargementMode = 1

; Strength of the neural detail transfer (0.0 to 2.0, default 1.0)
TransferStrength = 1.00

; Neural color/tint transfer strength (0.0 to 1.0, default 1.0)
; 1.00 = Full neural color transfer
; 0.00 = Luminance-only transfer (eliminates neural color shifts while keeping full lighting)
ColorStrength = 1.00

; Contrast-adaptive edge sharpening (0.0 to 1.0, default 0.20)
Sharpness = 0.20

; Depth-Aware Bilateral Silhouette Preservation (0 = Off, 1 = On, Default: 1)
EnableDepthAwareResolve = 1

; [EXPERIMENTAL] Alternating frame neural execution / VRNR (0 = Off, 1 = On, Default: 0)
EnableAlternatingFrames = 0

; Enable in-game hotkeys
EnableHotkeys = 1

[DLSSNR_Settings]
; When UseCustomSettings = 0 (default), passes through whatever NR params
; the caller (OptiScaler, RenoDX, game engine) sets.
; Set to 1 to override caller settings with the proxy values below.
UseCustomSettings = 0

; Style: 0 = Balanced (Default), 1 = Sharp, 2 = Cinematic
Style = 0

; Overall neural denoising and reconstruction intensity (0.00 to 2.00, default 1.00)
Intensity = 1.00

; High-frequency geometry & structure preservation (0.00 to 2.00, default 1.00)
LocalStructureStrength = 1.00

; Local HDR contrast & tonal micro-transitions (0.00 to 2.00, default 1.00)
LocalToneStrength = 1.00

; Skin texture & character structure preservation (-1.00 to 2.00, default -1.00 = Auto)
SkinStructureStrength = -1.00

; Enable automatic internal heuristic masking for fast-moving elements (0 = Off, 1 = On)
UseAutoMask = 0

[Hotkeys]
; Require Ctrl + Alt modifiers held down with the hotkey (1 = yes, 0 = no)
RequireCtrlAlt = 1

; Virtual-Key codes (Decimal):
; Space=32, PageUp=33, PageDown=34, End=35, Home=36, Insert=45, Delete=46
KeyToggleProxy = 32
KeyToggleMode = 35
KeyScaleUp = 33
KeyScaleDown = 34
```

---

## ReShade Companion Addon (`dlssnr-companion.addon64`)

If using ReShade, drop `dlssnr-companion.addon64` into your game directory alongside ReShade.

- **Non-Invasive:** Does not hook graphics draw calls or pipeline passes; operates purely as an overlay tab in the ReShade Home menu.
- **Debounced Sliders:** Features interactive debouncing to ensure rapid slider adjustments never hitch or cause GPU model thrashing.
- **In-Game Hotkey Rebinding:** Rebind shortcut keys and modifier requirements directly in the UI.

---

## In-Game Hotkeys

When `EnableHotkeys = 1`, the default shortcuts are:

- `Ctrl + Alt + Space` — Toggle proxy ON / OFF (switches between scaled proxy and native passthrough).
- `Ctrl + Alt + End` — Toggle EnlargementMode between Matched Residual (`1`) and Bilinear (`0`).
- `Ctrl + Alt + PageUp` — Increase resolution scale by +5%.
- `Ctrl + Alt + PageDown` — Decrease resolution scale by -5%.

---

## Building from Source

Prerequisites:
- Visual Studio 2022 or Build Tools with the Desktop C++ workload.
- Windows 10/11 SDK with `fxc.exe` (DirectX Shader Compiler).

To build:
1. Open the project folder.
2. Run `build.bat` from an x64 Developer Command Prompt or standard prompt.
3. The compiled `nvngx_dlssnr.dll` will be generated in the root folder.

---

## Credits

- [Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) — For pioneering DLSS-NR integration, the matched residual resolve concept, and feature lifecycle handling.
- [OptiScaler](https://github.com/optiscaler/OptiScaler) — For the parent upscaler framework.
- [clshortfuse / RenoDX](https://github.com/clshortfuse/renodx) — For the RenoDX framework and DLSS ReShade addon.
- [AMD](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) — For the Robust Contrast Adaptive Sharpening (RCAS) algorithm.

---

## License

This project is licensed under the [MIT License](LICENSE).
