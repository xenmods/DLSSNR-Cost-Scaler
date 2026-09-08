#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "shell32.lib")

#define ImTextureID ImU64
#include "imgui.h"
#include "reshade.hpp"
#include "dlssnr_shared.h"

extern "C" __declspec(dllexport) const char* NAME = "DLSS-NR Cost Scaler";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Live configuration overlay for the DLSSNR-Cost-Scaler proxy.";

struct KeyBinding {
    int vk;
    const char* name;
};

static const KeyBinding kAvailableKeys[] = {
    { VK_SPACE,     "Space" },
    { VK_PRIOR,     "Page Up" },
    { VK_NEXT,      "Page Down" },
    { VK_HOME,      "Home" },
    { VK_END,       "End" },
    { VK_INSERT,    "Insert" },
    { VK_DELETE,    "Delete" },
    { VK_F1,        "F1" },
    { VK_F2,        "F2" },
    { VK_F3,        "F3" },
    { VK_F4,        "F4" },
    { VK_F5,        "F5" },
    { VK_F6,        "F6" },
    { VK_F7,        "F7" },
    { VK_F8,        "F8" },
    { VK_F9,        "F9" },
    { VK_F10,       "F10" },
    { VK_F11,       "F11" },
    { VK_F12,       "F12" },
    { 'A', "A" }, { 'B', "B" }, { 'C', "C" }, { 'D', "D" }, { 'E', "E" },
    { 'F', "F" }, { 'G', "G" }, { 'H', "H" }, { 'I', "I" }, { 'J', "J" },
    { 'K', "K" }, { 'L', "L" }, { 'M', "M" }, { 'N', "N" }, { 'O', "O" },
    { 'P', "P" }, { 'Q', "Q" }, { 'R', "R" }, { 'S', "S" }, { 'T', "T" },
    { 'U', "U" }, { 'V', "V" }, { 'W', "W" }, { 'X', "X" }, { 'Y', "Y" },
    { 'Z', "Z" },
    { VK_NUMPAD0,   "Numpad 0" },
    { VK_NUMPAD1,   "Numpad 1" },
    { VK_NUMPAD2,   "Numpad 2" },
    { VK_NUMPAD3,   "Numpad 3" },
    { VK_NUMPAD4,   "Numpad 4" },
    { VK_NUMPAD5,   "Numpad 5" },
    { VK_NUMPAD6,   "Numpad 6" },
    { VK_NUMPAD7,   "Numpad 7" },
    { VK_NUMPAD8,   "Numpad 8" },
    { VK_NUMPAD9,   "Numpad 9" },
    { VK_MULTIPLY,  "Numpad *" },
    { VK_ADD,       "Numpad +" }
};

static int FindKeyIndex(int vk) {
    for (int i = 0; i < (int)(sizeof(kAvailableKeys) / sizeof(kAvailableKeys[0])); ++i) {
        if (kAvailableKeys[i].vk == vk) return i;
    }
    return 0;
}

// Runtime Configuration State
static bool  s_enableProxy      = true;
static float s_resolutionScale  = 0.75f;
static bool  s_enableAnamorphic = false;
static float s_scaleX           = 0.65f;
static float s_scaleY           = 0.85f;
static int   s_enlargementMode  = 1; // 1 = Matched Residual, 0 = Bilinear
static float s_transferStrength = 1.00f;
static float s_colorStrength    = 1.00f;
static float s_sharpness        = 0.20f;
static bool  s_enableHotkeys    = true;
static bool  s_requireCtrlAlt   = true;
static int   s_keyToggleProxy   = VK_SPACE;
static int   s_keyToggleMode    = VK_END;
static int   s_keyScaleUp       = VK_PRIOR;
static int   s_keyScaleDown     = VK_NEXT;

// Frame Alternation / VRNR & Official DLSS-NR Model Settings
static bool  s_enableVrnr              = false;
static bool  s_enableDepthAware        = false;
static int   s_nrStyle                 = 0;     // 0 = Balanced, 1 = Sharp, 2 = Cinematic
static float s_nrIntensity             = 1.00f; // 0.0 to 2.0
static float s_nrLocalStructureStrength = 1.00f; // 0.0 to 2.0
static float s_nrLocalToneStrength      = 1.00f; // 0.0 to 2.0
static float s_nrSkinStructureStrength  = -1.00f;// -1.0 to 2.0 (-1.0 = Auto)
static bool  s_nrUseAutoMask           = false;
static bool  s_useCustomNR             = false;  // false = passthrough caller's NR params

// Debounce & Notification State
static bool      s_dirty          = false;
static bool      s_scaleDragging  = false;
static ULONGLONG s_lastChangeTick = 0;
static constexpr ULONGLONG DEBOUNCE_DELAY_MS = 250;
static char      s_statusMsg[128] = "Synced with nvngx_dlssnr.ini";
static ULONGLONG s_statusMsgTick  = 0;
static FILETIME  s_lastDiskWriteTime = { 0, 0 };

// Inter-process Shared Memory State
static HANDLE              g_hSharedMem = nullptr;
static DlssnrSharedConfig* g_sharedConfig = nullptr;
static uint32_t            s_lastCompanionVersion = 0;

static std::wstring GetIniFilePath() {
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    return std::wstring(exePath) + L"nvngx_dlssnr.ini";
}

