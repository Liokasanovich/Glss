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

// 高质量双线性插值 + 锐化模拟
float3 HighQualityBilinear(float2 uv, float2 texelSize) {
    float2 pixelPos = uv / texelSize;
    float2 pixelCenter = floor(pixelPos - 0.5f) + 0.5f;
    float2 subPixel = pixelPos - pixelCenter;
    
    // 双线性权重
    float2 w = subPixel;
    float2 w1 = 1.0f - w;
    
    float4 weights = float4(w1.x * w1.y, w.x * w1.y, w1.x * w.y, w.x * w.y);
    
    float2 baseUV = pixelCenter * texelSize;
    float3 samples[4] = {
        INPUT.SampleLevel(sam, baseUV, 0).rgb,
        INPUT.SampleLevel(sam, baseUV + float2(texelSize.x, 0), 0).rgb,
        INPUT.SampleLevel(sam, baseUV + float2(0, texelSize.y), 0).rgb,
        INPUT.SampleLevel(sam, baseUV + texelSize, 0).rgb
    };
    
    float3 bilinearResult = 0;
    [unroll]
    for (int i = 0; i < 4; i++) {
        bilinearResult += samples[i] * weights[i];
    }
    
    // 锐化模拟补偿
    float3 laplacian = 4.0f * bilinearResult - 
                      (samples[0] + samples[1] + samples[2] + samples[3]) * 0.25f;
    return bilinearResult + laplacian * (paramSharpness * 0.1f);
}

// 3x3邻域采样结构
struct Neighborhood3x3 {
    float3 samples[9];
    float luminances[9];
    float2 texelSize;
    float2 centerUV;
};

// 一次性获取3x3邻域
Neighborhood3x3 Get3x3Neighborhood(float2 uv, float2 texelSize) {
    Neighborhood3x3 n;
    n.texelSize = texelSize;
    n.centerUV = uv;
    
    static const float2 offsets[9] = {
        float2(-1, -1), float2(0, -1), float2(1, -1),
        float2(-1, 0),  float2(0, 0),  float2(1, 0),
        float2(-1, 1),  float2(0, 1),  float2(1, 1)
    };
    
    [unroll]
    for (int i = 0; i < 9; i++) {
        n.samples[i] = INPUT.SampleLevel(sam, uv + offsets[i] * texelSize, 0).rgb;
        n.luminances[i] = dot(n.samples[i], lumWeights);
    }
    
    return n;
}

// 共享采样计算梯度
float2 CalculateGradientFromNeighborhood(const Neighborhood3x3 n) {
    // Sobel X
    float gradX = (-n.luminances[0] - 2.0f * n.luminances[3] - n.luminances[6] + 
                   n.luminances[2] + 2.0f * n.luminances[5] + n.luminances[8]);
    
    // Sobel Y  
    float gradY = (-n.luminances[0] - 2.0f * n.luminances[1] - n.luminances[2] + 
                   n.luminances[6] + 2.0f * n.luminances[7] + n.luminances[8]);
    
    float2 gradient = float2(gradX, gradY);
    float gradLength = length(gradient);
    
    // 简化的振铃保护（clamp代替条件分支）
    return clamp(gradient, -0.15f, 0.15f);
}

// 共享采样计算拉普拉斯
float3 CalculateLaplacianFromNeighborhood(const Neighborhood3x3 n) {
    return 4.0f * n.samples[4] - (n.samples[1] + n.samples[3] + n.samples[5] + n.samples[7]);
}

// 共享采样计算高斯模糊
float3 CalculateGaussianFromNeighborhood(const Neighborhood3x3 n) {
    static const float weights[9] = {
        1.0f/16.0f, 2.0f/16.0f, 1.0f/16.0f,
        2.0f/16.0f, 4.0f/16.0f, 2.0f/16.0f,
        1.0f/16.0f, 2.0f/16.0f, 1.0f/16.0f
    };
    
    float3 result = 0;
    [unroll]
    for (int i = 0; i < 9; i++) {
        result += n.samples[i] * weights[i];
    }
    return result;
}

