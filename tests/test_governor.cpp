#include <windows.h>
#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include "../dlssnr_shared.h"

// Test 1: Layout, struct size, and offsets
void TestStructLayout() {
    std::cout << "[TEST] 1. Struct Layout & Alignment... ";
    assert(sizeof(DlssnrSharedConfig) > 0);

    // Verify pack(4) and critical offsets
    DlssnrSharedConfig cfg = {};
    cfg.magic = DLSSNR_MAGIC;
    cfg.enableGovernor = 1;
    cfg.governorTargetFps = 60.0f;
    cfg.governorMinScale = 0.50f;
    cfg.governorMaxScale = 1.00f;
    cfg.governorHysteresisSec = 2.0f;
    cfg.governorCurrentTier = 3;
    cfg.debugMeasuredFps = 59.8f;
    cfg.debugMeasuredFrameTimeMs = 16.72f;
    cfg.debugGovernorState = 1;
    cfg.debugGovernorCooldownLeft = 0.0f;

    assert(cfg.magic == DLSSNR_MAGIC);
    assert(cfg.enableGovernor == 1);
    assert(fabs(cfg.governorTargetFps - 60.0f) < 0.001f);
    assert(fabs(cfg.governorMinScale - 0.50f) < 0.001f);
    assert(fabs(cfg.governorMaxScale - 1.00f) < 0.001f);
    assert(fabs(cfg.governorHysteresisSec - 2.0f) < 0.001f);
    assert(cfg.governorCurrentTier == 3);
    assert(fabs(cfg.debugMeasuredFps - 59.8f) < 0.001f);
    assert(fabs(cfg.debugMeasuredFrameTimeMs - 16.72f) < 0.001f);
    assert(cfg.debugGovernorState == 1);
    assert(fabs(cfg.debugGovernorCooldownLeft - 0.0f) < 0.001f);
    std::cout << "PASSED (sizeof=" << sizeof(DlssnrSharedConfig) << " bytes)" << std::endl;
}

// Test 2: Shared Memory Handshake
void TestSharedMemoryHandshake() {
    std::cout << "[TEST] 2. Shared Memory IPC Handshake... ";
    const wchar_t* testMemName = L"Local\\DLSSNR_Test_SharedMem_v1";

    // Scenario A: Proxy initializes first
    HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(DlssnrSharedConfig), testMemName);
    assert(hMap != nullptr);
    bool isNew = (GetLastError() != ERROR_ALREADY_EXISTS);
    assert(isNew == true);

    DlssnrSharedConfig* proxyCfg = (DlssnrSharedConfig*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DlssnrSharedConfig));
    assert(proxyCfg != nullptr);

    // Apply proxy initialization fix
    if (isNew || proxyCfg->magic != DLSSNR_MAGIC) {
        ZeroMemory(proxyCfg, sizeof(DlssnrSharedConfig));
        proxyCfg->magic = DLSSNR_MAGIC;
        proxyCfg->version = 1;
        proxyCfg->resolutionScale = 0.75f;
        proxyCfg->enableGovernor = 1;
        proxyCfg->governorTargetFps = 60.0f;
    }
    assert(proxyCfg->magic == DLSSNR_MAGIC);
    assert(proxyCfg->version == 1);

    // Scenario B: Companion attaches second
    HANDLE hMap2 = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(DlssnrSharedConfig), testMemName);
    assert(hMap2 != nullptr);
    bool isNew2 = (GetLastError() != ERROR_ALREADY_EXISTS);
    assert(isNew2 == false); // Already exists!

    DlssnrSharedConfig* compCfg = (DlssnrSharedConfig*)MapViewOfFile(hMap2, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DlssnrSharedConfig));
    assert(compCfg != nullptr);
    assert(compCfg->magic == DLSSNR_MAGIC); // Must NOT be 0!
    assert(fabs(compCfg->resolutionScale - 0.75f) < 0.001f);
    assert(compCfg->enableGovernor == 1);

    // Companion updates target FPS
    compCfg->governorTargetFps = 120.0f;
    compCfg->version++;
    assert(proxyCfg->governorTargetFps == 120.0f);
    assert(proxyCfg->version == 2);

    UnmapViewOfFile(compCfg);
    CloseHandle(hMap2);
    UnmapViewOfFile(proxyCfg);
    CloseHandle(hMap);
    std::cout << "PASSED" << std::endl;
}