static void InitSharedMemory() {
    if (g_sharedConfig) return;
    g_hSharedMem = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(DlssnrSharedConfig), DLSSNR_SHARED_MEM_NAME);
    if (g_hSharedMem) {
        g_sharedConfig = (DlssnrSharedConfig*)MapViewOfFile(g_hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DlssnrSharedConfig));
        if (g_sharedConfig) {
            if (GetLastError() != ERROR_ALREADY_EXISTS) {
                ZeroMemory(g_sharedConfig, sizeof(DlssnrSharedConfig));
                g_sharedConfig->magic = DLSSNR_MAGIC;
                g_sharedConfig->version = 1;
                g_sharedConfig->enableProxy = s_enableProxy ? 1 : 0;
                g_sharedConfig->resolutionScale = s_resolutionScale;
                g_sharedConfig->enlargementMode = s_enlargementMode;
                g_sharedConfig->transferStrength = s_transferStrength;
                g_sharedConfig->colorStrength = s_colorStrength;
                g_sharedConfig->sharpness = s_sharpness;
                g_sharedConfig->enableHotkeys = s_enableHotkeys ? 1 : 0;
                g_sharedConfig->requireCtrlAlt = s_requireCtrlAlt ? 1 : 0;
                g_sharedConfig->keyToggleProxy = s_keyToggleProxy;
                g_sharedConfig->keyToggleMode = s_keyToggleMode;
                g_sharedConfig->keyScaleUp = s_keyScaleUp;
                g_sharedConfig->keyScaleDown = s_keyScaleDown;

                g_sharedConfig->enableVrnr = s_enableVrnr ? 1 : 0;
                g_sharedConfig->enableDepthAware = s_enableDepthAware ? 1 : 0;
                g_sharedConfig->enableAnamorphic = s_enableAnamorphic ? 1 : 0;
                g_sharedConfig->scaleX = s_scaleX;
                g_sharedConfig->scaleY = s_scaleY;
                g_sharedConfig->nrStyle = s_nrStyle;
                g_sharedConfig->nrIntensity = s_nrIntensity;
                g_sharedConfig->nrLocalStructureStrength = s_nrLocalStructureStrength;
                g_sharedConfig->nrLocalToneStrength = s_nrLocalToneStrength;
                g_sharedConfig->nrSkinStructureStrength = s_nrSkinStructureStrength;
                g_sharedConfig->nrUseAutoMask = s_nrUseAutoMask ? 1 : 0;
                g_sharedConfig->useCustomNR = s_useCustomNR ? 1 : 0;

                g_sharedConfig->writerSource = 1;
                s_lastCompanionVersion = 1;
            } else {
                s_lastCompanionVersion = g_sharedConfig->version;
            }
        }
    }
}

static void ShutdownSharedMemory() {
    if (g_sharedConfig) {
        UnmapViewOfFile(g_sharedConfig);
        g_sharedConfig = nullptr;
    }
    if (g_hSharedMem) {
        CloseHandle(g_hSharedMem);
        g_hSharedMem = nullptr;
    }
}

static void PushToSharedMemory(uint32_t source) {
    if (!g_sharedConfig || g_sharedConfig->magic != DLSSNR_MAGIC) return;
    g_sharedConfig->enableProxy = s_enableProxy ? 1 : 0;
    g_sharedConfig->resolutionScale = s_resolutionScale;
    g_sharedConfig->enlargementMode = s_enlargementMode;
    g_sharedConfig->transferStrength = s_transferStrength;
    g_sharedConfig->colorStrength = s_colorStrength;
    g_sharedConfig->sharpness = s_sharpness;
    g_sharedConfig->enableHotkeys = s_enableHotkeys ? 1 : 0;
    g_sharedConfig->requireCtrlAlt = s_requireCtrlAlt ? 1 : 0;
    g_sharedConfig->keyToggleProxy = s_keyToggleProxy;
    g_sharedConfig->keyToggleMode = s_keyToggleMode;
    g_sharedConfig->keyScaleUp = s_keyScaleUp;
    g_sharedConfig->keyScaleDown = s_keyScaleDown;

    g_sharedConfig->enableVrnr = s_enableVrnr ? 1 : 0;
    g_sharedConfig->enableDepthAware = s_enableDepthAware ? 1 : 0;
    g_sharedConfig->enableAnamorphic = s_enableAnamorphic ? 1 : 0;
    g_sharedConfig->scaleX = s_scaleX;
    g_sharedConfig->scaleY = s_scaleY;
    g_sharedConfig->nrStyle = s_nrStyle;
    g_sharedConfig->nrIntensity = s_nrIntensity;
    g_sharedConfig->nrLocalStructureStrength = s_nrLocalStructureStrength;
    g_sharedConfig->nrLocalToneStrength = s_nrLocalToneStrength;
    g_sharedConfig->nrSkinStructureStrength = s_nrSkinStructureStrength;
    g_sharedConfig->nrUseAutoMask = s_nrUseAutoMask ? 1 : 0;
    g_sharedConfig->useCustomNR = s_useCustomNR ? 1 : 0;

    g_sharedConfig->writerSource = source;
    g_sharedConfig->version++;
    s_lastCompanionVersion = g_sharedConfig->version;
}

