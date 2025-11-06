//!MAGPIE EFFECT
//!VERSION 4

#include "StubDefs.hlsli"
//!PARAMETER
//!LABEL Sharpness
//!DEFAULT 1.8
//!MIN 0
//!MAX 3
//!STEP 0.01
float paramSharpness;

//!PARAMETER
//!LABEL Denoise
//!DEFAULT 0.4
//!MIN 0
//!MAX 1
//!STEP 0.01
float paramDenoise;

//!PARAMETER
//!LABEL AA Strength
//!DEFAULT 0.6
//!MIN 0
//!MAX 1
//!STEP 0.01
float paramAAStrength;

//!PARAMETER
//!LABEL Detail
//!DEFAULT 0.8
//!MIN 0
//!MAX 1
//!STEP 0.01
float paramDetail;

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
Texture2D OUTPUT;

//!SAMPLER
//!FILTER LINEAR
SamplerState sam;

//!PASS 1
//!STYLE PS
//!IN INPUT
//!OUT OUTPUT

// 预计算常量
static const float3 lumWeights = float3(0.299f, 0.587f, 0.114f);

// 从4个核心参数智能推导其他参数
float GetBParameter() {
    return 0.25f + paramDetail * 0.25f;
}

float GetCParameter() {
    return 0.5f - paramDetail * 0.3f;
}

float GetLowFreqBoost() {
    return 0.9f + paramDetail * 0.2f;
}

float GetMidFreqBoost() {
    return 1.2f + paramSharpness * 0.6f;
}

float GetHighFreqBoost() {
    return 2.0f + paramSharpness * 1.2f;
}

float GetEdgeThreshold() {
    return 0.02f + paramAAStrength * 0.02f;
}

float GetNoiseThreshold() {
    return 0.01f + paramDenoise * 0.02f;
}

float GetSpatialNoiseReduction() {
    return paramDenoise * 0.8f;
}

// 简化的Bicubic Weight Function
float weight(float x) {
    const float B = GetBParameter();
    const float C = GetCParameter();
    float ax = abs(x);

    if (ax < 1.0f) {
        return (x * x * ((12.0f - 9.0f * B - 6.0f * C) * ax + (-18.0f + 12.0f * B + 6.0f * C)) + (6.0f - 2.0f * B)) / 6.0f;
    }
    else if (ax >= 1.0f && ax < 2.0f) {
        return (x * x * ((-B - 6.0f * C) * ax + (6.0f * B + 30.0f * C)) + (-12.0f * B - 48.0f * C) * ax + (8.0f * B + 24.0f * C)) / 6.0f;
    }
    else {
        return 0.0f;
    }
}

float4 weight4(float x) {
    return float4(
        weight(x - 2.0f),
        weight(x - 1.0f),
        weight(x),
        weight(x + 1.0f)
    );
}

// 简化的3x3高斯核
static const float2 g_simpleGaussianOffsets[9] = {
    float2(-1, -1), float2(0, -1), float2(1, -1),
    float2(-1, 0), float2(0, 0), float2(1, 0),
    float2(-1, 1), float2(0, 1), float2(1, 1)
};

static const float g_simpleGaussianWeights[9] = {
    1.0f/16.0f, 2.0f/16.0f, 1.0f/16.0f,
    2.0f/16.0f, 4.0f/16.0f, 2.0f/16.0f,
    1.0f/16.0f, 2.0f/16.0f, 1.0f/16.0f
};

// 简化的高斯模糊 - 3x3核
float3 SimpleGaussianBlur(float2 uv, float2 texelSize, float scale = 1.0f) {
    float3 result = 0;
    float2 scaledOffset = texelSize * scale;
    
    [unroll]
    for (int i = 0; i < 9; i++) {
        float3 sample = INPUT.SampleLevel(sam, uv + g_simpleGaussianOffsets[i] * scaledOffset, 0).rgb;
        result += sample * g_simpleGaussianWeights[i];
    }
    
    return result;
}

// 简化的拉普拉斯滤波 - 3x3
float3 SimpleLaplacianFilter(float2 uv, float2 texelSize) {
    float3 center = INPUT.SampleLevel(sam, uv, 0).rgb;
    float3 up = INPUT.SampleLevel(sam, uv + float2(0, -texelSize.y), 0).rgb;
    float3 down = INPUT.SampleLevel(sam, uv + float2(0, texelSize.y), 0).rgb;
    float3 left = INPUT.SampleLevel(sam, uv + float2(-texelSize.x, 0), 0).rgb;
    float3 right = INPUT.SampleLevel(sam, uv + float2(texelSize.x, 0), 0).rgb;
    
    return 4.0f * center - (up + down + left + right);
}