// 共享采样双边滤波
float3 CalculateBilateralFromNeighborhood(const Neighborhood3x3 n) {
    float centerLum = n.luminances[4];
    float noiseThreshold = GetNoiseThreshold();
    float sigmaColor = noiseThreshold * noiseThreshold * 2.0f;
    
    float3 filtered = n.samples[4];
    float totalWeight = 1.0f;
    
    // 只采样4个方向的对称点
    int indices[4] = {1, 3, 5, 7}; // 上下左右
    
    [unroll]
    for (int i = 0; i < 4; i++) {
        int idx = indices[i];
        float colorDiff = abs(n.luminances[idx] - centerLum);
        float colorWeight = exp(-colorDiff * colorDiff / sigmaColor);
        
        float spatialWeight = 0.2f;
        float weight = spatialWeight * colorWeight;
        
        filtered += n.samples[idx] * weight;
        totalWeight += weight;
    }
    
    return filtered / totalWeight;
}

// 噪声检测（基于邻域方差）
float DetectNoiseFromNeighborhood(const Neighborhood3x3 n) {
    float3 center = n.samples[4];
    float3 gaussian = CalculateGaussianFromNeighborhood(n);
    
    float3 noise = abs(center - gaussian);
    float noiseEnergy = length(noise);
    
    float2 gradient = CalculateGradientFromNeighborhood(n);
    float gradMagnitude = length(gradient);
    float edgeMask = saturate(gradMagnitude * 2.0f);
    
    float adjustedNoise = noiseEnergy * (1.0f - edgeMask * 0.2f);
    
    return saturate(adjustedNoise - GetNoiseThreshold()) / (1.0f - GetNoiseThreshold());
}

// 主像素着色器：优化版本
float4 Pass1(float2 pos) {
    const float2 inputPt = GetInputPt();
    
    // 一次性获取3x3邻域
    Neighborhood3x3 neighborhood = Get3x3Neighborhood(pos, inputPt);
    
    // 高质量插值
    float3 baseColor = HighQualityBilinear(pos, inputPt);
    
    // 从共享采样计算所有需要的值
    float2 gradient = CalculateGradientFromNeighborhood(neighborhood);
    float gradMagnitude = length(gradient);
    float2 edgeDirection = normalize(gradient);
    if (gradMagnitude < 0.001f) edgeDirection = float2(1, 0);
    
    float3 laplacian = CalculateLaplacianFromNeighborhood(neighborhood);
    float3 gaussian = CalculateGaussianFromNeighborhood(neighborhood);
    
    // 频率分离
    float3 lowFreq = gaussian;
    float3 midFreq = baseColor - lowFreq;
    float3 highFreq = laplacian;
    
    // 频率增强
    float3 enhanced = lowFreq * GetLowFreqBoost() + 
                     midFreq * GetMidFreqBoost() + 
                     highFreq * GetHighFreqBoost() * 0.3f; // 拉普拉斯已包含高频信息
    
    // 自适应去噪
    float noiseLevel = DetectNoiseFromNeighborhood(neighborhood);
    float3 denoised = enhanced;
    if (noiseLevel > 0.05f) {
        float3 bilateral = CalculateBilateralFromNeighborhood(neighborhood);
        float reductionStrength = GetSpatialNoiseReduction() * noiseLevel;
        denoised = lerp(enhanced, bilateral, reductionStrength);
    } else {
        denoised = enhanced;
    }
    
    // 抗锯齿（简化版本，基于梯度方向）
    float3 aaResult = denoised;
    if (gradMagnitude > GetEdgeThreshold()) {
        float3 edgeSamples[4] = {
            INPUT.SampleLevel(sam, pos + edgeDirection * inputPt * 0.5f, 0).rgb,
            INPUT.SampleLevel(sam, pos - edgeDirection * inputPt * 0.5f, 0).rgb,
            INPUT.SampleLevel(sam, pos + float2(-edgeDirection.y, edgeDirection.x) * inputPt * 0.5f, 0).rgb,
            INPUT.SampleLevel(sam, pos + float2(edgeDirection.y, -edgeDirection.x) * inputPt * 0.5f, 0).rgb
        };
        
        float3 aaAvg = (edgeSamples[0] + edgeSamples[1] + edgeSamples[2] + edgeSamples[3]) * 0.25f;
        aaResult = lerp(denoised, aaAvg, paramAAStrength * 0.6f);
    }
    
    // 自适应锐化
    float3 sharpened = aaResult + laplacian * paramSharpness * 0.15f;
    
    // 最终输出
    return float4(saturate(sharpened), 1.0);
}