static void PullFromSharedMemory() {
    if (!g_sharedConfig || g_sharedConfig->magic != DLSSNR_MAGIC) return;
    if (g_sharedConfig->version == s_lastCompanionVersion) return;
    // If updated by Proxy hotkey or disk INI reload (writerSource != 1), reflect in UI
    if (g_sharedConfig->writerSource != 1) {
        s_enableProxy = (g_sharedConfig->enableProxy != 0);
        s_resolutionScale = g_sharedConfig->resolutionScale;
        s_enlargementMode = g_sharedConfig->enlargementMode;
        s_transferStrength = g_sharedConfig->transferStrength;
        s_colorStrength = g_sharedConfig->colorStrength;
        s_sharpness = g_sharedConfig->sharpness;
        s_enableHotkeys = (g_sharedConfig->enableHotkeys != 0);
        s_requireCtrlAlt = (g_sharedConfig->requireCtrlAlt != 0);
        s_keyToggleProxy = g_sharedConfig->keyToggleProxy;
        s_keyToggleMode = g_sharedConfig->keyToggleMode;
        s_keyScaleUp = g_sharedConfig->keyScaleUp;
        s_keyScaleDown = g_sharedConfig->keyScaleDown;

        s_enableVrnr = (g_sharedConfig->enableVrnr != 0);
        s_enableDepthAware = (g_sharedConfig->enableDepthAware != 0);
        s_enableAnamorphic = (g_sharedConfig->enableAnamorphic != 0);
        s_scaleX = g_sharedConfig->scaleX;
        s_scaleY = g_sharedConfig->scaleY;
        s_nrStyle = g_sharedConfig->nrStyle;
        s_nrIntensity = g_sharedConfig->nrIntensity;
        s_nrLocalStructureStrength = g_sharedConfig->nrLocalStructureStrength;
        s_nrLocalToneStrength = g_sharedConfig->nrLocalToneStrength;
        s_nrSkinStructureStrength = g_sharedConfig->nrSkinStructureStrength;
        s_nrUseAutoMask = (g_sharedConfig->nrUseAutoMask != 0);
        s_useCustomNR = (g_sharedConfig->useCustomNR != 0);

        s_lastCompanionVersion = g_sharedConfig->version;

        std::wstring iniPath = GetIniFilePath();
        WIN32_FILE_ATTRIBUTE_DATA fileInfo;
        if (GetFileAttributesExW(iniPath.c_str(), GetFileExInfoStandard, &fileInfo)) {
            s_lastDiskWriteTime = fileInfo.ftLastWriteTime;
        }
    }
}