// 简化的梯度计算 - 3x3 Sobel
float2 CalculateSimpleGradient(float2 uv, float2 texelSize) {
    float3 tl = INPUT.SampleLevel(sam, uv + float2(-texelSize.x, -texelSize.y), 0).rgb;
    float3 tm = INPUT.SampleLevel(sam, uv + float2(0, -texelSize.y), 0).rgb;
    float3 tr = INPUT.SampleLevel(sam, uv + float2(texelSize.x, -texelSize.y), 0).rgb;
    float3 ml = INPUT.SampleLevel(sam, uv + float2(-texelSize.x, 0), 0).rgb;
    float3 mr = INPUT.SampleLevel(sam, uv + float2(texelSize.x, 0), 0).rgb;
    float3 bl = INPUT.SampleLevel(sam, uv + float2(-texelSize.x, texelSize.y), 0).rgb;
    float3 bm = INPUT.SampleLevel(sam, uv + float2(0, texelSize.y), 0).rgb;
    float3 br = INPUT.SampleLevel(sam, uv + float2(texelSize.x, texelSize.y), 0).rgb;
    
    float tl_lum = dot(tl, lumWeights);
    float tm_lum = dot(tm, lumWeights);
    float tr_lum = dot(tr, lumWeights);
    float ml_lum = dot(ml, lumWeights);
    float mr_lum = dot(mr, lumWeights);
    float bl_lum = dot(bl, lumWeights);
    float bm_lum = dot(bm, lumWeights);
    float br_lum = dot(br, lumWeights);
    
    float gradX = (-tl_lum - 2.0f * ml_lum - bl_lum + tr_lum + 2.0f * mr_lum + br_lum);
    float gradY = (-tl_lum - 2.0f * tm_lum - tr_lum + bl_lum + 2.0f * bm_lum + br_lum);
    
    float2 gradient = float2(gradX, gradY);
    float gradLength = length(gradient);
    
    // 振铃保护
    if (gradLength > 0.15f) {
        gradient = normalize(gradient) * min(gradLength, 0.15f);
    }
    
    return gradient;
}

// 简化的双边滤波器 - 3x3核
float3 SimpleBilateralFilter(float2 uv, float2 texelSize) {
    float3 center = INPUT.SampleLevel(sam, uv, 0).rgb;
    float centerLum = dot(center, lumWeights);
    
    float3 filtered = center;
    float totalWeight = 1.0f;
    
    // 4个方向的邻近采样
    float2 offsets[4] = {float2(1, 0), float2(0, 1), float2(1, 1), float2(-1, 1)};
    
    [unroll]
    for (int i = 0; i < 4; i++) {
        float2 offset = offsets[i] * texelSize;
        float3 sample = INPUT.SampleLevel(sam, uv + offset, 0).rgb;
        float sampleLum = dot(sample, lumWeights);
        
        float colorDiff = abs(sampleLum - centerLum);
        float colorWeight = exp(-colorDiff * colorDiff / (2.0f * GetNoiseThreshold() * GetNoiseThreshold()));
        
        float weight = 0.2f * colorWeight;
        filtered += sample * weight;
        totalWeight += weight;
        
        // 对称采样
        float3 sample2 = INPUT.SampleLevel(sam, uv - offset, 0).rgb;
        float sample2Lum = dot(sample2, lumWeights);
        
        float colorDiff2 = abs(sample2Lum - centerLum);
        float colorWeight2 = exp(-colorDiff2 * colorDiff2 / (2.0f * GetNoiseThreshold() * GetNoiseThreshold()));
        
        float weight2 = 0.2f * colorWeight2;
        filtered += sample2 * weight2;
        totalWeight += weight2;
    }
    
    return filtered / totalWeight;
}

// 简化的噪声检测
float DetectNoise(float2 uv, float2 texelSize) {
    float3 center = INPUT.SampleLevel(sam, uv, 0).rgb;
    float3 blurred = SimpleGaussianBlur(uv, texelSize, 1.0f);
    
    float3 noise = abs(center - blurred);
    float noiseEnergy = length(noise);
    
    float2 gradient = CalculateSimpleGradient(uv, texelSize);
    float gradMagnitude = length(gradient);
    float edgeMask = saturate(gradMagnitude * 2.0f);
    
    float adjustedNoise = noiseEnergy * (1.0f - edgeMask * 0.2f);
    
    return saturate(adjustedNoise - GetNoiseThreshold()) / (1.0f - GetNoiseThreshold());
}

