// ================================================================================================
// DLSS-NR Proxy Shaders: Area-Weighted Downsample + High-Frequency Matched Residual Resolve
// ================================================================================================

// --- DOWNSAMPLE SHADER ---
cbuffer DownConstants : register(b0)
{
    uint gSrcWidth;
    uint gSrcHeight;
    uint gDstWidth;
    uint gDstHeight;
};

Texture2D<float4>   gDownSource  : register(t0);
RWTexture2D<float4> gDownTarget  : register(u0);
SamplerState        gLinearClamp : register(s0);

[numthreads(8, 8, 1)]
void CS_Downsample(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gDstWidth || id.y >= gDstHeight)
        return;

    // Hardware TMU bilinear downsampling (ultra-fast, zero-ALU overhead)
    float2 uv = (float2(id.xy) + 0.5f) / float2(gDstWidth, gDstHeight);
    gDownTarget[id.xy] = gDownSource.SampleLevel(gLinearClamp, uv, 0);
}


// --- RESOLVE & RESIDUAL COMPOSITE SHADER ---
cbuffer ResolveConstants : register(b0)
{
    uint  gNativeWidth;
    uint  gNativeHeight;
    uint  gWorkWidth;
    uint  gWorkHeight;
    float gTransferStrength;
    float gSharpness;
    uint  gEnlargementMode;
    float gColorStrength;
    uint  gIsSkipFrame;
    uint  gHasDepth;
};

Texture2D<float4>   gSmallInput    : register(t0); // Downsampled model input (g_colorSmall)
Texture2D<float4>   gSmallOutput   : register(t1); // Model output (g_outputSmall)
Texture2D<float4>   gNativeColor   : register(t2); // Pristine native frame (origColor)
Texture2D<float>    gDepth         : register(t3); // Native depth buffer (if available)
RWTexture2D<float4> gResolveTarget : register(u0); // Destination (origOutput)