static void LoadIniSettings() {
    std::wstring iniPath = GetIniFilePath();

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (GetFileAttributesExW(iniPath.c_str(), GetFileExInfoStandard, &fileInfo)) {
        s_lastDiskWriteTime = fileInfo.ftLastWriteTime;
    }

    s_enableProxy = (GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnableProxy", 1, iniPath.c_str()) != 0);

    wchar_t scaleBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"ResolutionScale", L"0.75", scaleBuf, 64, iniPath.c_str());
    float val = (float)_wtof(scaleBuf);
    if (val < 0.25f) val = 0.25f;
    if (val > 2.00f) val = 2.00f;
    s_resolutionScale = val;

    s_enableAnamorphic = (GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnableAnamorphic", 0, iniPath.c_str()) != 0);

    wchar_t scaleXBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"ResolutionScaleX", L"0.65", scaleXBuf, 64, iniPath.c_str());
    float sxVal = (float)_wtof(scaleXBuf);
    if (sxVal < 0.25f) sxVal = 0.25f;
    if (sxVal > 2.00f) sxVal = 2.00f;
    s_scaleX = sxVal;

    wchar_t scaleYBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"ResolutionScaleY", L"0.85", scaleYBuf, 64, iniPath.c_str());
    float syVal = (float)_wtof(scaleYBuf);
    if (syVal < 0.25f) syVal = 0.25f;
    if (syVal > 2.00f) syVal = 2.00f;
    s_scaleY = syVal;

    s_enlargementMode = GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnlargementMode", 1, iniPath.c_str());
    if (s_enlargementMode != 0 && s_enlargementMode != 1) s_enlargementMode = 1;

    wchar_t transferBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"TransferStrength", L"1.00", transferBuf, 64, iniPath.c_str());
    float tVal = (float)_wtof(transferBuf);
    if (tVal < 0.0f) tVal = 0.0f;
    if (tVal > 2.0f) tVal = 2.0f;
    s_transferStrength = tVal;

    wchar_t colorBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"ColorStrength", L"1.00", colorBuf, 64, iniPath.c_str());
    float cVal = (float)_wtof(colorBuf);
    if (cVal < 0.0f) cVal = 0.0f;
    if (cVal > 1.0f) cVal = 1.0f;
    s_colorStrength = cVal;

    wchar_t sharpBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"Sharpness", L"0.20", sharpBuf, 64, iniPath.c_str());
    float sVal = (float)_wtof(sharpBuf);
    if (sVal < 0.0f) sVal = 0.0f;
    if (sVal > 1.0f) sVal = 1.0f;
    s_sharpness = sVal;

    s_enableVrnr = (GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnableAlternatingFrames", 0, iniPath.c_str()) != 0);
    s_enableDepthAware = (GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnableDepthAwareResolve", 0, iniPath.c_str()) != 0);

    s_nrStyle = GetPrivateProfileIntW(L"DLSSNR_Settings", L"Style", 0, iniPath.c_str());
    if (s_nrStyle < 0 || s_nrStyle > 2) s_nrStyle = 0;

    wchar_t nrBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Settings", L"Intensity", L"1.00", nrBuf, 64, iniPath.c_str());
    s_nrIntensity = (float)_wtof(nrBuf);
    if (s_nrIntensity < 0.0f) s_nrIntensity = 0.0f;
    if (s_nrIntensity > 2.0f) s_nrIntensity = 2.0f;

    GetPrivateProfileStringW(L"DLSSNR_Settings", L"LocalStructureStrength", L"1.00", nrBuf, 64, iniPath.c_str());
    s_nrLocalStructureStrength = (float)_wtof(nrBuf);
    if (s_nrLocalStructureStrength < 0.0f) s_nrLocalStructureStrength = 0.0f;
    if (s_nrLocalStructureStrength > 2.0f) s_nrLocalStructureStrength = 2.0f;

    GetPrivateProfileStringW(L"DLSSNR_Settings", L"LocalToneStrength", L"1.00", nrBuf, 64, iniPath.c_str());
    s_nrLocalToneStrength = (float)_wtof(nrBuf);
    if (s_nrLocalToneStrength < 0.0f) s_nrLocalToneStrength = 0.0f;
    if (s_nrLocalToneStrength > 2.0f) s_nrLocalToneStrength = 2.0f;

    GetPrivateProfileStringW(L"DLSSNR_Settings", L"SkinStructureStrength", L"-1.00", nrBuf, 64, iniPath.c_str());
    s_nrSkinStructureStrength = (float)_wtof(nrBuf);
    if (s_nrSkinStructureStrength < -1.0f) s_nrSkinStructureStrength = -1.0f;
    if (s_nrSkinStructureStrength > 2.0f) s_nrSkinStructureStrength = 2.0f;

    s_nrUseAutoMask = (GetPrivateProfileIntW(L"DLSSNR_Settings", L"UseAutoMask", 0, iniPath.c_str()) != 0);
    s_useCustomNR = (GetPrivateProfileIntW(L"DLSSNR_Settings", L"UseCustomSettings", 0, iniPath.c_str()) != 0);

    s_enableHotkeys  = (GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnableHotkeys", 1, iniPath.c_str()) != 0);
    s_requireCtrlAlt = (GetPrivateProfileIntW(L"Hotkeys", L"RequireCtrlAlt", 1, iniPath.c_str()) != 0);
    s_keyToggleProxy = GetPrivateProfileIntW(L"Hotkeys", L"KeyToggleProxy", VK_SPACE, iniPath.c_str());
    s_keyToggleMode  = GetPrivateProfileIntW(L"Hotkeys", L"KeyToggleMode",  VK_END,   iniPath.c_str());
    s_keyScaleUp     = GetPrivateProfileIntW(L"Hotkeys", L"KeyScaleUp",     VK_PRIOR, iniPath.c_str());
    s_keyScaleDown   = GetPrivateProfileIntW(L"Hotkeys", L"KeyScaleDown",   VK_NEXT,  iniPath.c_str());
}

static void SaveIniSettings() {
    std::wstring iniPath = GetIniFilePath();

    wchar_t buf[64];

    swprintf_s(buf, L"%d", s_enableProxy ? 1 : 0);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"EnableProxy", buf, iniPath.c_str());

    swprintf_s(buf, L"%.2f", s_resolutionScale);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"ResolutionScale", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_enableAnamorphic ? 1 : 0);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"EnableAnamorphic", buf, iniPath.c_str());

    swprintf_s(buf, L"%.2f", s_scaleX);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"ResolutionScaleX", buf, iniPath.c_str());

    swprintf_s(buf, L"%.2f", s_scaleY);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"ResolutionScaleY", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_enlargementMode);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"EnlargementMode", buf, iniPath.c_str());

    swprintf_s(buf, L"%.2f", s_transferStrength);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"TransferStrength", buf, iniPath.c_str());

    swprintf_s(buf, L"%.2f", s_colorStrength);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"ColorStrength", buf, iniPath.c_str());

    swprintf_s(buf, L"%.2f", s_sharpness);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"Sharpness", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_enableVrnr ? 1 : 0);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"EnableAlternatingFrames", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_enableDepthAware ? 1 : 0);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"EnableDepthAwareResolve", buf, iniPath.c_str());

    // Official DLSS-NR model settings
    swprintf_s(buf, L"%d", s_nrStyle);
    WritePrivateProfileStringW(L"DLSSNR_Settings", L"Style", buf, iniPath.c_str());

    swprintf_s(buf, L"%.2f", s_nrIntensity);
    WritePrivateProfileStringW(L"DLSSNR_Settings", L"Intensity", buf, iniPath.c_str());

    swprintf_s(buf, L"%.2f", s_nrLocalStructureStrength);
    WritePrivateProfileStringW(L"DLSSNR_Settings", L"LocalStructureStrength", buf, iniPath.c_str());

    swprintf_s(buf, L"%.2f", s_nrLocalToneStrength);
    WritePrivateProfileStringW(L"DLSSNR_Settings", L"LocalToneStrength", buf, iniPath.c_str());

    swprintf_s(buf, L"%.2f", s_nrSkinStructureStrength);
    WritePrivateProfileStringW(L"DLSSNR_Settings", L"SkinStructureStrength", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_nrUseAutoMask ? 1 : 0);
    WritePrivateProfileStringW(L"DLSSNR_Settings", L"UseAutoMask", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_useCustomNR ? 1 : 0);
    WritePrivateProfileStringW(L"DLSSNR_Settings", L"UseCustomSettings", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_enableHotkeys ? 1 : 0);
    WritePrivateProfileStringW(L"DLSSNR_Proxy", L"EnableHotkeys", buf, iniPath.c_str());

    // Hotkey bindings section
    swprintf_s(buf, L"%d", s_requireCtrlAlt ? 1 : 0);
    WritePrivateProfileStringW(L"Hotkeys", L"RequireCtrlAlt", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_keyToggleProxy);
    WritePrivateProfileStringW(L"Hotkeys", L"KeyToggleProxy", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_keyToggleMode);
    WritePrivateProfileStringW(L"Hotkeys", L"KeyToggleMode", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_keyScaleUp);
    WritePrivateProfileStringW(L"Hotkeys", L"KeyScaleUp", buf, iniPath.c_str());

    swprintf_s(buf, L"%d", s_keyScaleDown);
    WritePrivateProfileStringW(L"Hotkeys", L"KeyScaleDown", buf, iniPath.c_str());

    // Flush Windows profile cache to disk immediately
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, iniPath.c_str());

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (GetFileAttributesExW(iniPath.c_str(), GetFileExInfoStandard, &fileInfo)) {
        s_lastDiskWriteTime = fileInfo.ftLastWriteTime;
    }
}

