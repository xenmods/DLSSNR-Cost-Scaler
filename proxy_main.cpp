#include <mutex>
#include <vector>
#include <unordered_map>
#include "forwarders.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <stdio.h>
#include <math.h>
#include <atomic>
#include <string>

#include "nvsdk_ngx_params.h"

#include "Downsample_Shader.h"
#include "Resolve_Shader.h"
#include "dlssnr_shared.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static bool EnsureRealModuleLoaded();

static void GetCurrentModulePath(wchar_t* outPath, DWORD maxLen) {
    HMODULE hSelf = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&EnsureRealModuleLoaded, &hSelf) && hSelf) {
        GetModuleFileNameW(hSelf, outPath, maxLen);
    } else {
        GetModuleFileNameW(GetModuleHandleW(L"nvngx_dlssnr.dll"), outPath, maxLen);
    }
}

static FILE* g_logFile = nullptr;

static void Log(const char* fmt, ...) {
    if (!g_logFile) {
        wchar_t modulePath[MAX_PATH] = { 0 };
        GetCurrentModulePath(modulePath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(modulePath, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';
        wcscat_s(modulePath, L"nvngx_dlssnr_proxy.log");
        g_logFile = _wfsopen(modulePath, L"w", _SH_DENYNO);
        if (!g_logFile) {
            wchar_t tempPath[MAX_PATH] = { 0 };
            GetTempPathW(MAX_PATH, tempPath);
            wcscat_s(tempPath, L"nvngx_dlssnr_proxy.log");
            g_logFile = _wfsopen(tempPath, L"w", _SH_DENYNO);
        }
    }
    if (g_logFile) {
        va_list args;
        va_start(args, fmt);
        vfprintf(g_logFile, fmt, args);
        va_end(args);
        fprintf(g_logFile, "\n");
        fflush(g_logFile);
    }
}

static std::atomic<bool>     g_enableProxy(true);
static std::atomic<float>    g_scale(0.75f);
static std::atomic<bool>     g_enableAnamorphic(false);
static std::atomic<float>    g_scaleX(0.65f);
static std::atomic<float>    g_scaleY(0.85f);
static std::atomic<uint32_t> g_enlargementMode(1);     // 1 = Matched Residual, 0 = Classic Bilinear
static std::atomic<float>    g_transferStrength(1.0f); // 0.0 to 2.0
static std::atomic<float>    g_sharpness(0.20f);       // 0.0 to 1.0 (RCAS)
static std::atomic<float>    g_colorStrength(1.00f);   // 0.0 to 1.0 (Issue #5)
static std::atomic<bool>     g_enableVrnr(false);
static std::atomic<bool>     g_enableDepthAware(true);
static std::atomic<uint32_t> g_nrStyle(0);
static std::atomic<float>    g_nrIntensity(1.00f);
static std::atomic<float>    g_nrLocalStructureStrength(1.00f);
static std::atomic<float>    g_nrLocalToneStrength(1.00f);
static std::atomic<float>    g_nrSkinStructureStrength(-1.00f);
static std::atomic<uint32_t> g_nrUseAutoMask(0);
static std::atomic<bool>     g_useCustomNR(false);    // false = passthrough caller's NR params
static bool                  g_enableHotkeys = true;
static bool                  g_requireCtrlAlt = true;
static int                   g_keyToggleProxy = VK_SPACE;
static int                   g_keyToggleMode  = VK_END;
static int                   g_keyScaleUp     = VK_PRIOR;
static int                   g_keyScaleDown   = VK_NEXT;
static wchar_t               g_iniPath[MAX_PATH] = { 0 };
static FILETIME              g_lastIniWriteTime = { 0 };

static HANDLE              g_hProxySharedMem = nullptr;
static DlssnrSharedConfig* g_proxySharedConfig = nullptr;
static uint32_t            s_lastProxySharedVersion = 0;

static void InitProxySharedMemory() {
    if (g_proxySharedConfig) return;
    g_hProxySharedMem = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(DlssnrSharedConfig), DLSSNR_SHARED_MEM_NAME);
    if (g_hProxySharedMem) {
        g_proxySharedConfig = (DlssnrSharedConfig*)MapViewOfFile(g_hProxySharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DlssnrSharedConfig));
    }
}

static void SaveConfigValue(const wchar_t* key, const wchar_t* value) {
    if (g_iniPath[0] == L'\0') return;
    WritePrivateProfileStringW(L"DLSSNR_Proxy", key, value, g_iniPath);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_iniPath);
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (GetFileAttributesExW(g_iniPath, GetFileExInfoStandard, &fileInfo)) {
        g_lastIniWriteTime = fileInfo.ftLastWriteTime;
    }
}

static void PushProxyToSharedMemory();