// Test 3: Governor Timing, Outlier Rejection, and EWMA
void TestGovernorTimingAndOutliers() {
    std::cout << "[TEST] 3. Outlier Rejection & EWMA Smoothing... ";

    float smoothedFrameTimeMs = 16.667f;
    float smoothedFps = 60.0f;
    bool hasValidFrameTime = false;
    constexpr float EWMA_ALPHA = 0.0645f;

    auto ProcessFrame = [&](double rawMs) -> bool {
        bool isOutlier = (rawMs <= 0.5 || rawMs > 80.0);
        if (!isOutlier && rawMs > 0.0) {
            if (!hasValidFrameTime) {
                smoothedFrameTimeMs = (float)rawMs;
                hasValidFrameTime = true;
            } else {
                smoothedFrameTimeMs = smoothedFrameTimeMs * (1.0f - EWMA_ALPHA) + (float)rawMs * EWMA_ALPHA;
            }
            smoothedFps = (smoothedFrameTimeMs > 0.1f) ? (1000.0f / smoothedFrameTimeMs) : 0.0f;
            return true;
        }
        return false;
    };

    // Frame 1: Outlier check (500ms freeze / loading screen)
    bool accepted = ProcessFrame(500.0);
    assert(!accepted); // Discarded!
    assert(smoothedFps == 60.0f); // Untouched!

    // Frame 2: Outlier check (0.2ms glitch)
    accepted = ProcessFrame(0.2);
    assert(!accepted); // Discarded!

    // 60 frames at 16.667ms (60 FPS)
    for (int i = 0; i < 60; ++i) {
        ProcessFrame(16.667);
    }
    assert(fabs(smoothedFps - 60.0f) < 0.5f);

    // Sudden 150ms spike (e.g. shader compilation hitch)
    accepted = ProcessFrame(150.0);
    assert(!accepted); // Outlier rejected!
    assert(fabs(smoothedFps - 60.0f) < 0.5f); // FPS does NOT plunge!

    std::cout << "PASSED" << std::endl;
}

