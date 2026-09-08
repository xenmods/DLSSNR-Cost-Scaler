#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

#define DLSSNR_SHARED_MEM_NAME L"Local\\DLSSNR_Config_Shared_v1"
#define DLSSNR_MAGIC 0x524E5344 // 'DSNR'

#pragma pack(push, 4)
struct DlssnrSharedConfig {
    uint32_t magic;            // DLSSNR_MAGIC
    uint32_t version;          // Monotonically increasing version counter
    uint32_t enableProxy;      // 1 = Active, 0 = Bypassed
    float    resolutionScale;  // 0.25 to 1.00
    uint32_t enlargementMode;  // 1 = Matched Residual, 0 = Bilinear Direct
    float    transferStrength; // 0.0 to 2.0
    float    colorStrength;    // 0.0 to 1.0
    float    sharpness;        // 0.0 to 1.0
    uint32_t enableHotkeys;    // 1 or 0
    uint32_t requireCtrlAlt;   // 1 or 0
    uint32_t keyToggleProxy;
    uint32_t keyToggleMode;
    uint32_t keyScaleUp;
    uint32_t keyScaleDown;
    uint32_t writerSource;     // 1 = Companion UI, 2 = Proxy/Hotkey, 3 = Disk INI

    // Frame Alternation / VRNR (Variable Rate Neural Reconstruction)
    uint32_t enableVrnr;               // 0 = Off (every frame), 1 = On (alternate frames)
    uint32_t enableDepthAware;         // 0 = Off, 1 = On (Depth-Aware Bilateral Silhouette Preservation)

    // Anamorphic / Asymmetric Neural Scaling
    uint32_t enableAnamorphic;         // 0 = Off (uniform scale), 1 = On (asymmetric scale)
    float    scaleX;                   // Horizontal scale (0.25 to 2.00)
    float    scaleY;                   // Vertical scale (0.25 to 2.00)

    // Official DLSS-NR Model Settings
    uint32_t nrStyle;                  // 0 = Balanced, 1 = Sharp, 2 = Cinematic
    float    nrIntensity;              // 0.0 to 2.0 (Default 1.0)
    float    nrLocalStructureStrength; // 0.0 to 2.0 (Default 1.0)
    float    nrLocalToneStrength;      // 0.0 to 2.0 (Default 1.0)
    float    nrSkinStructureStrength;  // -1.0 to 2.0 (-1.0 = Auto)
    uint32_t nrUseAutoMask;            // 0 = Off, 1 = On
    uint32_t useCustomNR;              // 0 = Passthrough caller's NR params, 1 = Override with proxy values

    // Telemetry & Diagnostics
    uint32_t debugNativeW;
    uint32_t debugNativeH;
    uint32_t debugWorkW;
    uint32_t debugWorkH;
    uint32_t debugFormat;
    uint32_t debugHasDepth;
    uint32_t debugDepthW;
    uint32_t debugDepthH;
    uint32_t debugHasMVec;
    uint32_t debugMvW;
    uint32_t debugMvH;
    uint32_t debugActiveSlot;
    uint32_t debugVrnrSkippedThisFrame;// 1 if real_Evaluate was skipped this frame
};
#pragma pack(pop)

