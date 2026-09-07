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

Texture2D<float4>   gDownSource : register(t0);
RWTexture2D<float4> gDownTarget : register(u0);

[numthreads(8, 8, 1)]
void CS_Downsample(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gDstWidth || id.y >= gDstHeight)
        return;

    const float x0 = ((float) id.x * (float) gSrcWidth) / (float) gDstWidth;
    const float x1 = ((float) (id.x + 1) * (float) gSrcWidth) / (float) gDstWidth;
    const float y0 = ((float) id.y * (float) gSrcHeight) / (float) gDstHeight;
    const float y1 = ((float) (id.y + 1) * (float) gSrcHeight) / (float) gDstHeight;
    const float area = max((x1 - x0) * (y1 - y0), 1e-6);

    const int i0 = (int) floor(x0);
    const int i1 = (int) ceil(x1) - 1;
    const int j0 = (int) floor(y0);
    const int j1 = (int) ceil(y1) - 1;

    float4 acc = float4(0, 0, 0, 0);

    for (int j = j0; j <= j1; ++j)
    {
        const int jj = clamp(j, 0, (int) gSrcHeight - 1);
        const float aY = max(y0, (float) j);
        const float bY = min(y1, (float) j + 1.0);
        const float wy = max(bY - aY, 0.0);

        for (int i = i0; i <= i1; ++i)
        {
            const int ii = clamp(i, 0, (int) gSrcWidth - 1);
            const float aX = max(x0, (float) i);
            const float bX = min(x1, (float) i + 1.0);
            acc += gDownSource.Load(int3(ii, jj, 0)) * (max(bX - aX, 0.0) * wy);
        }
    }

    gDownTarget[id.xy] = acc / area;
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
    uint  gHasMotionVectors;
    float gMvScaleX;
    float gMvScaleY;
};

Texture2D<float4>   gSmallInput    : register(t0); // Downsampled model input (g_colorSmall)
Texture2D<float4>   gSmallOutput   : register(t1); // Model output (g_outputSmall)
Texture2D<float4>   gNativeColor   : register(t2); // Pristine native frame (origColor)
Texture2D<float4>   gMotionVectors : register(t3); // Motion vectors buffer (mvecRes)
RWTexture2D<float4> gResolveTarget : register(u0); // Destination (origOutput)