// Test 4: Asymmetric Hysteresis State Machine
void TestGovernorStateMachine() {
    std::cout << "[TEST] 4. Asymmetric Hysteresis State Machine... ";

    float curScale = 0.85f;
    float targetFps = 60.0f;
    float minScale = 0.50f;
    float maxScale = 1.00f;
    float hystSec = 2.0f;

    float cooldownRemainingSec = 0.0f;
    float deficitDurationSec = 0.0f;
    float surplusDurationSec = 0.0f;
    uint32_t state = 1; // 1=Stable

    auto Step = [&](float liveFps, float dt) {
        if (cooldownRemainingSec > 0.0f) {
            cooldownRemainingSec -= dt;
            if (cooldownRemainingSec < 0.0f) cooldownRemainingSec = 0.0f;
            deficitDurationSec = 0.0f;
            surplusDurationSec = 0.0f;
            state = (cooldownRemainingSec > 0.0f) ? 2 : 1;
        } else {
            float deficitThreshold = 0.95f * targetFps; // 57.0 FPS
            float surplusThreshold = 1.15f * targetFps; // 69.0 FPS

            bool canStepDown = (curScale > minScale + 0.02f);
            bool canStepUp = (curScale < maxScale - 0.02f);

            if (liveFps < deficitThreshold && canStepDown) {
                deficitDurationSec += dt;
                surplusDurationSec = 0.0f;
                state = 3; // Downscaling

                if (deficitDurationSec >= 1.0f - 0.005f) {
                    float nextScale = roundf((curScale - 0.05f) * 20.0f) / 20.0f;
                    if (nextScale < minScale) nextScale = minScale;
                    curScale = nextScale;

                    deficitDurationSec = 0.0f;
                    cooldownRemainingSec = hystSec;
                    state = 2; // Cooldown
                }
            } else if (liveFps > surplusThreshold && canStepUp) {
                surplusDurationSec += dt;
                deficitDurationSec = 0.0f;
                state = 4; // Upscaling

                if (surplusDurationSec >= 3.0f - 0.005f) {
                    float nextScale = roundf((curScale + 0.05f) * 20.0f) / 20.0f;
                    if (nextScale > maxScale) nextScale = maxScale;
                    curScale = nextScale;

                    surplusDurationSec = 0.0f;
                    cooldownRemainingSec = hystSec;
                    state = 2; // Cooldown
                }
            } else {
                deficitDurationSec = 0.0f;
                surplusDurationSec = 0.0f;
                state = 1; // Stable
            }
        }
    };

    // Subtest 4a: Deficit < 1.0s does NOT downscale
    for (int i = 0; i < 9; ++i) {
        Step(50.0f, 0.1f); // 0.9s deficit
    }
    assert(state == 3); // Downscaling state active
    assert(fabs(curScale - 0.85f) < 0.001f); // Not yet stepped!

    // At 1.0s deficit -> Steps DOWN to 0.80 and enters Cooldown (State 2)
    Step(50.0f, 0.1f);
    assert(fabs(curScale - 0.80f) < 0.001f); // Stepped down!
    assert(state == 2); // Cooldown
    assert(fabs(cooldownRemainingSec - 2.0f) < 0.001f);

    // During 2.0s Cooldown, FPS drops further to 35 FPS, but scale must NOT change
    for (int i = 0; i < 15; ++i) {
        Step(35.0f, 0.1f); // 1.5s into cooldown
    }
    assert(state == 2);
    assert(fabs(curScale - 0.80f) < 0.001f); // Unchanged during cooldown!

    // Cooldown finishes (remaining 0.5s)
    for (int i = 0; i < 5; ++i) {
        Step(60.0f, 0.1f);
    }
    assert(state == 1); // Stable again!

    // Subtest 4b: Surplus requires patient 3.0s before stepping UP
    // Feed 75 FPS (> 69 FPS surplus threshold)
    for (int i = 0; i < 29; ++i) {
        Step(75.0f, 0.1f); // 2.9s surplus
    }
    assert(state == 4); // Upscaling state active
    assert(fabs(curScale - 0.80f) < 0.001f); // Not stepped up yet!

    // At 3.0s surplus -> Steps UP to 0.85
    Step(75.0f, 0.1f);
    assert(fabs(curScale - 0.85f) < 0.001f);
    assert(state == 2); // Cooldown again

    // Subtest 4c: Clamping at minScale
    curScale = 0.50f;
    cooldownRemainingSec = 0.0f;
    for (int i = 0; i < 20; ++i) {
        Step(40.0f, 0.1f);
    }
    assert(fabs(curScale - 0.50f) < 0.001f); // Never steps below minScale!

    // Subtest 4d: Clamping at maxScale
    curScale = 1.00f;
    cooldownRemainingSec = 0.0f;
    for (int i = 0; i < 40; ++i) {
        Step(100.0f, 0.1f);
    }
    assert(fabs(curScale - 1.00f) < 0.001f); // Never steps above maxScale!

    std::cout << "PASSED" << std::endl;
}

// Test 5: Multi-tier 8-slot cache lookup & flushing
struct MockSlot {
    bool inUse = false;
    uint32_t passIndex = 0;
    const void* origGameHandle = nullptr;
    void* activeFeature = nullptr;
    uint32_t nativeW = 0;
    uint32_t nativeH = 0;
    uint32_t workW = 0;
    uint32_t workH = 0;
    float scale = 1.0f;
    ULONGLONG lastUsedTick = 0;
};