static void LoadConfig() {
    if (g_iniPath[0] == L'\0') {
        wchar_t exePath[MAX_PATH] = { 0 };
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';
        wcscat_s(exePath, L"nvngx_dlssnr.ini");
        if (GetFileAttributesW(exePath) != INVALID_FILE_ATTRIBUTES) {
            wcscpy_s(g_iniPath, exePath);
        } else {
            GetCurrentModulePath(g_iniPath, MAX_PATH);
            wchar_t* lastSlash2 = wcsrchr(g_iniPath, L'\\');
            if (lastSlash2) *(lastSlash2 + 1) = L'\0';
            wcscat_s(g_iniPath, L"nvngx_dlssnr.ini");
        }
    }

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (GetFileAttributesExW(g_iniPath, GetFileExInfoStandard, &fileInfo)) {
        g_lastIniWriteTime = fileInfo.ftLastWriteTime;
    }

    wchar_t scaleBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"ResolutionScale", L"0.75", scaleBuf, 64, g_iniPath);
    float val = (float)_wtof(scaleBuf);
    if (val < 0.25f) val = 0.25f;
    if (val > 2.00f) val = 2.00f;
    g_scale.store(val);

    g_enableAnamorphic.store(GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnableAnamorphic", 0, g_iniPath) != 0);

    wchar_t scaleXBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"ResolutionScaleX", L"0.65", scaleXBuf, 64, g_iniPath);
    float sxVal = (float)_wtof(scaleXBuf);
    if (sxVal < 0.25f) sxVal = 0.25f;
    if (sxVal > 2.00f) sxVal = 2.00f;
    g_scaleX.store(sxVal);

    wchar_t scaleYBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"ResolutionScaleY", L"0.85", scaleYBuf, 64, g_iniPath);
    float syVal = (float)_wtof(scaleYBuf);
    if (syVal < 0.25f) syVal = 0.25f;
    if (syVal > 2.00f) syVal = 2.00f;
    g_scaleY.store(syVal);

    g_enableProxy.store(GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnableProxy", 1, g_iniPath) != 0);
    g_enableHotkeys = (GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnableHotkeys", 1, g_iniPath) != 0);

    g_enlargementMode.store((uint32_t)GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnlargementMode", 1, g_iniPath));

    wchar_t transferBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"TransferStrength", L"1.00", transferBuf, 64, g_iniPath);
    float tVal = (float)_wtof(transferBuf);
    if (tVal < 0.0f) tVal = 0.0f;
    if (tVal > 2.0f) tVal = 2.0f;
    g_transferStrength.store(tVal);

    wchar_t sharpBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"Sharpness", L"0.20", sharpBuf, 64, g_iniPath);
    float sVal = (float)_wtof(sharpBuf);
    if (sVal < 0.0f) sVal = 0.0f;
    if (sVal > 1.0f) sVal = 1.0f;
    g_sharpness.store(sVal);

    wchar_t colorBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Proxy", L"ColorStrength", L"1.00", colorBuf, 64, g_iniPath);
    float cVal = (float)_wtof(colorBuf);
    if (cVal < 0.0f) cVal = 0.0f;
    if (cVal > 1.0f) cVal = 1.0f;
    g_colorStrength.store(cVal);

    g_enableVrnr.store(GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnableAlternatingFrames", 0, g_iniPath) != 0);
    g_enableDepthAware.store(GetPrivateProfileIntW(L"DLSSNR_Proxy", L"EnableDepthAwareResolve", 1, g_iniPath) != 0);

    g_nrStyle.store((uint32_t)GetPrivateProfileIntW(L"DLSSNR_Settings", L"Style", 0, g_iniPath));
    wchar_t nrBuf[64] = { 0 };
    GetPrivateProfileStringW(L"DLSSNR_Settings", L"Intensity", L"1.00", nrBuf, 64, g_iniPath);
    g_nrIntensity.store((float)_wtof(nrBuf));

    GetPrivateProfileStringW(L"DLSSNR_Settings", L"LocalStructureStrength", L"1.00", nrBuf, 64, g_iniPath);
    g_nrLocalStructureStrength.store((float)_wtof(nrBuf));

    GetPrivateProfileStringW(L"DLSSNR_Settings", L"LocalToneStrength", L"1.00", nrBuf, 64, g_iniPath);
    g_nrLocalToneStrength.store((float)_wtof(nrBuf));

    GetPrivateProfileStringW(L"DLSSNR_Settings", L"SkinStructureStrength", L"-1.00", nrBuf, 64, g_iniPath);
    g_nrSkinStructureStrength.store((float)_wtof(nrBuf));

    g_nrUseAutoMask.store((uint32_t)GetPrivateProfileIntW(L"DLSSNR_Settings", L"UseAutoMask", 0, g_iniPath));
    g_useCustomNR.store(GetPrivateProfileIntW(L"DLSSNR_Settings", L"UseCustomSettings", 0, g_iniPath) != 0);

    g_requireCtrlAlt = (GetPrivateProfileIntW(L"Hotkeys", L"RequireCtrlAlt", 1, g_iniPath) != 0);
    g_keyToggleProxy = GetPrivateProfileIntW(L"Hotkeys", L"KeyToggleProxy", VK_SPACE, g_iniPath);
    g_keyToggleMode  = GetPrivateProfileIntW(L"Hotkeys", L"KeyToggleMode",  VK_END,   g_iniPath);
    g_keyScaleUp     = GetPrivateProfileIntW(L"Hotkeys", L"KeyScaleUp",     VK_PRIOR, g_iniPath);
    g_keyScaleDown   = GetPrivateProfileIntW(L"Hotkeys", L"KeyScaleDown",   VK_NEXT,  g_iniPath);

    Log("[Proxy] Config loaded: EnableProxy = %d, ResolutionScale = %.2f (Anamorphic=%d, ScaleX=%.2f, ScaleY=%.2f), EnlargementMode = %u, TransferStrength = %.2f, Sharpness = %.2f, ColorStrength = %.2f, EnableVrnr = %d, DepthAware = %d, UseCustomNR = %d, Style = %u, Intensity = %.2f",
        g_enableProxy.load() ? 1 : 0, val, g_enableAnamorphic.load() ? 1 : 0, g_scaleX.load(), g_scaleY.load(), g_enlargementMode.load(), g_transferStrength.load(), g_sharpness.load(), g_colorStrength.load(), g_enableVrnr.load() ? 1 : 0, g_enableDepthAware.load() ? 1 : 0, g_useCustomNR.load() ? 1 : 0, g_nrStyle.load(), g_nrIntensity.load());

    PushProxyToSharedMemory();
}

static void PushProxyToSharedMemory() {
    if (!g_proxySharedConfig || g_proxySharedConfig->magic != DLSSNR_MAGIC) return;
    g_proxySharedConfig->enableProxy = g_enableProxy.load() ? 1 : 0;
    g_proxySharedConfig->resolutionScale = g_scale.load();
    g_proxySharedConfig->enableAnamorphic = g_enableAnamorphic.load() ? 1 : 0;
    g_proxySharedConfig->scaleX = g_scaleX.load();
    g_proxySharedConfig->scaleY = g_scaleY.load();
    g_proxySharedConfig->enlargementMode = g_enlargementMode.load();
    g_proxySharedConfig->transferStrength = g_transferStrength.load();
    g_proxySharedConfig->colorStrength = g_colorStrength.load();
    g_proxySharedConfig->sharpness = g_sharpness.load();
    g_proxySharedConfig->enableHotkeys = g_enableHotkeys ? 1 : 0;
    g_proxySharedConfig->requireCtrlAlt = g_requireCtrlAlt ? 1 : 0;
    g_proxySharedConfig->keyToggleProxy = g_keyToggleProxy;
    g_proxySharedConfig->keyToggleMode = g_keyToggleMode;
    g_proxySharedConfig->keyScaleUp = g_keyScaleUp;
    g_proxySharedConfig->keyScaleDown = g_keyScaleDown;

    g_proxySharedConfig->enableVrnr = g_enableVrnr.load() ? 1 : 0;
    g_proxySharedConfig->enableDepthAware = g_enableDepthAware.load() ? 1 : 0;
    g_proxySharedConfig->nrStyle = g_nrStyle.load();
    g_proxySharedConfig->nrIntensity = g_nrIntensity.load();
    g_proxySharedConfig->nrLocalStructureStrength = g_nrLocalStructureStrength.load();
    g_proxySharedConfig->nrLocalToneStrength = g_nrLocalToneStrength.load();
    g_proxySharedConfig->nrSkinStructureStrength = g_nrSkinStructureStrength.load();
    g_proxySharedConfig->nrUseAutoMask = g_nrUseAutoMask.load();
    g_proxySharedConfig->useCustomNR = g_useCustomNR.load() ? 1 : 0;

    g_proxySharedConfig->writerSource = 2; // Proxy/Hotkey
    g_proxySharedConfig->version++;
    s_lastProxySharedVersion = g_proxySharedConfig->version;
}

static void CheckConfigHotReload() {
    InitProxySharedMemory();

    if (g_proxySharedConfig && g_proxySharedConfig->magic == DLSSNR_MAGIC) {
        if (g_proxySharedConfig->version != s_lastProxySharedVersion) {
            if (g_proxySharedConfig->writerSource != 2) {
                g_enableProxy.store(g_proxySharedConfig->enableProxy != 0);
                g_scale.store(g_proxySharedConfig->resolutionScale);
                g_enableAnamorphic.store(g_proxySharedConfig->enableAnamorphic != 0);
                g_scaleX.store(g_proxySharedConfig->scaleX);
                g_scaleY.store(g_proxySharedConfig->scaleY);
                g_enlargementMode.store(g_proxySharedConfig->enlargementMode);
                g_transferStrength.store(g_proxySharedConfig->transferStrength);
                g_colorStrength.store(g_proxySharedConfig->colorStrength);
                g_sharpness.store(g_proxySharedConfig->sharpness);
                g_enableHotkeys = (g_proxySharedConfig->enableHotkeys != 0);
                g_requireCtrlAlt = (g_proxySharedConfig->requireCtrlAlt != 0);
                g_keyToggleProxy = g_proxySharedConfig->keyToggleProxy;
                g_keyToggleMode  = g_proxySharedConfig->keyToggleMode;
                g_keyScaleUp     = g_proxySharedConfig->keyScaleUp;
                g_keyScaleDown   = g_proxySharedConfig->keyScaleDown;

                g_enableVrnr.store(g_proxySharedConfig->enableVrnr != 0);
                g_enableDepthAware.store(g_proxySharedConfig->enableDepthAware != 0);
                g_nrStyle.store(g_proxySharedConfig->nrStyle);
                g_nrIntensity.store(g_proxySharedConfig->nrIntensity);
                g_nrLocalStructureStrength.store(g_proxySharedConfig->nrLocalStructureStrength);
                g_nrLocalToneStrength.store(g_proxySharedConfig->nrLocalToneStrength);
                g_nrSkinStructureStrength.store(g_proxySharedConfig->nrSkinStructureStrength);
                g_nrUseAutoMask.store(g_proxySharedConfig->nrUseAutoMask != 0);
                g_useCustomNR.store(g_proxySharedConfig->useCustomNR != 0);
            }
            s_lastProxySharedVersion = g_proxySharedConfig->version;
        }
    }

    static ULONGLONG s_lastCheck = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastCheck < 100) return;
    s_lastCheck = now;

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (GetFileAttributesExW(g_iniPath, GetFileExInfoStandard, &fileInfo)) {
        if (CompareFileTime(&fileInfo.ftLastWriteTime, &g_lastIniWriteTime) != 0) {
            LoadConfig();
        }
    }
}

static void CheckHotkeys() {
    if (!g_enableHotkeys) return;

    static ULONGLONG s_lastPress = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastPress < 250) return;

    bool modifiersOk = true;
    if (g_requireCtrlAlt) {
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt  = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        modifiersOk = (ctrl && alt);
    }

    if (modifiersOk) {
        float current = g_scale.load();
        if ((GetAsyncKeyState(g_keyToggleProxy) & 0x8000) != 0) {
            bool newState = !g_enableProxy.load();
            g_enableProxy.store(newState);
            wchar_t buf[16];
            swprintf_s(buf, L"%d", newState ? 1 : 0);
            SaveConfigValue(L"EnableProxy", buf);
            PushProxyToSharedMemory();
            Log("[Proxy] Hotkey ToggleProxy: Proxy is now %s (synced to INI)", newState ? "ENABLED" : "DISABLED (Native Passthrough)");
            s_lastPress = now;
        }
        else if ((GetAsyncKeyState(g_keyToggleMode) & 0x8000) != 0) {
            uint32_t newMode = (g_enlargementMode.load() == 1) ? 0 : 1;
            g_enlargementMode.store(newMode);
            wchar_t buf[16];
            swprintf_s(buf, L"%u", newMode);
            SaveConfigValue(L"EnlargementMode", buf);
            PushProxyToSharedMemory();
            Log("[Proxy] Hotkey ToggleMode: EnlargementMode changed to %s (synced to INI)", newMode == 1 ? "Matched Residual" : "Classic Bilinear");
            s_lastPress = now;
        }
        else if ((GetAsyncKeyState(g_keyScaleUp) & 0x8000) != 0) {
            float next = (float)(floor((current + 0.051f) * 20.0f) / 20.0f);
            if (next > 2.00f) next = 2.00f;
            if (next != current) {
                g_scale.store(next);
                wchar_t buf[16];
                swprintf_s(buf, L"%.2f", next);
                SaveConfigValue(L"ResolutionScale", buf);
                PushProxyToSharedMemory();
                Log("[Proxy] Hotkey ScaleUp: Scale changed from %.2f to %.2f (synced to INI)", current, next);
                s_lastPress = now;
            }
        }
        else if ((GetAsyncKeyState(g_keyScaleDown) & 0x8000) != 0) {
            float next = (float)(floor((current - 0.049f) * 20.0f) / 20.0f);
            if (next < 0.25f) next = 0.25f;
            if (next != current) {
                g_scale.store(next);
                wchar_t buf[16];
                swprintf_s(buf, L"%.2f", next);
                SaveConfigValue(L"ResolutionScale", buf);
                PushProxyToSharedMemory();
                Log("[Proxy] Hotkey ScaleDown: Scale changed from %.2f to %.2f (synced to INI)", current, next);
                s_lastPress = now;
            }
        }
    }
}

typedef int(__cdecl *PFN_InitExt)(unsigned long long InApplicationId, const wchar_t* InApplicationDataPath, ID3D12Device* InDevice, int InVersion, const void* InFeatureInfo);
typedef int(__cdecl *PFN_Create)(ID3D12GraphicsCommandList* InCmdList, int InFeatureId, const void* InParameters, void** OutHandle);
typedef int(__cdecl *PFN_Evaluate)(ID3D12GraphicsCommandList* InCmdList, const void* InFeatureHandle, const void* InParameters, void* InCallback);
typedef int(__cdecl *PFN_Release)(void* InFeatureHandle);