SamplerState gLinear : register(s0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

[numthreads(8, 8, 1)]
void CS_Resolve(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gNativeWidth || id.y >= gNativeHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gNativeWidth, gNativeHeight);

    // Reproject UV coordinate along motion vectors on skip frames
    float2 sampleUV = uv;
    if (gIsSkipFrame != 0 && gHasMotionVectors != 0)
    {
        float4 mvSample = gMotionVectors.SampleLevel(gLinear, uv, 0);
        float2 velocity = mvSample.xy * float2(gMvScaleX, gMvScaleY);

        // Sanity check velocity magnitude (< 0.5 of screen dimensions) to reject camera cuts
        if (abs(velocity.x) < 0.5 && abs(velocity.y) < 0.5)
        {
            float2 reprojected = uv + velocity;
            if (reprojected.x >= 0.001 && reprojected.x <= 0.999 &&
                reprojected.y >= 0.001 && reprojected.y <= 0.999)
            {
                sampleUV = reprojected;
            }
        }
    }

    // Mode 0: Direct Neural Reconstruction with RCAS (Recommended for DLSS-NR / Ray Reconstruction)
    if (gEnlargementMode == 0)
    {
        float4 outSample = gSmallOutput.SampleLevel(gLinear, sampleUV, 0);
        float3 result = outSample.rgb;

        // On skip frames, check temporal confidence to prevent disocclusion flicker
        if (gIsSkipFrame != 0)
        {
            float4 nativeSample = gNativeColor.Load(int3(id.xy, 0));
            float3 inSample = gSmallInput.SampleLevel(gLinear, sampleUV, 0).rgb;
            float lumaIn = dot(max(inSample, 0.0), kLuma);
            float lumaNative = dot(max(nativeSample.rgb, 0.0), kLuma);
            float lumaDiff = abs(lumaIn - lumaNative);
            float confidence = saturate(1.0 - (lumaDiff * 2.0) / (lumaIn + lumaNative + 0.05));
            result = lerp(nativeSample.rgb, result, confidence * saturate(gTransferStrength));
        }
        else if (gTransferStrength < 0.999)
        {
            float4 nativeSample = gNativeColor.Load(int3(id.xy, 0));
            result = lerp(nativeSample.rgb, result, saturate(gTransferStrength));
        }

        // Contrast-adaptive edge sharpening (RCAS) on denoised features
        if (gSharpness > 0.001)
        {
            float2 px = float2(1.0 / (float)gNativeWidth, 1.0 / (float)gNativeHeight);
            float3 cE = gSmallOutput.SampleLevel(gLinear, sampleUV + float2( px.x, 0), 0).rgb;
            float3 cW = gSmallOutput.SampleLevel(gLinear, sampleUV + float2(-px.x, 0), 0).rgb;
            float3 cS = gSmallOutput.SampleLevel(gLinear, sampleUV + float2(0,  px.y), 0).rgb;
            float3 cN = gSmallOutput.SampleLevel(gLinear, sampleUV + float2(0, -px.y), 0).rgb;

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

        float4 nativeSample = gNativeColor.Load(int3(id.xy, 0));
        gResolveTarget[id.xy] = float4(max(result, 0.0), nativeSample.a);
        return;
    }

    // Mode 1: Matched Residual (1:1 Native Resolution Anchor + Scaled Neural Delta)
    // 1. Pristine 1:1 Native Game Pixel (preserves all geometry, subpixel edges, textures, text)
    float4 nativeSample = gNativeColor.Load(int3(id.xy, 0));
    float3 original = nativeSample.rgb;

    // 2. Sample neural input and output (reprojected along motion vectors on skip frames!)
    float3 smallInput = gSmallInput.SampleLevel(gLinear, sampleUV, 0).rgb;
    float3 smallOutput = gSmallOutput.SampleLevel(gLinear, sampleUV, 0).rgb;

    // 3. Compute neural delta / edit
    float3 edit = smallOutput - smallInput;

    // Chroma vs Luma control for ColorStrength
    float editLuma = dot(edit, kLuma);
    float3 editChroma = edit - editLuma;
    float3 controlledEdit = editLuma + editChroma * saturate(gColorStrength);

    // Apply TransferStrength
    float3 scaledEdit = controlledEdit * gTransferStrength;

    // On skip frames, calculate temporal confidence to prevent disocclusion / motion flicker
    if (gIsSkipFrame != 0)
    {
        float origLuma = dot(max(original, 0.0), kLuma);
        float inLuma   = dot(max(smallInput, 0.0), kLuma);
        float lumaDiff = abs(inLuma - origLuma);
        // Confidence smoothly drops from 1.0 down to 0.0 if the reprojected pixel brightness diverges
        float confidence = saturate(1.0 - (lumaDiff * 2.0) / (inLuma + origLuma + 0.05));
        scaledEdit *= confidence;
    }

    // Base native frame + scaled neural delta
    float3 result = max(original + scaledEdit, 0.0);

    // 4. HDR highlight & shadow guard using luminance ratio
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

    // 5. RCAS (Robust Contrast-Adaptive Sharpening) directly on the native pixel grid
    if (gSharpness > 0.001)
    {
        int2 coord = int2(id.xy);
        int w = (int)gNativeWidth - 1;
        int h = (int)gNativeHeight - 1;

        float3 cE = gNativeColor.Load(int3(min(coord.x + 1, w), coord.y, 0)).rgb;
        float3 cW = gNativeColor.Load(int3(max(coord.x - 1, 0), coord.y, 0)).rgb;
        float3 cS = gNativeColor.Load(int3(coord.x, min(coord.y + 1, h), 0)).rgb;
        float3 cN = gNativeColor.Load(int3(coord.x, max(coord.y - 1, 0), 0)).rgb;

        float lE = dot(cE, kLuma);
        float lW = dot(cW, kLuma);
        float lS = dot(cS, kLuma);
        float lN = dot(cN, kLuma);
        float lM = origLuma;

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

    gResolveTarget[id.xy] = float4(result, nativeSample.a);
}