// 简化的自适应去噪
float3 SimpleAdaptiveDenoise(float2 uv, float2 texelSize, float3 color) {
    float noiseLevel = DetectNoise(uv, texelSize);
    
    if (noiseLevel > 0.05f) {
        float3 denoised = SimpleBilateralFilter(uv, texelSize);
        float reductionStrength = GetSpatialNoiseReduction() * noiseLevel;
        return lerp(color, denoised, reductionStrength);
    }
    
    return color;
}

// 简化的抗锯齿 - 4方向采样
float3 SimpleAA(float2 uv, float2 texelSize, float2 edgeDir) {
    float3 center = INPUT.SampleLevel(sam, uv, 0).rgb;
    
    // 4方向采样
    float2 directions[4] = {
        float2(edgeDir.x, edgeDir.y),
        float2(-edgeDir.x, -edgeDir.y),
        float2(-edgeDir.y, edgeDir.x),
        float2(edgeDir.y, -edgeDir.x)
    };
    
    float3 samples[4];
    [unroll]
    for (int i = 0; i < 4; i++) {
        samples[i] = INPUT.SampleLevel(sam, uv + directions[i] * texelSize * 0.5f, 0).rgb;
    }
    
    float3 aaResult = (samples[0] + samples[1] + samples[2] + samples[3]) * 0.25f;
    
    return lerp(center, aaResult, paramAAStrength * 0.6f);
}

// 简化的自适应锐化
float3 SimpleAdaptiveSharpen(float2 uv, float2 texelSize, float3 baseColor) {
    float3 laplacian = SimpleLaplacianFilter(uv, texelSize);
    float3 sharpened = baseColor + laplacian * paramSharpness * 0.3f;
    
    // 振铃控制
    float3 diff = sharpened - baseColor;
    float maxSharpen = 0.1f;
    float3 clampedDiff = clamp(diff, -maxSharpen, maxSharpen);
    
    return clamp(clampedDiff + baseColor, 0, 1);
}

// 主像素着色器：简化版本
float4 Pass1(float2 pos) {
    const float2 inputPt = GetInputPt();
    const float2 inputSize = GetInputSize();

    // 双三次插值（保持原有逻辑）
    float2 pixelPos = pos * inputSize;
    float2 pixelCenter = floor(pixelPos - 0.5f) + 0.5f;
    float2 subPixel = pixelPos - pixelCenter;

    float4 rowWeights = weight4(1.0f - subPixel.x);
    float4 colWeights = weight4(1.0f - subPixel.y);

    rowWeights /= dot(rowWeights, 1.0f);
    colWeights /= dot(colWeights, 1.0f);

    float2 baseUV = pixelCenter * inputPt;
    float3 samples[4][4];
    [unroll]
    for (int j = 0; j < 4; j++) {
        [unroll]
        for (int i = 0; i < 4; i++) {
            float2 offset = float2((i - 1) * inputPt.x, (j - 1) * inputPt.y);
            samples[j][i] = INPUT.SampleLevel(sam, baseUV + offset, 0).rgb;
        }
    }

    float3 bicubicResult = 0;
    [unroll]
    for (int j = 0; j < 4; j++) {
        float3 rowResult = 0;
        [unroll]
        for (int i = 0; i < 4; i++) {
            rowResult += samples[j][i] * rowWeights[i];
        }
        bicubicResult += rowResult * colWeights[j];
    }

    // 简化的梯度计算
    float2 simpleGradient = CalculateSimpleGradient(pos, inputPt);
    float gradientMagnitude = length(simpleGradient);
    float2 edgeDirection = normalize(simpleGradient);
    if (gradientMagnitude < 0.001f) edgeDirection = float2(1, 0);
    
    // 简化的频率增强
    float3 lowFreq = SimpleGaussianBlur(pos, inputPt, 1.5f);
    float3 midFreq = SimpleGaussianBlur(pos, inputPt, 0.8f) - lowFreq;
    float3 highFreq = bicubicResult - SimpleGaussianBlur(pos, inputPt, 0.5f);
    
    float3 enhanced = lowFreq * GetLowFreqBoost() * 0.9f + 
                     midFreq * GetMidFreqBoost() * 1.0f + 
                     highFreq * GetHighFreqBoost() * 1.2f;
    
    // 简化的自适应去噪
    float3 denoised = SimpleAdaptiveDenoise(pos, inputPt, enhanced);

    // 简化的抗锯齿
    float3 aaResult = SimpleAA(pos, inputPt, edgeDirection);
    float3 antiAliased = lerp(denoised, aaResult, saturate(gradientMagnitude * 3.0f) * paramAAStrength);

    // 简化的自适应锐化
    float3 sharpened = SimpleAdaptiveSharpen(pos, inputPt, antiAliased);
    
    // 最终输出
    float3 finalColor = sharpened;
    
    return float4(saturate(finalColor), 1.0);
}