static HMODULE g_realModule = nullptr;
static PFN_InitExt real_InitExt = nullptr;
static PFN_Create real_Create = nullptr;
static PFN_Evaluate real_Evaluate = nullptr;
static PFN_Release real_Release = nullptr;

typedef DWORD (WINAPI *PFN_GetModuleFileNameW)(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
static PFN_GetModuleFileNameW g_origGetModuleFileNameW = nullptr;

static DWORD WINAPI Hooked_GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
    if (g_origGetModuleFileNameW) {
        g_origGetModuleFileNameW(hModule, lpFilename, nSize);
    } else {
        GetModuleFileNameW(hModule, lpFilename, nSize);
    }
    wchar_t sysDir[MAX_PATH] = { 0 };
    if (GetSystemDirectoryW(sysDir, MAX_PATH) > 0) {
        swprintf_s(lpFilename, nSize, L"%ls\\nvngx.dll", sysDir);
    } else {
        wcscpy_s(lpFilename, nSize, L"C:\\Windows\\System32\\nvngx.dll");
    }
    return (DWORD)wcslen(lpFilename);
}

static void HookSnippetCallerCheck(HMODULE targetModule) {
    if (!targetModule) return;

    __try {
        BYTE* base = (BYTE*)targetModule;
        IMAGE_DOS_HEADER* dosHeader = (IMAGE_DOS_HEADER*)base;
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return;

        IMAGE_NT_HEADERS* ntHeaders = (IMAGE_NT_HEADERS*)(base + dosHeader->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return;

        IMAGE_DATA_DIRECTORY importDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (importDir.VirtualAddress == 0 || importDir.Size == 0) return;

        IMAGE_IMPORT_DESCRIPTOR* importDesc = (IMAGE_IMPORT_DESCRIPTOR*)(base + importDir.VirtualAddress);

        for (; importDesc->Name != 0; ++importDesc) {
            const char* modName = (const char*)(base + importDesc->Name);
            if (_stricmp(modName, "KERNEL32.dll") == 0) {
                IMAGE_THUNK_DATA* thunkOrig = (IMAGE_THUNK_DATA*)(base + (importDesc->OriginalFirstThunk ? importDesc->OriginalFirstThunk : importDesc->FirstThunk));
                IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + importDesc->FirstThunk);

                for (; thunk->u1.Function != 0; ++thunk, ++thunkOrig) {
                    if (!(thunkOrig->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                        IMAGE_IMPORT_BY_NAME* importByName = (IMAGE_IMPORT_BY_NAME*)(base + thunkOrig->u1.AddressOfData);
                        if (strcmp(importByName->Name, "GetModuleFileNameW") == 0) {
                            g_origGetModuleFileNameW = (PFN_GetModuleFileNameW)thunk->u1.Function;
                            DWORD oldProtect = 0;
                            if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                                thunk->u1.Function = (ULONG_PTR)&Hooked_GetModuleFileNameW;
                                VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                                Log("[Proxy] Installed GetModuleFileNameW hook in real module IAT");
                            }
                            return;
                        }
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[Proxy] SEH caught exception in HookSnippetCallerCheck");
    }
}

static bool EnsureRealModuleLoaded() {
    if (g_realModule) return true;

    wchar_t path[MAX_PATH] = { 0 };
    GetCurrentModulePath(path, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscat_s(path, L"nvngx_dlssnr_real.dll");

    g_realModule = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HookSnippetCallerCheck(g_realModule);
    if (!g_realModule) {
        Log("[Proxy] Failed to load real module from: %ls (error=%lu)", path, GetLastError());
        return false;
    }

    real_InitExt  = (PFN_InitExt)GetProcAddress(g_realModule, "NVSDK_NGX_D3D12_Init_Ext");
    real_Create   = (PFN_Create)GetProcAddress(g_realModule, "NVSDK_NGX_D3D12_CreateFeature");
    real_Evaluate = (PFN_Evaluate)GetProcAddress(g_realModule, "NVSDK_NGX_D3D12_EvaluateFeature");
    real_Release  = (PFN_Release)GetProcAddress(g_realModule, "NVSDK_NGX_D3D12_ReleaseFeature");

    Log("[Proxy] Loaded real module successfully (Init=%p, Create=%p, Eval=%p, Rel=%p)",
        real_InitExt, real_Create, real_Evaluate, real_Release);
    return true;
}

static DXGI_FORMAT ToNonTypeless(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return format;
    }
}

static DXGI_FORMAT ToUavCompatibleFormat(DXGI_FORMAT format) {
    format = ToNonTypeless(format);
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    // R11G11B10_FLOAT natively supports typed UAV writes in D3D12
    default: return format;
    }
}

static DXGI_FORMAT GetUavSafeScratchFormat(DXGI_FORMAT format) {
    return ToUavCompatibleFormat(format);
}

static DXGI_FORMAT ToDepthSrvFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

struct DownsampleConstants {
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t dstWidth;
    uint32_t dstHeight;
};

struct ResolveConstants {
    uint32_t nativeWidth;
    uint32_t nativeHeight;
    uint32_t workWidth;
    uint32_t workHeight;
    float    transferStrength;
    float    sharpness;
    uint32_t enlargementMode;
    float    colorStrength;
    uint32_t isSkipFrame;
    uint32_t hasDepth;
};

static ID3D12Device*             g_device = nullptr;
static ID3D12RootSignature*      g_rootSigDownsample = nullptr;
static ID3D12RootSignature*      g_rootSigResolve = nullptr;
static ID3D12PipelineState*      g_psoDownsample = nullptr;
static ID3D12PipelineState*      g_psoResolve = nullptr;
static ID3D12DescriptorHeap*     g_descHeap = nullptr;

// Multi-Slot Feature Cache to support concurrent viewports and hooks (e.g. MSFS 2024 Upscaled + Present)
struct FeatureSlot {
    bool                inUse = false;
    uint32_t            passIndex = 0;
    const void*         origGameHandle = nullptr;
    void*               activeFeature = nullptr;
    uint32_t            nativeW = 0;
    uint32_t            nativeH = 0;
    uint32_t            workW = 0;
    uint32_t            workH = 0;
    DXGI_FORMAT         colorFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT         scratchFormat = DXGI_FORMAT_UNKNOWN;
    float               scale = 1.0f;
    int                 skipEvaluateFrames = 0;

    ID3D12Resource*     colorSmall = nullptr;
    ID3D12Resource*     outputSmall = nullptr;
    ID3D12Resource*     nativeScratch = nullptr;
    D3D12_RESOURCE_STATES colorSmallState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES outputSmallState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES nativeScratchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ULONGLONG           lastUsedTick = 0;
    ULONGLONG           lastAllocAttemptTick = 0;
    ULONGLONG           lastCreateAttemptTick = 0;
    bool                allocFailed = false;
    bool                hasEvaluatedOnce = false;
};

static constexpr size_t MAX_FEATURE_SLOTS = 4;
static FeatureSlot g_slots[MAX_FEATURE_SLOTS] = {};

static std::recursive_mutex g_proxyMutex;

static constexpr int RETIRE_FRAME_DELAY = 64;

struct NrRetired {
    void* feature = nullptr;
    ID3D12Resource* resource = nullptr;
    int framesLeft = RETIRE_FRAME_DELAY;
};
static std::vector<NrRetired> g_retiredList;

static void ParkNrFeature(void*& feature) {
    if (!feature) return;
    NrRetired r;
    r.feature = feature;
    r.framesLeft = RETIRE_FRAME_DELAY;
    feature = nullptr;
    g_retiredList.push_back(r);
}

static void ReleaseSlotScratch(FeatureSlot& slot) {
    if (slot.colorSmall) {
        NrRetired r; r.resource = slot.colorSmall; r.framesLeft = RETIRE_FRAME_DELAY; g_retiredList.push_back(r);
        slot.colorSmall = nullptr;
    }
    if (slot.outputSmall) {
        NrRetired r; r.resource = slot.outputSmall; r.framesLeft = RETIRE_FRAME_DELAY; g_retiredList.push_back(r);
        slot.outputSmall = nullptr;
    }
    slot.colorSmallState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    slot.outputSmallState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

static void ReleaseSlotResources(FeatureSlot& slot) {
    if (slot.activeFeature) {
        ParkNrFeature(slot.activeFeature);
    }
    ReleaseSlotScratch(slot);
    if (slot.nativeScratch) {
        NrRetired r; r.resource = slot.nativeScratch; r.framesLeft = RETIRE_FRAME_DELAY; g_retiredList.push_back(r);
        slot.nativeScratch = nullptr;
    }
    slot.nativeScratchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

static void TickRetired() {
    for (size_t i = 0; i < g_retiredList.size();) {
        if (--g_retiredList[i].framesLeft > 0) {
            ++i;
            continue;
        }
        if (g_retiredList[i].feature) {
            if (real_Release) {
                int res = real_Release(g_retiredList[i].feature);
                Log("[Proxy] Retired DLSS-NR feature %p released (res=0x%X)", g_retiredList[i].feature, res);
            }
        }
        if (g_retiredList[i].resource) {
            g_retiredList[i].resource->Release();
        }
        g_retiredList.erase(g_retiredList.begin() + i);
    }
}

static void TransitionBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to || !res) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

static void ReleaseD3D12Pipeline() {
    if (g_psoDownsample) { g_psoDownsample->Release(); g_psoDownsample = nullptr; }
    if (g_psoResolve) { g_psoResolve->Release(); g_psoResolve = nullptr; }
    if (g_rootSigDownsample) { g_rootSigDownsample->Release(); g_rootSigDownsample = nullptr; }
    if (g_rootSigResolve) { g_rootSigResolve->Release(); g_rootSigResolve = nullptr; }
    if (g_descHeap) { g_descHeap->Release(); g_descHeap = nullptr; }
    g_device = nullptr;
}

static bool InitD3D12Pipeline(ID3D12Device* device) {
    if (!device) return false;
    if (g_device != nullptr && g_device != device) {
        Log("[Proxy] D3D12 device changed (%p -> %p), resetting pipeline and slots", g_device, device);
        ReleaseD3D12Pipeline();
        for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
            ReleaseSlotResources(g_slots[i]);
            g_slots[i].inUse = false;
        }
    }
    if (g_rootSigDownsample && g_rootSigResolve && g_psoDownsample && g_psoResolve && g_descHeap) return true;
    g_device = device;

    {
        D3D12_DESCRIPTOR_RANGE downRanges[2] = {};
        downRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        downRanges[0].NumDescriptors = 1;
        downRanges[0].BaseShaderRegister = 0;
        downRanges[0].RegisterSpace = 0;
        downRanges[0].OffsetInDescriptorsFromTableStart = 0;

        downRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        downRanges[1].NumDescriptors = 1;
        downRanges[1].BaseShaderRegister = 0;
        downRanges[1].RegisterSpace = 0;
        downRanges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER downParams[2] = {};
        downParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        downParams[0].Constants.ShaderRegister = 0;
        downParams[0].Constants.RegisterSpace = 0;
        downParams[0].Constants.Num32BitValues = 4;
        downParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        downParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        downParams[1].DescriptorTable.NumDescriptorRanges = 2;
        downParams[1].DescriptorTable.pDescriptorRanges = downRanges;
        downParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC downSampler = {};
        downSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        downSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        downSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        downSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        downSampler.ShaderRegister = 0;
        downSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC downRootDesc = {};
        downRootDesc.NumParameters = 2;
        downRootDesc.pParameters = downParams;
        downRootDesc.NumStaticSamplers = 1;
        downRootDesc.pStaticSamplers = &downSampler;

        ID3DBlob* signatureBlob = nullptr;
        ID3DBlob* errorBlob = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&downRootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
        if (FAILED(hr)) {
            Log("[Proxy] D3D12SerializeRootSignature (down) failed (hr=0x%08X)", hr);
            if (errorBlob) {
                Log("[Proxy] > %s", (const char*)errorBlob->GetBufferPointer());
                errorBlob->Release();
            }
            return false;
        }

        hr = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSigDownsample));
        if (FAILED(hr)) {
            Log("[Proxy] CreateRootSignature (down) failed (hr=0x%08X)", hr);
            signatureBlob->Release();
            return false;
        }
        signatureBlob->Release();
    }

    {
        D3D12_DESCRIPTOR_RANGE resolveRanges[2] = {};
        resolveRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        resolveRanges[0].NumDescriptors = 4; // t0: colorSmall, t1: outputSmall, t2: nativeColor, t3: depth
        resolveRanges[0].BaseShaderRegister = 0;
        resolveRanges[0].RegisterSpace = 0;
        resolveRanges[0].OffsetInDescriptorsFromTableStart = 0;

        resolveRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        resolveRanges[1].NumDescriptors = 1;
        resolveRanges[1].BaseShaderRegister = 0;
        resolveRanges[1].RegisterSpace = 0;
        resolveRanges[1].OffsetInDescriptorsFromTableStart = 4;

        D3D12_ROOT_PARAMETER resolveParams[2] = {};
        resolveParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        resolveParams[0].Constants.ShaderRegister = 0;
        resolveParams[0].Constants.RegisterSpace = 0;
        resolveParams[0].Constants.Num32BitValues = 10;
        resolveParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        resolveParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        resolveParams[1].DescriptorTable.NumDescriptorRanges = 2;
        resolveParams[1].DescriptorTable.pDescriptorRanges = resolveRanges;
        resolveParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC resolveRootDesc = {};
        resolveRootDesc.NumParameters = 2;
        resolveRootDesc.pParameters = resolveParams;
        resolveRootDesc.NumStaticSamplers = 1;
        resolveRootDesc.pStaticSamplers = &sampler;

        ID3DBlob* signatureBlob = nullptr;
        ID3DBlob* errorBlob = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&resolveRootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
        if (FAILED(hr)) {
            Log("[Proxy] D3D12SerializeRootSignature (resolve) failed (hr=0x%08X)", hr);
            if (errorBlob) {
                Log("[Proxy] > %s", (const char*)errorBlob->GetBufferPointer());
                errorBlob->Release();
            }
            return false;
        }

        hr = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSigResolve));
        if (FAILED(hr)) {
            Log("[Proxy] CreateRootSignature (resolve) failed (hr=0x%08X)", hr);
            signatureBlob->Release();
            return false;
        }
        signatureBlob->Release();
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC downPsoDesc = {};
    downPsoDesc.pRootSignature = g_rootSigDownsample;
    downPsoDesc.CS = { g_DownsampleShader, sizeof(g_DownsampleShader) };
    HRESULT hrPso = device->CreateComputePipelineState(&downPsoDesc, IID_PPV_ARGS(&g_psoDownsample));
    if (FAILED(hrPso)) {
        Log("[Proxy] CreateComputePipelineState (down) failed (hr=0x%08X)", hrPso);
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC resolvePsoDesc = {};
    resolvePsoDesc.pRootSignature = g_rootSigResolve;
    resolvePsoDesc.CS = { g_ResolveShader, sizeof(g_ResolveShader) };
    hrPso = device->CreateComputePipelineState(&resolvePsoDesc, IID_PPV_ARGS(&g_psoResolve));
    if (FAILED(hrPso)) {
        Log("[Proxy] CreateComputePipelineState (resolve) failed (hr=0x%08X)", hrPso);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 512;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hrHeap = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_descHeap));
    if (FAILED(hrHeap)) {
        Log("[Proxy] CreateDescriptorHeap failed (hr=0x%08X)", hrHeap);
        return false;
    }

    Log("[Proxy] D3D12 compute pipeline initialized (512 descriptors)");
    return true;
}