static void PollDiskChanges() {
    PullFromSharedMemory();

    if (s_dirty) return;

    static ULONGLONG s_lastCheck = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastCheck < 100) return;
    s_lastCheck = now;

    std::wstring iniPath = GetIniFilePath();
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (GetFileAttributesExW(iniPath.c_str(), GetFileExInfoStandard, &fileInfo)) {
        if (CompareFileTime(&fileInfo.ftLastWriteTime, &s_lastDiskWriteTime) != 0) {
            LoadIniSettings();
            PushToSharedMemory(3); // Reflect disk update into shared memory
        }
    }
}

static void DrawKeySelector(const char* label, int* currentVk) {
    int currentIdx = FindKeyIndex(*currentVk);
    int totalKeys = (int)(sizeof(kAvailableKeys) / sizeof(kAvailableKeys[0]));

    if (ImGui::BeginCombo(label, kAvailableKeys[currentIdx].name)) {
        for (int i = 0; i < totalKeys; ++i) {
            bool isSelected = (i == currentIdx);
            if (ImGui::Selectable(kAvailableKeys[i].name, isSelected)) {
                *currentVk = kAvailableKeys[i].vk;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

static const char* GetDxgiFormatString(uint32_t format) {
    switch (format) {
    case 10: return "R16G16B16A16_FLOAT (10)";
    case 24: return "R10G10B10A2_UNORM (24)";
    case 26: return "R11G11B10_FLOAT (26)";
    case 28: return "R8G8B8A8_UNORM (28)";
    case 29: return "R8G8B8A8_UNORM_SRGB (29)";
    case 87: return "B8G8R8A8_UNORM (87)";
    case 91: return "B8G8R8A8_UNORM_SRGB (91)";
    default: {
        static char buf[32];
        snprintf(buf, sizeof(buf), "DXGI_FORMAT_%u", format);
        return buf;
    }
    }
}

static void CopyDebugInfoToClipboard() {
    if (!g_sharedConfig || g_sharedConfig->magic != DLSSNR_MAGIC) return;
    char text[1024];

    const char* styleStr = "Balanced";
    if (g_sharedConfig->nrStyle == 1) styleStr = "Sharp";
    else if (g_sharedConfig->nrStyle == 2) styleStr = "Cinematic";

    char skinBuf[32];
    if (g_sharedConfig->nrSkinStructureStrength < -0.01f) {
        snprintf(skinBuf, sizeof(skinBuf), "Auto (%.2f)", g_sharedConfig->nrSkinStructureStrength);
    } else {
        snprintf(skinBuf, sizeof(skinBuf), "%.2f", g_sharedConfig->nrSkinStructureStrength);
    }

    char scaleInfo[64];
    if (g_sharedConfig->enableAnamorphic != 0) {
        snprintf(scaleInfo, sizeof(scaleInfo), "Anamorphic (%.2fx X, %.2fy Y)", g_sharedConfig->scaleX, g_sharedConfig->scaleY);
    } else {
        snprintf(scaleInfo, sizeof(scaleInfo), "%.2f", g_sharedConfig->resolutionScale);
    }

    snprintf(text, sizeof(text),
        "=== DLSS-NR Cost Scaler Diagnostics ===\r\n"
        "Proxy Status: %s\r\n"
        "Resolution Scale: %s (Work: %ux%u -> Native: %ux%u)\r\n"
        "Resolve Mode: %s\r\n"
        "Alternating Frames: %s\r\n"
        "Transfer Strength: %.2f\r\n"
        "Color Strength: %.2f\r\n"
        "Sharpness: %.2f\r\n"
        "Model Settings: %s\r\n"
        "  - Style: %s (%u)\r\n"
        "  - Intensity: %.2f\r\n"
        "  - Local Structure: %.2f\r\n"
        "  - Local Tone: %.2f\r\n"
        "  - Skin Structure: %s\r\n"
        "  - Auto Mask: %s\r\n"
        "DXGI Format: %s\r\n"
        "G-Buffers:\r\n"
        "  - Depth: %s (%ux%u)\r\n"
        "  - Motion Vectors: %s (%ux%u)\r\n"
        "Active Slot: Pass %u\r\n"
        "Shared Mem Version: %u (Source: %u)\r\n"
        "========================================",
        (g_sharedConfig->enableProxy != 0) ? "Active" : "Bypassed",
        scaleInfo,
        g_sharedConfig->debugWorkW, g_sharedConfig->debugWorkH,
        g_sharedConfig->debugNativeW, g_sharedConfig->debugNativeH,
        (g_sharedConfig->enlargementMode == 1) ? "Matched Residual" : "Direct Upscale",
        (g_sharedConfig->enableVrnr != 0) ? (g_sharedConfig->debugVrnrSkippedThisFrame ? "Active (Cached Frame)" : "Active (Evaluated Frame)") : "Disabled",
        g_sharedConfig->transferStrength,
        g_sharedConfig->colorStrength,
        g_sharedConfig->sharpness,
        (g_sharedConfig->useCustomNR != 0) ? "Custom Override" : "Caller Passthrough (Default)",
        styleStr, g_sharedConfig->nrStyle,
        g_sharedConfig->nrIntensity,
        g_sharedConfig->nrLocalStructureStrength,
        g_sharedConfig->nrLocalToneStrength,
        skinBuf,
        (g_sharedConfig->nrUseAutoMask != 0) ? "Enabled" : "Disabled",
        GetDxgiFormatString(g_sharedConfig->debugFormat),
        g_sharedConfig->debugHasDepth ? "Present" : "None",
        g_sharedConfig->debugDepthW, g_sharedConfig->debugDepthH,
        g_sharedConfig->debugHasMVec ? "Present" : "None",
        g_sharedConfig->debugMvW, g_sharedConfig->debugMvH,
        g_sharedConfig->debugActiveSlot,
        g_sharedConfig->version,
        g_sharedConfig->writerSource
    );

    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        size_t len = strlen(text) + 1;
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
        if (hMem) {
            memcpy(GlobalLock(hMem), text, len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    }
}

static void DrawOverlay(reshade::api::effect_runtime* /*runtime*/) {
    PollDiskChanges();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));

    ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "%s", "DLSS-NR Cost Scaler");
    ImGui::SameLine();
    if (s_enableProxy) {
        ImGui::TextColored(ImVec4(0.20f, 0.90f, 0.30f, 1.00f), "%s", "[ACTIVE]");
    } else {
        ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.70f, 1.00f), "%s", "[BYPASSED]");
    }

    ImGui::Separator();

    if (ImGui::Checkbox("Enable Proxy", &s_enableProxy)) {
        s_dirty = true;
        s_lastChangeTick = 0;
        PushToSharedMemory(1);
    }

    if (!s_enableProxy) {
        ImGui::TextDisabled("%s", "Proxy is disabled. DLSS-NR runs at native resolution with zero scaling.");
    } else {
        if (!s_enableAnamorphic) {
            if (ImGui::SliderFloat("Resolution Scale", &s_resolutionScale, 0.25f, 2.00f, "%.2f")) {
                s_scaleDragging = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                s_scaleDragging = false;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }

            if (s_resolutionScale < 0.995f) {
                float pixelPct = (1.0f - (s_resolutionScale * s_resolutionScale)) * 100.0f;
                ImGui::SameLine();
                ImGui::TextDisabled("(%.0f%% fewer pixels)", pixelPct);
            } else if (s_resolutionScale > 1.005f) {
                float pixelPct = ((s_resolutionScale * s_resolutionScale) - 1.0f) * 100.0f;
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "(+%.0f%% Super-Sample / Photo)", pixelPct);
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("(1:1 Native Passthrough)");
            }

            ImGui::TextUnformatted("Quick Presets:");
            ImGui::SameLine();
            if (ImGui::SmallButton("75% (Performance)")) {
                s_resolutionScale = 0.75f;
                s_scaleDragging = false;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("100% (Native)")) {
                s_resolutionScale = 1.00f;
                s_scaleDragging = false;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("150% (Photo Mode)")) {
                s_resolutionScale = 1.50f;
                s_scaleDragging = false;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("200% (4K SSAA)")) {
                s_resolutionScale = 2.00f;
                s_scaleDragging = false;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }
        } else {
            if (ImGui::SliderFloat("Scale X (Horizontal)", &s_scaleX, 0.25f, 2.00f, "%.2f")) {
                s_scaleDragging = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                s_scaleDragging = false;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }

            if (ImGui::SliderFloat("Scale Y (Vertical)", &s_scaleY, 0.25f, 2.00f, "%.2f")) {
                s_scaleDragging = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                s_scaleDragging = false;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }

            float pixelPct = (1.0f - (s_scaleX * s_scaleY)) * 100.0f;
            if (pixelPct > 0.5f) {
                ImGui::TextDisabled("Effective Load: %.0f%% fewer neural pixels", pixelPct);
            } else if (pixelPct < -0.5f) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Effective Load: +%.0f%% super-sampling", -pixelPct);
            } else {
                ImGui::TextDisabled("Effective Load: 1:1 Native");
            }

            ImGui::TextUnformatted("Anamorphic Presets:");
            ImGui::SameLine();
            if (ImGui::SmallButton("Widescreen (0.65x / 0.85x)")) {
                s_scaleX = 0.65f;
                s_scaleY = 0.85f;
                s_scaleDragging = false;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Ultra-Perf (0.50x / 0.75x)")) {
                s_scaleX = 0.50f;
                s_scaleY = 0.75f;
                s_scaleDragging = false;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Quality (0.80x / 0.90x)")) {
                s_scaleX = 0.80f;
                s_scaleY = 0.90f;
                s_scaleDragging = false;
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }
        }

        if (ImGui::Checkbox("Anamorphic / Asymmetric Scaling", &s_enableAnamorphic)) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Scales horizontal and vertical resolution independently.\n"
                              "Human peripheral vision is less sensitive to horizontal high-frequency details\n"
                              "on widescreen displays. For example, 0.65x Horizontal and 0.85x Vertical yields ~45%% neural cost reduction\n"
                              "with near-native vertical clarity and perfectly consistent frame pacing!");
        }
        if (s_enableAnamorphic) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "[%.2fx X, %.2fy Y]", s_scaleX, s_scaleY);
        }

        const char* modeItems[] = {
            "Direct Neural Upscale (0) - Full Neural Magnify",
            "Matched Residual (1) - 1:1 Native Resolution Anchor (Recommended)"
        };
        int currentModeIdx = (s_enlargementMode == 1) ? 1 : 0;
        if (ImGui::Combo("Resolve Mode", &currentModeIdx, modeItems, 2)) {
            s_enlargementMode = (currentModeIdx == 1) ? 1 : 0;
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }

        if (ImGui::SliderFloat("RCAS Sharpness", &s_sharpness, 0.00f, 1.00f, "%.2f")) {
            s_dirty = true;
            s_lastChangeTick = GetTickCount64();
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }

        if (ImGui::SliderFloat("Transfer Strength", &s_transferStrength, 0.00f, 2.00f, "%.2f")) {
            s_dirty = true;
            s_lastChangeTick = GetTickCount64();
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }

        if (ImGui::SliderFloat("Color Strength", &s_colorStrength, 0.00f, 1.00f, "%.2f")) {
            s_dirty = true;
            s_lastChangeTick = GetTickCount64();
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }

        if (ImGui::Checkbox("Depth-Aware Bilateral Resolve", &s_enableDepthAware)) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Uses native depth buffer to preserve razor-sharp object silhouettes and geometric boundaries,\n"
                              "preventing low-resolution neural radiance from bleeding across foreground edges.");
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "[EXPERIMENTAL]");
        ImGui::SameLine();
        if (ImGui::Checkbox("Alternating Frames (VRNR 2x)", &s_enableVrnr)) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Variable Rate Neural Reconstruction: Evaluates DLSS-NR every 2nd frame.\n"
                              "NOTE: Boosts average FPS counter, but causes 30Hz frame pacing micro-stutter\n"
                              "and motion flicker in native ray-traced games. Recommended OFF for smooth motion.");
        }
        if (s_enableVrnr) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "[Active]");
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                "  ! WARNING: Alternating frames creates uneven frame pacing (sawtooth delivery).\n"
                "    Camera motion will feel choppy despite a higher FPS counter.\n"
                "    Keep OFF for buttery smooth, consistent frame delivery.");
        }
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Neural Model Tuning (Official DLSS-NR)")) {
        if (ImGui::Checkbox("Override Caller NR Settings", &s_useCustomNR)) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When unchecked (Default), passes through whatever DLSS-NR settings\n"
                              "the caller (OptiScaler, RenoDX, game engine) configured on the feature.\n"
                              "When checked, the proxy forces the values configured below onto the neural model.");
        }

        if (!s_useCustomNR) {
            ImGui::TextDisabled("Status: Caller Settings Active (Passthrough)");
            ImGui::BeginDisabled();
        } else {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Status: Custom Proxy Overrides Active");
        }

        const char* styleItems[] = {
            "Balanced (0) - Default",
            "Sharp (1) - Crisp Edges & Detail",
            "Cinematic (2) - Film Grain & Smooth Roll-off"
        };
        int currentStyle = s_nrStyle;
        if (ImGui::Combo("Reconstruction Style", &currentStyle, styleItems, 3)) {
            s_nrStyle = currentStyle;
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }

        if (ImGui::SliderFloat("Model Intensity", &s_nrIntensity, 0.00f, 2.00f, "%.2f")) {
            s_dirty = true;
            s_lastChangeTick = GetTickCount64();
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }

        if (ImGui::SliderFloat("Local Structure Strength", &s_nrLocalStructureStrength, 0.00f, 2.00f, "%.2f")) {
            s_dirty = true;
            s_lastChangeTick = GetTickCount64();
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }

        if (ImGui::SliderFloat("Local Tone Strength", &s_nrLocalToneStrength, 0.00f, 2.00f, "%.2f")) {
            s_dirty = true;
            s_lastChangeTick = GetTickCount64();
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }

        char skinFormat[32];
        if (s_nrSkinStructureStrength < -0.01f) {
            snprintf(skinFormat, sizeof(skinFormat), "Auto (%.2f)", s_nrSkinStructureStrength);
        } else {
            snprintf(skinFormat, sizeof(skinFormat), "%.2f", s_nrSkinStructureStrength);
        }
        if (ImGui::SliderFloat("Skin Structure Strength", &s_nrSkinStructureStrength, -1.00f, 2.00f, skinFormat)) {
            s_dirty = true;
            s_lastChangeTick = GetTickCount64();
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }

        if (ImGui::Checkbox("Use Auto Mask", &s_nrUseAutoMask)) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Enables DLSS-NR automatic masking for ghosting reduction on dynamic elements.");
        }

        if (!s_useCustomNR) {
            ImGui::EndDisabled();
        }
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Hotkey Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enable In-Game Hotkeys", &s_enableHotkeys)) {
            s_dirty = true;
            s_lastChangeTick = 0;
            PushToSharedMemory(1);
        }

        if (s_enableHotkeys) {
            if (ImGui::Checkbox("Require Ctrl + Alt Modifiers", &s_requireCtrlAlt)) {
                s_dirty = true;
                s_lastChangeTick = 0;
                PushToSharedMemory(1);
            }

            DrawKeySelector("Toggle Proxy Key", &s_keyToggleProxy);
            DrawKeySelector("Toggle Resolve Mode Key", &s_keyToggleMode);
            DrawKeySelector("Scale Up (+5%) Key", &s_keyScaleUp);
            DrawKeySelector("Scale Down (-5%) Key", &s_keyScaleDown);

            const char* prefix = s_requireCtrlAlt ? "Ctrl + Alt + " : "";
            int kProxyIdx = FindKeyIndex(s_keyToggleProxy);
            int kUpIdx    = FindKeyIndex(s_keyScaleUp);
            int kDownIdx  = FindKeyIndex(s_keyScaleDown);
            ImGui::TextDisabled("Shortcuts: %s%s (Toggle) | %s%s / %s%s (Scale +/-)",
                prefix, kAvailableKeys[kProxyIdx].name,
                prefix, kAvailableKeys[kUpIdx].name,
                prefix, kAvailableKeys[kDownIdx].name);
        }
    }

    ImGui::Separator();

    ULONGLONG now = GetTickCount64();
    if (s_scaleDragging) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", "Release slider to apply resolution...");
    } else if (s_dirty) {
        if (s_lastChangeTick == 0 || (now - s_lastChangeTick >= DEBOUNCE_DELAY_MS)) {
            PushToSharedMemory(1);
            SaveIniSettings();
            s_dirty = false;
            snprintf(s_statusMsg, sizeof(s_statusMsg), "Saved to nvngx_dlssnr.ini (Scale=%.2f, Sharp=%.2f)", s_resolutionScale, s_sharpness);
            s_statusMsgTick = now;
        }
    }

    if (!s_dirty) {
        if (now - s_statusMsgTick < 4000) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Saved: %s", s_statusMsg);
        } else {
            ImGui::TextDisabled("%s", "All settings saved and active in nvngx_dlssnr.ini");
        }
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Debug & Diagnostics")) {
        if (g_sharedConfig && g_sharedConfig->magic == DLSSNR_MAGIC && g_sharedConfig->debugNativeW > 0) {
            ImGui::Text("Native Resolution:  %ux%u", g_sharedConfig->debugNativeW, g_sharedConfig->debugNativeH);
            ImGui::Text("Working Resolution: %ux%u (%.2fx)", g_sharedConfig->debugWorkW, g_sharedConfig->debugWorkH, g_sharedConfig->resolutionScale);
            ImGui::Text("Color Format:       %s", GetDxgiFormatString(g_sharedConfig->debugFormat));
            ImGui::Text("Active Slot:        Pass %u", g_sharedConfig->debugActiveSlot);

            if (g_sharedConfig->enableVrnr) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Alternating Frames: [EXPERIMENTAL] Active (%s - Stutter Expected)",
                    g_sharedConfig->debugVrnrSkippedThisFrame ? "Frame Cached" : "Frame Evaluated");
            } else {
                ImGui::TextDisabled("Alternating Frames: Disabled (Smooth Frame Pacing)");
            }

            if (g_sharedConfig->debugHasDepth) {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "Depth Buffer:       Found (%ux%u)", g_sharedConfig->debugDepthW, g_sharedConfig->debugDepthH);
            } else {
                ImGui::TextDisabled("Depth Buffer:       None");
            }

            if (g_sharedConfig->enableDepthAware) {
                if (g_sharedConfig->debugHasDepth) {
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "Depth Bilateral:    Active (Guarding Silhouettes)");
                } else {
                    ImGui::TextDisabled("Depth Bilateral:    Enabled (Awaiting Depth Buffer)");
                }
            } else {
                ImGui::TextDisabled("Depth Bilateral:    Disabled");
            }

            if (g_sharedConfig->debugHasMVec) {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "Motion Vectors:     Found (%ux%u)", g_sharedConfig->debugMvW, g_sharedConfig->debugMvH);
            } else {
                ImGui::TextDisabled("Motion Vectors:     None");
            }

            if (g_sharedConfig->useCustomNR) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "NR Model Tuning:    Custom Override Active");
            } else {
                ImGui::TextDisabled("NR Model Tuning:    Caller Passthrough (Default)");
            }

            ImGui::Spacing();
            static ULONGLONG s_copiedTick = 0;
            if (ImGui::Button("Copy Debug Info to Clipboard")) {
                CopyDebugInfoToClipboard();
                s_copiedTick = GetTickCount64();
            }
            if (GetTickCount64() - s_copiedTick < 3000) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", "Copied!");
            }
        } else {
            ImGui::TextDisabled("%s", "No active frame telemetry received yet from proxy.");
            ImGui::TextDisabled("%s", "(Launch game with proxy DLL and enter 3D scene)");
        }
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Links & Credits")) {
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "%s", "DLSS-NR Cost Scaler");
        ImGui::Text("Created & Maintained by Xen");
        ImGui::Spacing();

        ImGui::BulletText("GitHub:");
        ImGui::SameLine();
        if (ImGui::Selectable("https://github.com/xenmods/DLSSNR-Cost-Scaler")) {
            ShellExecuteW(nullptr, L"open", L"https://github.com/xenmods/DLSSNR-Cost-Scaler", nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to open repository in default browser");
        }

        ImGui::BulletText("Discord:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.55f, 1.00f, 1.00f), "%s", "xenmods");
        ImGui::SameLine();
        static ULONGLONG s_discordCopiedTick = 0;
        if (ImGui::SmallButton("Copy Handle")) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                const char* handle = "xenmods";
                size_t len = strlen(handle) + 1;
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                if (hMem) {
                    memcpy(GlobalLock(hMem), handle, len);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_TEXT, hMem);
                }
                CloseClipboard();
                s_discordCopiedTick = GetTickCount64();
            }
        }
        if (GetTickCount64() - s_discordCopiedTick < 3000) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", "Copied!");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("%s", "Special thanks to RenoDX community, Shortfuse, and contributors.");
    }

    ImGui::PopStyleVar(2);
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
            return FALSE;
        LoadIniSettings();
        InitSharedMemory();
        reshade::register_overlay("DLSS-NR Cost Scaler", DrawOverlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay("DLSS-NR Cost Scaler", DrawOverlay);
        ShutdownSharedMemory();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