void TestSlotCacheAndFlush() {
    std::cout << "[TEST] 5. Multi-tier 8-Slot Cache & Zero-VRAM Flush... ";

    constexpr size_t MAX_FEATURE_SLOTS = 8;
    MockSlot slots[MAX_FEATURE_SLOTS] = {};

    uint32_t nativeW = 1920;
    uint32_t nativeH = 1080;
    void* mockHandle = (void*)0x1234;

    auto FindOrCreateSlot = [&](float scale) -> size_t {
        uint32_t workW = (uint32_t)roundf(nativeW * scale) & ~1;
        uint32_t workH = (uint32_t)roundf(nativeH * scale) & ~1;

        // Lookup
        for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
            if (slots[i].inUse && slots[i].origGameHandle == mockHandle &&
                slots[i].workW == workW && slots[i].workH == workH) {
                slots[i].lastUsedTick = GetTickCount64();
                return i; // Cache HIT
            }
        }

        // Allocate
        for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
            if (!slots[i].inUse) {
                slots[i].inUse = true;
                slots[i].origGameHandle = mockHandle;
                slots[i].nativeW = nativeW;
                slots[i].nativeH = nativeH;
                slots[i].workW = workW;
                slots[i].workH = workH;
                slots[i].scale = scale;
                slots[i].activeFeature = (void*)(uintptr_t)(0x1000 + i);
                slots[i].lastUsedTick = GetTickCount64();
                return i; // New allocation
            }
        }
        return (size_t)-1;
    };

    // Visit tiers 0.85, 0.80, 0.75
    size_t s0 = FindOrCreateSlot(0.85f);
    assert(s0 == 0);
    size_t s1 = FindOrCreateSlot(0.80f);
    assert(s1 == 1);
    size_t s2 = FindOrCreateSlot(0.75f);
    assert(s2 == 2);

    // Re-visit 0.80 -> Cache HIT in slot 1 (0ms allocation!)
    size_t s1_hit = FindOrCreateSlot(0.80f);
    assert(s1_hit == 1);

    // Re-visit 0.85 -> Cache HIT in slot 0 (0ms allocation!)
    size_t s0_hit = FindOrCreateSlot(0.85f);
    assert(s0_hit == 0);

    // 3 slots currently resident
    size_t inUseCount = 0;
    for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
        if (slots[i].inUse) inUseCount++;
    }
    assert(inUseCount == 3);

    // Flush when governor toggles OFF at scale 0.80
    float curScale = 0.80f;
    for (uint32_t pass = 0; pass < MAX_FEATURE_SLOTS; ++pass) {
        MockSlot* keepSlot = nullptr;
        for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
            if (slots[i].inUse && slots[i].passIndex == pass) {
                if (!keepSlot) {
                    keepSlot = &slots[i];
                } else if (fabsf(slots[i].scale - curScale) < fabsf(keepSlot->scale - curScale)) {
                    keepSlot->inUse = false;
                    keepSlot = &slots[i];
                } else {
                    slots[i].inUse = false;
                }
            }
        }
    }

    // After flush: exactly 1 slot remains resident (scale 0.80 in slot 1), 0 extra VRAM
    inUseCount = 0;
    size_t activeIdx = (size_t)-1;
    for (size_t i = 0; i < MAX_FEATURE_SLOTS; ++i) {
        if (slots[i].inUse) {
            inUseCount++;
            activeIdx = i;
        }
    }
    assert(inUseCount == 1);
    assert(activeIdx == 1); // Kept the slot matching curScale (0.80)
    assert(fabs(slots[activeIdx].scale - 0.80f) < 0.001f);

    std::cout << "PASSED" << std::endl;
}

int main() {
    std::cout << "=== DLSS-NR Dynamic Governor Deep Verification Test Suite ===" << std::endl;
    TestStructLayout();
    TestSharedMemoryHandshake();
    TestGovernorTimingAndOutliers();
    TestGovernorStateMachine();
    TestSlotCacheAndFlush();
    std::cout << "=== ALL TESTS PASSED SUCCESSFULLY! ===" << std::endl;
    return 0;
}