static ID3D12Resource* CreateScratchTexture(ID3D12Device* device, DXGI_FORMAT format, uint32_t width, uint32_t height) {
    if (!device) return nullptr;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* res = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    if (FAILED(hr)) {
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    }
    if (FAILED(hr)) {
        HRESULT reason = device->GetDeviceRemovedReason();
        static HRESULT s_lastLoggedHr = S_OK;
        static ULONGLONG s_lastLogTime = 0;
        ULONGLONG now = GetTickCount64();
        if (hr != s_lastLoggedHr || (now - s_lastLogTime) > 3000) {
            Log("[Proxy] Failed to allocate scratch texture %ux%u (hr=0x%08X, reason=0x%08X)", width, height, hr, reason);
            s_lastLoggedHr = hr;
            s_lastLogTime = now;
        }
        return nullptr;
    }
    return res;
}

extern "C" {

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_Init_Ext(
    unsigned long long InApplicationId,
    const wchar_t* InApplicationDataPath,
    ID3D12Device* InDevice,
    int InVersion,
    const void* InFeatureInfo)
{
    std::lock_guard<std::recursive_mutex> lock(g_proxyMutex);
    EnsureRealModuleLoaded();
    LoadConfig();

    Log("[Proxy] NVSDK_NGX_D3D12_Init_Ext (AppId=0x%llX, Device=%p)", InApplicationId, InDevice);
    if (!real_InitExt) return -1;
    return real_InitExt(InApplicationId, InApplicationDataPath, InDevice, InVersion, InFeatureInfo);
}

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList* InCmdList,
    int InFeatureId,
    const void* InParameters,
    void** OutHandle)
{
    std::lock_guard<std::recursive_mutex> lock(g_proxyMutex);
    EnsureRealModuleLoaded();
    CheckConfigHotReload();

    Log("[Proxy] NVSDK_NGX_D3D12_CreateFeature (FeatureId=%d)", InFeatureId);

    if (!real_Create) return -1;
    return real_Create(InCmdList, InFeatureId, InParameters, OutHandle);
}