SamplerState gLinear : register(s0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
groupshared float3 s_nativeTile[10][10];

[numthreads(8, 8, 1)]
void CS_Resolve(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    // Cooperative loading of 10x10 apron into LDS (eliminates redundant VRAM reads during RCAS)
    int2 baseCoord = int2(gid.xy * 8) - 1;
    int2 maxCoord = int2((int)gNativeWidth - 1, (int)gNativeHeight - 1);
    uint linearThreadId = tid.y * 8 + tid.x;

    int2 c0 = clamp(baseCoord + int2((int)(linearThreadId % 10), (int)(linearThreadId / 10)), int2(0, 0), maxCoord);
    s_nativeTile[linearThreadId / 10][linearThreadId % 10] = gNativeColor.Load(int3(c0, 0)).rgb;

    if (linearThreadId < 36)
    {
        uint idx1 = linearThreadId + 64;
        int2 c1 = clamp(baseCoord + int2((int)(idx1 % 10), (int)(idx1 / 10)), int2(0, 0), maxCoord);
        s_nativeTile[idx1 / 10][idx1 % 10] = gNativeColor.Load(int3(c1, 0)).rgb;
    }

    GroupMemoryBarrierWithGroupSync();

    if (id.x >= gNativeWidth || id.y >= gNativeHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gNativeWidth, gNativeHeight);

    // Mode 0: Direct Neural Reconstruction with RCAS (Recommended for DLSS-NR / Ray Reconstruction)
    if (gEnlargementMode == 0)
    {
        float4 outSample = gSmallOutput.SampleLevel(gLinear, uv, 0);
        float3 result = outSample.rgb;
        float3 original = s_nativeTile[tid.y + 1][tid.x + 1];

        // Blend with native if TransferStrength < 1.0 or on skip frames
        if (gIsSkipFrame != 0)
        {
            float3 inSample = gSmallInput.SampleLevel(gLinear, uv, 0).rgb;
            float lumaIn = dot(max(inSample, 0.0), kLuma);
            float lumaNative = dot(max(original, 0.0), kLuma);
            float diff = abs(lumaIn - lumaNative);
            float weight = saturate(1.0 - (diff * 2.0) / (lumaIn + lumaNative + 0.05));
            result = lerp(original, result, weight * saturate(gTransferStrength));
        }
        else if (gTransferStrength < 0.999)
        {
            result = lerp(original, result, saturate(gTransferStrength));
        }

        // Contrast-adaptive edge sharpening (RCAS) on denoised features
        if (gSharpness > 0.001)
        {
            float2 px = float2(1.0 / (float)gNativeWidth, 1.0 / (float)gNativeHeight);
            float3 cE = gSmallOutput.SampleLevel(gLinear, uv + float2( px.x, 0), 0).rgb;
            float3 cW = gSmallOutput.SampleLevel(gLinear, uv + float2(-px.x, 0), 0).rgb;
            float3 cS = gSmallOutput.SampleLevel(gLinear, uv + float2(0,  px.y), 0).rgb;
            float3 cN = gSmallOutput.SampleLevel(gLinear, uv + float2(0, -px.y), 0).rgb;

            float lE = dot(cE, kLuma);
            float lW = dot(cW, kLuma);
            float lS = dot(cS, kLuma);
            float lN = dot(cN, kLuma);
            float lM = dot(result, kLuma);

            float minL = min(lM, min(min(lE, lW), min(lS, lN)));
            float maxL = max(lM, max(max(lE, lW), max(lS, lN)));

            float range = maxL - minL;
            if (range > 1e-5)
            {
                float3 crossAvg = (cE + cW + cS + cN) * 0.25;
                float3 highFreq = result - crossAvg;
                float adaptiveScale = saturate(1.0 - range / (maxL + 1e-4));
                float rcasWeight = saturate(gSharpness) * (0.2 + 0.8 * adaptiveScale);
                result = max(result + highFreq * rcasWeight, 0.0);
            }
        }

        float nativeAlpha = gNativeColor.Load(int3(id.xy, 0)).a;
        gResolveTarget[id.xy] = float4(max(result, 0.0), nativeAlpha);
        return;
    }

    // Mode 1: Matched Residual (1:1 Native Resolution Anchor + Scaled Neural Delta)
    // 1. Pristine 1:1 Native Game Pixel loaded directly from on-chip LDS tile
    float3 original = s_nativeTile[tid.y + 1][tid.x + 1];

    // 2. Sample neural input and output at standard screen UV
    float3 smallInput = gSmallInput.SampleLevel(gLinear, uv, 0).rgb;
    float3 smallOutput = gSmallOutput.SampleLevel(gLinear, uv, 0).rgb;

    // 3. Compute neural delta / edit
    float3 edit = smallOutput - smallInput;

    // Chroma vs Luma control for ColorStrength
    float editLuma = dot(edit, kLuma);
    float3 editChroma = edit - editLuma;
    float3 controlledEdit = editLuma + editChroma * saturate(gColorStrength);

    // Depth-Aware Bilateral Silhouette Preservation:
    // If native depth is available, detect geometric silhouette discontinuities and prevent
    // low-res neural radiance deltas from bleeding across thin foreground edges.
    if (gHasDepth != 0)
    {
        float nativeDepth = gDepth.Load(int3(id.xy, 0)).r;
        float dE = gDepth.Load(int3(min(id.x + 1, (uint)maxCoord.x), id.y, 0)).r;
        float dW = gDepth.Load(int3(max((int)id.x - 1, 0), id.y, 0)).r;
        float dS = gDepth.Load(int3(id.x, min(id.y + 1, (uint)maxCoord.y), 0)).r;
        float dN = gDepth.Load(int3(id.x, max((int)id.y - 1, 0), 0)).r;

        float minD = min(nativeDepth, min(min(dE, dW), min(dS, dN)));
        float maxD = max(nativeDepth, max(max(dE, dW), max(dS, dN)));
        float depthRange = (maxD - minD) / (maxD + 1e-4);

        if (depthRange > 0.02)
        {
            float edgeWeight = saturate(1.0 - (depthRange - 0.02) * 20.0);
            controlledEdit *= lerp(0.25, 1.0, edgeWeight);
        }
    }

    // Apply TransferStrength
    float3 scaledEdit = controlledEdit * gTransferStrength;

    // On skip frames, detect motion/edge transitions by comparing cached neural input with fresh native color.
    // When scene content moves, smoothly fade the stale delta so the pixel displays the clean 1:1 native game pixel!
    if (gIsSkipFrame != 0)
    {
        float origLuma = dot(max(original, 0.0), kLuma);
        float inLuma   = dot(max(smallInput, 0.0), kLuma);
        float diff     = abs(inLuma - origLuma);
        float weight   = saturate(1.0 - (diff * 2.5) / (origLuma + inLuma + 0.05));
        scaledEdit *= weight;
    }

    // Base native frame + scaled neural delta
    float3 result = max(original + scaledEdit, 0.0);

    // 4. HDR highlight & shadow guard using luminance ratio (only on evaluated frames to prevent stale luminance clamping)
    if (gIsSkipFrame == 0)
    {
        float origLuma = dot(max(original, 0.0), kLuma);
        float inLuma   = dot(max(smallInput, 0.0), kLuma);
        float outLuma  = dot(max(smallOutput, 0.0), kLuma);

        const float kFloor = 1.0 / 512.0;
        float lumaRatio = (outLuma + kFloor) / (inLuma + kFloor);

        float resLuma = dot(result, kLuma);
        if (resLuma > 1e-5 && inLuma > 1e-5)
        {
            float targetLuma = origLuma * lumaRatio;
            float maxAllowedLuma = max(origLuma * 2.5, targetLuma * 1.5 + 0.1);
            if (resLuma > maxAllowedLuma)
            {
                result *= (maxAllowedLuma / resLuma);
            }
        }
    }

    // 5. RCAS (Robust Contrast-Adaptive Sharpening) using on-chip LDS tile (zero global VRAM reads!)
    if (gSharpness > 0.001)
    {
        float3 cE = s_nativeTile[tid.y + 1][tid.x + 2];
        float3 cW = s_nativeTile[tid.y + 1][tid.x + 0];
        float3 cS = s_nativeTile[tid.y + 2][tid.x + 1];
        float3 cN = s_nativeTile[tid.y + 0][tid.x + 1];

        float lE = dot(cE, kLuma);
        float lW = dot(cW, kLuma);
        float lS = dot(cS, kLuma);
        float lN = dot(cN, kLuma);
        float lM = dot(max(original, 0.0), kLuma);

        float minL = min(lM, min(min(lE, lW), min(lS, lN)));
        float maxL = max(lM, max(max(lE, lW), max(lS, lN)));

        float range = maxL - minL;
        if (range > 1e-5)
        {
            float3 crossAvg = (cE + cW + cS + cN) * 0.25;
            float3 highFreq = original - crossAvg;
            float adaptiveScale = saturate(1.0 - range / (maxL + 1e-4));
            float rcasWeight = saturate(gSharpness) * (0.2 + 0.8 * adaptiveScale);
            result = max(result + highFreq * rcasWeight, 0.0);
        }
    }

    float nativeAlpha = gNativeColor.Load(int3(id.xy, 0)).a;
    gResolveTarget[id.xy] = float4(result, nativeAlpha);
}