static int EvaluateFeatureInternal(
    ID3D12GraphicsCommandList* InCmdList,
    const void* InFeatureHandle,
    const void* InParameters,
    void* InCallback)
{
    CheckConfigHotReload();
    CheckHotkeys();
    TickRetired();

    static ID3D12GraphicsCommandList* s_lastCmdList = nullptr;
    static LARGE_INTEGER s_qpcFreq = {};
    static LARGE_INTEGER s_lastPassQpc = {};
    static uint32_t s_passCountThisFrame = 0;
    static ID3D12Resource* s_lastResolveOutput = nullptr;

    if (s_qpcFreq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_qpcFreq);
    }

    LARGE_INTEGER nowQpc;
    QueryPerformanceCounter(&nowQpc);

    double deltaMs = 0.0;
    if (s_lastPassQpc.QuadPart > 0) {
        deltaMs = (double)(nowQpc.QuadPart - s_lastPassQpc.QuadPart) * 1000.0 / (double)s_qpcFreq.QuadPart;
    }
    s_lastPassQpc = nowQpc;

    // Consecutive evaluate calls within the same frame execute on the CPU within < 2.0 ms.
    // Inter-frame intervals (even at 240 FPS = 4.16 ms) are always >= 2.0 ms.
    if (InCmdList == s_lastCmdList && deltaMs > 0.0 && deltaMs < 2.0) {
        s_passCountThisFrame++;
    } else {
        s_passCountThisFrame = 0;
        s_lastCmdList = InCmdList;
        s_lastResolveOutput = nullptr;
    }

    NVSDK_NGX_Parameter* params = (NVSDK_NGX_Parameter*)InParameters;

    ID3D12Resource* origColor = nullptr;
    ID3D12Resource* origOutput = nullptr;
    if (params) {
        params->Get("DLSSNR.Color", &origColor);
        params->Get("DLSSNR.Output", &origOutput);
        if (!origColor) params->Get("Color", &origColor);
        if (!origOutput) params->Get("Output", &origOutput);
    }

    if (!origColor || !origOutput || !InCmdList) {
        static bool s_loggedNoRes = false;
        if (!s_loggedNoRes) {
            Log("[Proxy] EvaluateInternal: Bailout (no res): origColor=%p, origOutput=%p, InCmdList=%p", origColor, origOutput, InCmdList);
            s_loggedNoRes = true;
        }
        return real_Evaluate(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    D3D12_RESOURCE_DESC colorDesc = origColor->GetDesc();
    D3D12_RESOURCE_DESC outDesc = origOutput->GetDesc();

    if (colorDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        outDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        colorDesc.Width == 0 || colorDesc.Height == 0)
    {
        static bool s_loggedDim = false;
        if (!s_loggedDim) {
            Log("[Proxy] EvaluateInternal: Bailout (bad dim): colorDim=%d (%llux%u), outDim=%d (%llux%u)",
                colorDesc.Dimension, colorDesc.Width, colorDesc.Height,
                outDesc.Dimension, outDesc.Width, outDesc.Height);
            s_loggedDim = true;
        }
        return real_Evaluate(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    uint32_t nativeW = (uint32_t)colorDesc.Width;
    uint32_t nativeH = colorDesc.Height;
    DXGI_FORMAT typedColorFormat = ToNonTypeless(colorDesc.Format);
    float currentScale = g_scale.load();
    bool isAnamorphic = g_enableAnamorphic.load();
    float currentScaleX = isAnamorphic ? g_scaleX.load() : currentScale;
    float currentScaleY = isAnamorphic ? g_scaleY.load() : currentScale;

    bool isNativePassthrough = !isAnamorphic
        ? (currentScale >= 0.999f)
        : (fabsf(currentScaleX - 1.0f) < 0.005f && fabsf(currentScaleY - 1.0f) < 0.005f);

    // Pass through directly to real DLL when proxy is disabled OR scale is 100% native
    if (!g_enableProxy.load() || isNativePassthrough) {
        if (g_proxySharedConfig && g_proxySharedConfig->magic == DLSSNR_MAGIC) {
            g_proxySharedConfig->debugNativeW = nativeW;
            g_proxySharedConfig->debugNativeH = nativeH;
            g_proxySharedConfig->debugWorkW = nativeW;
            g_proxySharedConfig->debugWorkH = nativeH;
            g_proxySharedConfig->debugFormat = (uint32_t)typedColorFormat;
            g_proxySharedConfig->debugHasDepth = 0;
            g_proxySharedConfig->debugDepthW = 0;
            g_proxySharedConfig->debugDepthH = 0;
            g_proxySharedConfig->debugHasMVec = 0;
            g_proxySharedConfig->debugMvW = 0;
            g_proxySharedConfig->debugMvH = 0;
            g_proxySharedConfig->debugActiveSlot = 0;
        }
        return real_Evaluate(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    static int s_evalLogCount = 0;
    if (s_evalLogCount < 10) {
        s_evalLogCount++;
        Log("[Proxy] EvaluateInternal: scale=%.2f (Anamorphic=%d, scaleX=%.2f, scaleY=%.2f), params=%p, color=%p, output=%p, cmdList=%p",
            currentScale, isAnamorphic ? 1 : 0, currentScaleX, currentScaleY, params, origColor, origOutput, InCmdList);
    }

    ID3D12Device* device = nullptr;
    InCmdList->GetDevice(IID_PPV_ARGS(&device));
    if (!device) {
        static bool s_loggedDev = false;
        if (!s_loggedDev) {
            Log("[Proxy] EvaluateInternal: Bailout (GetDevice failed)");
            s_loggedDev = true;
        }
        return real_Evaluate(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    if (!InitD3D12Pipeline(device)) {
        static bool s_loggedPipe = false;
        if (!s_loggedPipe) {
            Log("[Proxy] EvaluateInternal: Bailout (InitD3D12Pipeline failed)");
            s_loggedPipe = true;
        }
        device->Release();
        return real_Evaluate(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    uint32_t workW = (fabsf(currentScaleX - 1.0f) < 0.005f) ? nativeW : (((uint32_t)roundf(nativeW * currentScaleX)) & ~1);
    uint32_t workH = (fabsf(currentScaleY - 1.0f) < 0.005f) ? nativeH : (((uint32_t)roundf(nativeH * currentScaleY)) & ~1);
    if (workW < 64) workW = 64;
    if (workH < 64) workH = 64;

    DXGI_FORMAT typedOutFormat = ToNonTypeless(outDesc.Format);
    DXGI_FORMAT scratchFormat = GetUavSafeScratchFormat(typedColorFormat);

    uint32_t currentPass = (s_passCountThisFrame < MAX_FEATURE_SLOTS) ? s_passCountThisFrame : (MAX_FEATURE_SLOTS - 1);

    // Multi-Slot Lookup: Find slot matching this game handle, pass index, resolution, and color format
    FeatureSlot* slot = nullptr;
    if (InFeatureHandle) {
        for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
            if (g_slots[i].inUse &&
                g_slots[i].origGameHandle == InFeatureHandle &&
                g_slots[i].passIndex == currentPass &&
                g_slots[i].nativeW == nativeW &&
                g_slots[i].nativeH == nativeH &&
                g_slots[i].colorFormat == typedColorFormat)
            {
                slot = &g_slots[i];
                break;
            }
        }
    }
    if (!slot) {
        for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
            if (g_slots[i].inUse &&
                g_slots[i].passIndex == currentPass &&
                g_slots[i].nativeW == nativeW &&
                g_slots[i].nativeH == nativeH &&
                g_slots[i].colorFormat == typedColorFormat)
            {
                slot = &g_slots[i];
                break;
            }
        }
    }

    // Allocate slot if not found
    if (!slot) {
        for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
            if (!g_slots[i].inUse) {
                slot = &g_slots[i];
                break;
            }
        }
        if (!slot) {
            size_t lruIdx = 0;
            ULONGLONG oldest = g_slots[0].lastUsedTick;
            for (size_t i = 1; i < MAX_FEATURE_SLOTS; ++i) {
                if (g_slots[i].lastUsedTick < oldest) {
                    oldest = g_slots[i].lastUsedTick;
                    lruIdx = i;
                }
            }
            slot = &g_slots[lruIdx];
            ReleaseSlotResources(*slot);
        }
        slot->inUse = true;
        slot->passIndex = currentPass;
        slot->origGameHandle = InFeatureHandle;
        slot->nativeW = nativeW;
        slot->nativeH = nativeH;
        slot->colorFormat = typedColorFormat;
        slot->scratchFormat = scratchFormat;
        slot->scale = 0.0f;
        slot->workW = 0;
        slot->workH = 0;
    }

    slot->origGameHandle = InFeatureHandle;
    slot->lastUsedTick = GetTickCount64();

    bool scaleChanged = (fabsf(slot->scale - currentScale) > 0.005f) || (slot->workW != workW) || (slot->workH != workH);
    bool formatChanged = (slot->colorFormat != typedColorFormat || slot->scratchFormat != scratchFormat);
    bool sizeChanged = (slot->nativeW != nativeW || slot->nativeH != nativeH);
    bool needRecreate = scaleChanged || formatChanged || sizeChanged || !slot->activeFeature;

    slot->colorFormat = typedColorFormat;
    slot->scratchFormat = scratchFormat;
    slot->nativeW = nativeW;
    slot->nativeH = nativeH;

    bool isScalingActive = (workW != nativeW || workH != nativeH);

    if (isScalingActive) {
        if (!slot->colorSmall || slot->workW != workW || slot->workH != workH) {
            ULONGLONG now = GetTickCount64();
            if (slot->allocFailed && (now - slot->lastAllocAttemptTick < 2000)) {
                // Wait during backoff after previous allocation failure
            } else {
                ReleaseSlotScratch(*slot);
                bool inPlace = (origColor == origOutput);
                bool outHasUav = (outDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
                bool needsNativeScratch = (inPlace || !outHasUav);

                if (needsNativeScratch) {
                    if (!slot->nativeScratch || slot->nativeW != nativeW || slot->nativeH != nativeH) {
                        if (slot->nativeScratch) {
                            NrRetired r; r.resource = slot->nativeScratch; r.framesLeft = RETIRE_FRAME_DELAY; g_retiredList.push_back(r);
                            slot->nativeScratch = nullptr;
                        }
                        slot->nativeScratch = CreateScratchTexture(device, scratchFormat, nativeW, nativeH);
                    }
                } else if (slot->nativeScratch) {
                    NrRetired r; r.resource = slot->nativeScratch; r.framesLeft = RETIRE_FRAME_DELAY; g_retiredList.push_back(r);
                    slot->nativeScratch = nullptr;
                }
                slot->colorSmall = CreateScratchTexture(device, scratchFormat, workW, workH);
                slot->outputSmall = CreateScratchTexture(device, scratchFormat, workW, workH);
                if (!slot->colorSmall || !slot->outputSmall) {
                    slot->allocFailed = true;
                    slot->lastAllocAttemptTick = now;
                    Log("[Proxy] Scratch texture allocation failed for %ux%u, backing off for 2s", workW, workH);
                } else {
                    slot->allocFailed = false;
                    slot->lastAllocAttemptTick = 0;
                    slot->workW = workW;
                    slot->workH = workH;
                    slot->scratchFormat = scratchFormat;
                    needRecreate = true;
                    Log("[Proxy] Allocated slot %u textures: work=%ux%u, native=%ux%u (Format=%d, ScratchFormat=%d, Scale=%.2f, ScaleX=%.2f, ScaleY=%.2f)",
                        currentPass, workW, workH, nativeW, nativeH, typedColorFormat, scratchFormat, currentScale, currentScaleX, currentScaleY);
                }
            }
        }
    } else {
        if (slot->colorSmall) {
            ReleaseSlotResources(*slot);
            needRecreate = true;
        }
    }

    if (!slot->colorSmall || !slot->outputSmall) {
        device->Release();
        return real_Evaluate(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    if (needRecreate) {
        if (slot->activeFeature) {
            ParkNrFeature(slot->activeFeature);
        }

        uint32_t origDlssW = nativeW, origDlssH = nativeH;
        params->Get("DLSSNR.Width", &origDlssW);
        params->Get("DLSSNR.Height", &origDlssH);

        uint32_t origColorSubW = nativeW, origColorSubH = nativeH;
        params->Get("DLSSNR.ColorSubrectWidth", &origColorSubW);
        params->Get("DLSSNR.ColorSubrectHeight", &origColorSubH);

        uint32_t origOutSubW = nativeW, origOutSubH = nativeH;
        params->Get("DLSSNR.OutputSubrectWidth", &origOutSubW);
        params->Get("DLSSNR.OutputSubrectHeight", &origOutSubH);

        params->Set("DLSSNR.Width", workW);
        params->Set("DLSSNR.Height", workH);
        params->Set("DLSSNR.ColorSubrectBaseX", 0u);
        params->Set("DLSSNR.ColorSubrectBaseY", 0u);
        params->Set("DLSSNR.ColorSubrectWidth", workW);
        params->Set("DLSSNR.ColorSubrectHeight", workH);
        params->Set("DLSSNR.OutputSubrectBaseX", 0u);
        params->Set("DLSSNR.OutputSubrectBaseY", 0u);
        params->Set("DLSSNR.OutputSubrectWidth", workW);
        params->Set("DLSSNR.OutputSubrectHeight", workH);

        // Guide buffer subrect synchronization during feature creation
        ID3D12Resource* depthResCreate = nullptr;
        if (params->Get("DLSSNR.Depth", &depthResCreate) != 0 || !depthResCreate) {
            params->Get("Depth", &depthResCreate);
        }
        uint32_t origDepthSubW = 0, origDepthSubH = 0;
        if (depthResCreate) {
            params->Get("DLSSNR.DepthSubrectWidth", &origDepthSubW);
            params->Get("DLSSNR.DepthSubrectHeight", &origDepthSubH);
            D3D12_RESOURCE_DESC dDesc = depthResCreate->GetDesc();
            uint32_t dW = origDepthSubW ? origDepthSubW : (uint32_t)dDesc.Width;
            uint32_t dH = origDepthSubH ? origDepthSubH : dDesc.Height;
            params->Set("DLSSNR.DepthSubrectBaseX", 0u);
            params->Set("DLSSNR.DepthSubrectBaseY", 0u);
            params->Set("DLSSNR.DepthSubrectWidth", dW);
            params->Set("DLSSNR.DepthSubrectHeight", dH);
        }

        ID3D12Resource* mvecResCreate = nullptr;
        if (params->Get("DLSSNR.MotionVectors", &mvecResCreate) != 0 || !mvecResCreate) {
            if (params->Get("DLSSNR.MVec", &mvecResCreate) != 0 || !mvecResCreate) {
                params->Get("MotionVectors", &mvecResCreate);
            }
        }
        uint32_t origMvSubW = 0, origMvSubH = 0;
        if (mvecResCreate) {
            params->Get("DLSSNR.MVecSubrectWidth", &origMvSubW);
            params->Get("DLSSNR.MVecSubrectHeight", &origMvSubH);
            D3D12_RESOURCE_DESC mDesc = mvecResCreate->GetDesc();
            uint32_t mW = origMvSubW ? origMvSubW : (uint32_t)mDesc.Width;
            uint32_t mH = origMvSubH ? origMvSubH : mDesc.Height;
            params->Set("DLSSNR.MVecSubrectBaseX", 0u);
            params->Set("DLSSNR.MVecSubrectBaseY", 0u);
            params->Set("DLSSNR.MVecSubrectWidth", mW);
            params->Set("DLSSNR.MVecSubrectHeight", mH);
        }

        if (isScalingActive) {
            params->Set("DLSSNR.Color", slot->colorSmall);
            params->Set("DLSSNR.Output", slot->outputSmall);
        } else {
            params->Set("DLSSNR.Color", origColor);
            params->Set("DLSSNR.Output", origOutput);
        }

        // Apply official DLSS-NR model settings (only when user opts in; otherwise caller's values pass through)
        if (g_useCustomNR.load()) {
            params->Set("DLSSNR.Style", g_nrStyle.load());
            params->Set("DLSSNR.Intensity", g_nrIntensity.load());
            params->Set("DLSSNR.LocalStructureStrength", g_nrLocalStructureStrength.load());
            params->Set("DLSSNR.LocalToneStrength", g_nrLocalToneStrength.load());
            if (g_nrSkinStructureStrength.load() >= 0.0f) {
                params->Set("DLSSNR.SkinStructureStrength", g_nrSkinStructureStrength.load());
            }
            params->Set("DLSSNR.UseAutoMask", g_nrUseAutoMask.load());
        }

        int createRes = real_Create(InCmdList, 18, params, &slot->activeFeature);
        Log("[Proxy] Created neural feature in slot %u (%ux%u -> native %ux%u, scale=%.2f, scaleX=%.2f, scaleY=%.2f): res=0x%X, handle=%p",
            currentPass, workW, workH, nativeW, nativeH, currentScale, currentScaleX, currentScaleY, createRes, slot->activeFeature);

        params->Set("DLSSNR.Color", origColor);
        params->Set("DLSSNR.Output", origOutput);
        params->Set("DLSSNR.Width", origDlssW ? origDlssW : nativeW);
        params->Set("DLSSNR.Height", origDlssH ? origDlssH : nativeH);
        params->Set("DLSSNR.ColorSubrectWidth", origColorSubW ? origColorSubW : nativeW);
        params->Set("DLSSNR.ColorSubrectHeight", origColorSubH ? origColorSubH : nativeH);
        params->Set("DLSSNR.OutputSubrectWidth", origOutSubW ? origOutSubW : nativeW);
        params->Set("DLSSNR.OutputSubrectHeight", origOutSubH ? origOutSubH : nativeH);
        if (depthResCreate && origDepthSubW > 0) {
            params->Set("DLSSNR.DepthSubrectWidth", origDepthSubW);
            params->Set("DLSSNR.DepthSubrectHeight", origDepthSubH);
        }
        if (mvecResCreate && origMvSubW > 0) {
            params->Set("DLSSNR.MVecSubrectWidth", origMvSubW);
            params->Set("DLSSNR.MVecSubrectHeight", origMvSubH);
        }

        if (NVSDK_NGX_FAILED(createRes) || !slot->activeFeature) {
            static ULONGLONG s_lastCreateFailTick = 0;
            ULONGLONG now = GetTickCount64();
            if (now - s_lastCreateFailTick > 2000) {
                Log("[Proxy] real_Create failed (0x%X), falling back to native passthrough", createRes);
                s_lastCreateFailTick = now;
            }
            slot->activeFeature = nullptr;
            slot->scale = currentScale;
            device->Release();
            return real_Evaluate(InCmdList, InFeatureHandle, InParameters, InCallback);
        }

        slot->scale = currentScale;
        slot->skipEvaluateFrames = 0;
    }

    device->Release();

    if (!slot->colorSmall || !slot->outputSmall || !slot->activeFeature) {
        return real_Evaluate(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    // Save original parameters
    float origMvX = 1.0f, origMvY = 1.0f;
    uint32_t origDlssW = nativeW, origDlssH = nativeH;

    uint32_t origColorBaseX = 0, origColorBaseY = 0, origColorSubW = nativeW, origColorSubH = nativeH;
    uint32_t origOutBaseX = 0, origOutBaseY = 0, origOutSubW = nativeW, origOutSubH = nativeH;
    uint32_t origDepthBaseX = 0, origDepthBaseY = 0, origDepthSubW = 0, origDepthSubH = 0;
    uint32_t origMvBaseX = 0, origMvBaseY = 0, origMvSubW = 0, origMvSubH = 0;

    if (params->Get("DLSSNR.MVecScaleX", &origMvX) != 0) params->Get("MVecScaleX", &origMvX);
    if (params->Get("DLSSNR.MVecScaleY", &origMvY) != 0) params->Get("MVecScaleY", &origMvY);
    params->Get("DLSSNR.Width", &origDlssW);
    params->Get("DLSSNR.Height", &origDlssH);

    params->Get("DLSSNR.ColorSubrectBaseX", &origColorBaseX);
    params->Get("DLSSNR.ColorSubrectBaseY", &origColorBaseY);
    params->Get("DLSSNR.ColorSubrectWidth", &origColorSubW);
    params->Get("DLSSNR.ColorSubrectHeight", &origColorSubH);

    params->Get("DLSSNR.OutputSubrectBaseX", &origOutBaseX);
    params->Get("DLSSNR.OutputSubrectBaseY", &origOutBaseY);
    params->Get("DLSSNR.OutputSubrectWidth", &origOutSubW);
    params->Get("DLSSNR.OutputSubrectHeight", &origOutSubH);

    params->Get("DLSSNR.DepthSubrectBaseX", &origDepthBaseX);
    params->Get("DLSSNR.DepthSubrectBaseY", &origDepthBaseY);
    params->Get("DLSSNR.DepthSubrectWidth", &origDepthSubW);
    params->Get("DLSSNR.DepthSubrectHeight", &origDepthSubH);

    params->Get("DLSSNR.MVecSubrectBaseX", &origMvBaseX);
    params->Get("DLSSNR.MVecSubrectBaseY", &origMvBaseY);
    params->Get("DLSSNR.MVecSubrectWidth", &origMvSubW);
    params->Get("DLSSNR.MVecSubrectHeight", &origMvSubH);

    // Query G-buffers if present (e.g. RenoDX Upscaled hook)
    ID3D12Resource* depthRes = nullptr;
    if (params->Get("DLSSNR.Depth", &depthRes) != 0 || !depthRes) {
        params->Get("Depth", &depthRes);
    }

    ID3D12Resource* mvecRes = nullptr;
    if (params->Get("DLSSNR.MVec", &mvecRes) != 0 || !mvecRes) {
        if (params->Get("MotionVectors", &mvecRes) != 0 || !mvecRes) {
            params->Get("DLSSNR.MotionVectors", &mvecRes);
        }
    }

    uint32_t actualDepthW = origDepthSubW, actualDepthH = origDepthSubH;
    if (depthRes) {
        D3D12_RESOURCE_DESC dDesc = depthRes->GetDesc();
        actualDepthW = origDepthSubW ? origDepthSubW : (uint32_t)dDesc.Width;
        actualDepthH = origDepthSubH ? origDepthSubH : dDesc.Height;
    }

    uint32_t actualMvW = origMvSubW, actualMvH = origMvSubH;
    if (mvecRes) {
        D3D12_RESOURCE_DESC mDesc = mvecRes->GetDesc();
        actualMvW = origMvSubW ? origMvSubW : (uint32_t)mDesc.Width;
        actualMvH = origMvSubH ? origMvSubH : mDesc.Height;
    }

    static bool s_loggedGbuffers = false;
    if (!s_loggedGbuffers && (depthRes || mvecRes)) {
        s_loggedGbuffers = true;
        Log("[Proxy] G-buffers detected on evaluate: Depth=%p (%ux%u), MVec=%p (%ux%u)",
            depthRes, actualDepthW, actualDepthH, mvecRes, actualMvW, actualMvH);
    }

    static uint64_t s_evaluateFrameIndex = 0;
    if (s_passCountThisFrame == 0) {
        s_evaluateFrameIndex++;
    }

    bool isVrnrActive = g_enableProxy.load() && g_enableVrnr.load() && isScalingActive;
    bool isSkipFrame = (isVrnrActive && slot->hasEvaluatedOnce && ((s_evaluateFrameIndex % 2) == 1));

    // Update live telemetry for companion UI
    if (g_proxySharedConfig && g_proxySharedConfig->magic == DLSSNR_MAGIC) {
        g_proxySharedConfig->debugNativeW = nativeW;
        g_proxySharedConfig->debugNativeH = nativeH;
        g_proxySharedConfig->debugWorkW = workW;
        g_proxySharedConfig->debugWorkH = workH;
        g_proxySharedConfig->debugFormat = (uint32_t)typedColorFormat;
        g_proxySharedConfig->debugHasDepth = depthRes ? 1 : 0;
        g_proxySharedConfig->debugDepthW = depthRes ? actualDepthW : 0;
        g_proxySharedConfig->debugDepthH = depthRes ? actualDepthH : 0;
        g_proxySharedConfig->debugHasMVec = mvecRes ? 1 : 0;
        g_proxySharedConfig->debugMvW = mvecRes ? actualMvW : 0;
        g_proxySharedConfig->debugMvH = mvecRes ? actualMvH : 0;
        g_proxySharedConfig->debugActiveSlot = currentPass;
        g_proxySharedConfig->debugVrnrSkippedThisFrame = isSkipFrame ? 1 : 0;
    }

    float mvFactorX = (float)workW / (float)nativeW;
    float mvFactorY = (float)workH / (float)nativeH;

    auto RestoreParameters = [&]() {
        params->Set("DLSSNR.Color", origColor);
        params->Set("DLSSNR.Output", origOutput);

        params->Set("DLSSNR.Width", origDlssW ? origDlssW : nativeW);
        params->Set("DLSSNR.Height", origDlssH ? origDlssH : nativeH);

        params->Set("DLSSNR.ColorSubrectBaseX", origColorBaseX);
        params->Set("DLSSNR.ColorSubrectBaseY", origColorBaseY);
        params->Set("DLSSNR.ColorSubrectWidth", origColorSubW ? origColorSubW : nativeW);
        params->Set("DLSSNR.ColorSubrectHeight", origColorSubH ? origColorSubH : nativeH);

        params->Set("DLSSNR.OutputSubrectBaseX", origOutBaseX);
        params->Set("DLSSNR.OutputSubrectBaseY", origOutBaseY);
        params->Set("DLSSNR.OutputSubrectWidth", origOutSubW ? origOutSubW : nativeW);
        params->Set("DLSSNR.OutputSubrectHeight", origOutSubH ? origOutSubH : nativeH);

        if (depthRes) {
            params->Set("DLSSNR.DepthSubrectBaseX", origDepthBaseX);
            params->Set("DLSSNR.DepthSubrectBaseY", origDepthBaseY);
            params->Set("DLSSNR.DepthSubrectWidth", origDepthSubW ? origDepthSubW : actualDepthW);
            params->Set("DLSSNR.DepthSubrectHeight", origDepthSubH ? origDepthSubH : actualDepthH);
        }

        if (mvecRes) {
            params->Set("DLSSNR.MVecSubrectBaseX", origMvBaseX);
            params->Set("DLSSNR.MVecSubrectBaseY", origMvBaseY);
            params->Set("DLSSNR.MVecSubrectWidth", origMvSubW ? origMvSubW : actualMvW);
            params->Set("DLSSNR.MVecSubrectHeight", origMvSubH ? origMvSubH : actualMvH);
        }

        params->Set("DLSSNR.MVecScaleX", origMvX);
        params->Set("DLSSNR.MVecScaleY", origMvY);
    };

    // Multi-pass transition barrier:
    // If this is a subsequent pass on the same command list and origColor was written
    // as a UAV output in the immediately preceding pass, transition it from UAV to SRV.
    bool origColorWasUavOutput = (s_passCountThisFrame > 0 && s_lastResolveOutput && origColor == s_lastResolveOutput);
    if (origColorWasUavOutput) {
        TransitionBarrier(InCmdList, origColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    int result = 1; // NVSDK_NGX_Result_Success

    UINT descSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    static uint32_t s_frameSlot = 0;
    s_frameSlot = (s_frameSlot + 1) % 64;
    uint32_t baseSlot = s_frameSlot * 8;

    D3D12_CPU_DESCRIPTOR_HANDLE heapCpuStart = g_descHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE heapGpuStart = g_descHeap->GetGPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap* heaps[] = { g_descHeap };

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    if (!isSkipFrame) {
        // Pass 1: Downsample native color to scratch input
        TransitionBarrier(InCmdList, slot->colorSmall, slot->colorSmallState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot->colorSmallState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle0 = { heapCpuStart.ptr + (baseSlot + 0) * descSize };
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle1 = { heapCpuStart.ptr + (baseSlot + 1) * descSize };
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandleDown = { heapGpuStart.ptr + (baseSlot + 0) * descSize };

        srvDesc.Format = typedColorFormat;
        g_device->CreateShaderResourceView(origColor, &srvDesc, cpuHandle0);

        uavDesc.Format = scratchFormat;
        g_device->CreateUnorderedAccessView(slot->colorSmall, nullptr, &uavDesc, cpuHandle1);

        InCmdList->SetComputeRootSignature(g_rootSigDownsample);
        InCmdList->SetDescriptorHeaps(1, heaps);

        DownsampleConstants downConstants = { nativeW, nativeH, workW, workH };
        InCmdList->SetComputeRoot32BitConstants(0, 4, &downConstants, 0);
        InCmdList->SetComputeRootDescriptorTable(1, gpuHandleDown);
        InCmdList->SetPipelineState(g_psoDownsample);
        InCmdList->Dispatch((workW + 7) / 8, (workH + 7) / 8, 1);

        TransitionBarrier(InCmdList, slot->colorSmall, slot->colorSmallState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        slot->colorSmallState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        TransitionBarrier(InCmdList, slot->outputSmall, slot->outputSmallState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot->outputSmallState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        // Apply official DLSS-NR model settings (only when user opts in; otherwise caller's values pass through)
        if (g_useCustomNR.load()) {
            params->Set("DLSSNR.Style", g_nrStyle.load());
            params->Set("DLSSNR.Intensity", g_nrIntensity.load());
            params->Set("DLSSNR.LocalStructureStrength", g_nrLocalStructureStrength.load());
            params->Set("DLSSNR.LocalToneStrength", g_nrLocalToneStrength.load());
            if (g_nrSkinStructureStrength.load() >= 0.0f) {
                params->Set("DLSSNR.SkinStructureStrength", g_nrSkinStructureStrength.load());
            }
            params->Set("DLSSNR.UseAutoMask", g_nrUseAutoMask.load());
        }

        params->Set("DLSSNR.Color", slot->colorSmall);
        params->Set("DLSSNR.Output", slot->outputSmall);

        params->Set("DLSSNR.Width", workW);
        params->Set("DLSSNR.Height", workH);

        params->Set("DLSSNR.ColorSubrectBaseX", 0u);
        params->Set("DLSSNR.ColorSubrectBaseY", 0u);
        params->Set("DLSSNR.ColorSubrectWidth", workW);
        params->Set("DLSSNR.ColorSubrectHeight", workH);

        params->Set("DLSSNR.OutputSubrectBaseX", 0u);
        params->Set("DLSSNR.OutputSubrectBaseY", 0u);
        params->Set("DLSSNR.OutputSubrectWidth", workW);
        params->Set("DLSSNR.OutputSubrectHeight", workH);

        if (depthRes && actualDepthW > 0 && actualDepthH > 0) {
            params->Set("DLSSNR.DepthSubrectBaseX", origDepthBaseX);
            params->Set("DLSSNR.DepthSubrectBaseY", origDepthBaseY);
            params->Set("DLSSNR.DepthSubrectWidth", actualDepthW);
            params->Set("DLSSNR.DepthSubrectHeight", actualDepthH);
        }

        if (mvecRes && actualMvW > 0 && actualMvH > 0) {
            params->Set("DLSSNR.MVecSubrectBaseX", origMvBaseX);
            params->Set("DLSSNR.MVecSubrectBaseY", origMvBaseY);
            params->Set("DLSSNR.MVecSubrectWidth", actualMvW);
            params->Set("DLSSNR.MVecSubrectHeight", actualMvH);
        }

        params->Set("DLSSNR.MVecScaleX", origMvX * mvFactorX);
        params->Set("DLSSNR.MVecScaleY", origMvY * mvFactorY);

        result = real_Evaluate(InCmdList, slot->activeFeature, InParameters, InCallback);

        // If neural evaluate failed, skip resolve pass to avoid corrupting output
        if (NVSDK_NGX_FAILED(result)) {
            Log("[Proxy] real_Evaluate failed (0x%X), skipping resolve pass", result);
            RestoreParameters();
            return result;
        }

        slot->hasEvaluatedOnce = true;

        TransitionBarrier(InCmdList, slot->outputSmall, slot->outputSmallState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        slot->outputSmallState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    bool inPlace = (origColor == origOutput);
    bool outHasUav = (outDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;

    ID3D12Resource* resolveReadSource = origColor;
    ID3D12Resource* resolveWriteDest = origOutput;
    DXGI_FORMAT resolveReadFormat = typedColorFormat;
    DXGI_FORMAT resolveWriteFormat = ToUavCompatibleFormat(typedOutFormat);

    if (inPlace && slot->nativeScratch && colorDesc.Format == slot->nativeScratch->GetDesc().Format) {
        TransitionBarrier(InCmdList, origColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        TransitionBarrier(InCmdList, slot->nativeScratch, slot->nativeScratchState, D3D12_RESOURCE_STATE_COPY_DEST);
        slot->nativeScratchState = D3D12_RESOURCE_STATE_COPY_DEST;
        InCmdList->CopyResource(slot->nativeScratch, origColor);

        TransitionBarrier(InCmdList, slot->nativeScratch, slot->nativeScratchState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        slot->nativeScratchState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        TransitionBarrier(InCmdList, origColor, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        resolveReadSource = slot->nativeScratch;
        resolveReadFormat = scratchFormat;
    }
    else if (!outHasUav && slot->nativeScratch) {
        TransitionBarrier(InCmdList, slot->nativeScratch, slot->nativeScratchState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot->nativeScratchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        resolveWriteDest = slot->nativeScratch;
        resolveWriteFormat = scratchFormat;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuRes0 = { heapCpuStart.ptr + (baseSlot + 2) * descSize };
    D3D12_CPU_DESCRIPTOR_HANDLE cpuRes1 = { heapCpuStart.ptr + (baseSlot + 3) * descSize };
    D3D12_CPU_DESCRIPTOR_HANDLE cpuRes2 = { heapCpuStart.ptr + (baseSlot + 4) * descSize };
    D3D12_CPU_DESCRIPTOR_HANDLE cpuRes3 = { heapCpuStart.ptr + (baseSlot + 5) * descSize };
    D3D12_CPU_DESCRIPTOR_HANDLE cpuRes4 = { heapCpuStart.ptr + (baseSlot + 6) * descSize };
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandleResolve = { heapGpuStart.ptr + (baseSlot + 2) * descSize };

    srvDesc.Format = scratchFormat;
    g_device->CreateShaderResourceView(slot->colorSmall, &srvDesc, cpuRes0);
    g_device->CreateShaderResourceView(slot->outputSmall, &srvDesc, cpuRes1);

    srvDesc.Format = resolveReadFormat;
    g_device->CreateShaderResourceView(resolveReadSource, &srvDesc, cpuRes2);

    bool hasValidDepth = false;
    DXGI_FORMAT depthSrvFormat = DXGI_FORMAT_UNKNOWN;
    if (depthRes != nullptr && g_enableDepthAware.load()) {
        D3D12_RESOURCE_DESC dDesc = depthRes->GetDesc();
        depthSrvFormat = ToDepthSrvFormat(dDesc.Format);
        if (depthSrvFormat != DXGI_FORMAT_UNKNOWN &&
            (dDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 &&
            (uint32_t)dDesc.Width == nativeW &&
            dDesc.Height == nativeH) {
            hasValidDepth = true;
        }
    }

    if (hasValidDepth) {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;
        depthSrvDesc.Format = depthSrvFormat;
        g_device->CreateShaderResourceView(depthRes, &depthSrvDesc, cpuRes3);
    } else {
        srvDesc.Format = scratchFormat;
        g_device->CreateShaderResourceView(slot->colorSmall, &srvDesc, cpuRes3);
    }

    uavDesc.Format = resolveWriteFormat;
    g_device->CreateUnorderedAccessView(resolveWriteDest, nullptr, &uavDesc, cpuRes4);

    InCmdList->SetComputeRootSignature(g_rootSigResolve);
    InCmdList->SetDescriptorHeaps(1, heaps);

    ResolveConstants resConstants = {};
    resConstants.nativeWidth = nativeW;
    resConstants.nativeHeight = nativeH;
    resConstants.workWidth = workW;
    resConstants.workHeight = workH;
    // Multi-pass protection:
    // If this pass is writing to the same buffer in a multi-pass pipeline,
    // disable RCAS to prevent exponential edge ringing / shimmer.
    // Transfer strength is preserved full-power (never halved) to prevent multi-pass flickering.
    bool isSameBufferMultiPass = (s_passCountThisFrame > 0 && s_lastResolveOutput && origColor == s_lastResolveOutput);
    resConstants.transferStrength = g_transferStrength.load();
    resConstants.sharpness = isSameBufferMultiPass ? 0.0f : g_sharpness.load();
    resConstants.enlargementMode = g_enlargementMode.load();
    resConstants.colorStrength = g_colorStrength.load();
    resConstants.isSkipFrame = isSkipFrame ? 1 : 0;
    resConstants.hasDepth = hasValidDepth ? 1 : 0;

    InCmdList->SetComputeRoot32BitConstants(0, 10, &resConstants, 0);
    InCmdList->SetComputeRootDescriptorTable(1, gpuHandleResolve);
    InCmdList->SetPipelineState(g_psoResolve);
    InCmdList->Dispatch((nativeW + 7) / 8, (nativeH + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uavFlush = {};
    uavFlush.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavFlush.UAV.pResource = resolveWriteDest;
    InCmdList->ResourceBarrier(1, &uavFlush);

    s_lastResolveOutput = resolveWriteDest;

    if (!outHasUav && slot->nativeScratch && resolveWriteDest == slot->nativeScratch && outDesc.Format == slot->nativeScratch->GetDesc().Format) {
        TransitionBarrier(InCmdList, slot->nativeScratch, slot->nativeScratchState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        slot->nativeScratchState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        TransitionBarrier(InCmdList, origOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        InCmdList->CopyResource(origOutput, slot->nativeScratch);
        TransitionBarrier(InCmdList, origOutput, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (!isSkipFrame) {
        RestoreParameters();
    }

    return result;
}

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* InCmdList,
    const void* InFeatureHandle,
    const void* InParameters,
    void* InCallback)
{
    g_proxyMutex.lock();
    if (!real_Evaluate) {
        g_proxyMutex.unlock();
        return -1;
    }

    int result = -1;
    __try {
        result = EvaluateFeatureInternal(InCmdList, InFeatureHandle, InParameters, InCallback);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[Proxy] CRITICAL: SEH Exception 0x%08X caught in EvaluateFeature! Falling back to native passthrough.", GetExceptionCode());
        if (real_Evaluate) {
            result = real_Evaluate(InCmdList, InFeatureHandle, InParameters, InCallback);
        }
    }
    g_proxyMutex.unlock();
    return result;
}

__declspec(dllexport) void __cdecl NVSDK_NGX_D3D12_ReleaseFeature(void* InFeatureHandle)
{
    std::lock_guard<std::recursive_mutex> lock(g_proxyMutex);
    EnsureRealModuleLoaded();
    Log("[Proxy] NVSDK_NGX_D3D12_ReleaseFeature (Handle=%p)", InFeatureHandle);

    bool alreadyParked = false;
    for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
        if (g_slots[i].inUse && (g_slots[i].origGameHandle == InFeatureHandle || g_slots[i].activeFeature == InFeatureHandle)) {
            if (g_slots[i].activeFeature == InFeatureHandle) {
                alreadyParked = true;
            }
            ReleaseSlotResources(g_slots[i]);
            g_slots[i].inUse = false;
            g_slots[i].origGameHandle = nullptr;
            break;
        }
    }

    if (InFeatureHandle && !alreadyParked && real_Release) {
        void* f = InFeatureHandle;
        ParkNrFeature(f);
    }
}

}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
    } else if (fdwReason == DLL_PROCESS_DETACH && !lpvReserved) {
        ReleaseD3D12Pipeline();
        for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
            if (g_slots[i].inUse) {
                ReleaseSlotResources(g_slots[i]);
                g_slots[i].inUse = false;
            }
        }
        if (g_logFile) {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
        if (g_realModule) {
            FreeLibrary(g_realModule);
            g_realModule = nullptr;
        }
    }
    return TRUE;
}
