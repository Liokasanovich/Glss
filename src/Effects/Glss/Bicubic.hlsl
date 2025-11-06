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

//!PARAMETER
//!LABEL Softness
//!DEFAULT 0.3
//!MIN 0
//!MAX 1
//!STEP 0.01
float paramSoftness;

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

// 改进的Bicubic Weight Function
float weight(float x) {
    const float B = 0.33f + paramDetail * 0.17f;
    const float C = 0.33f - paramDetail * 0.17f;
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

float GetAAQuality() {
    return 0.7f + paramAAStrength * 0.2f;
}

float GetMorphAA() {
    return 0.6f + paramAAStrength * 0.3f;
}

float GetNoiseThreshold() {
    return 0.01f + paramDenoise * 0.02f;
}

float GetSpatialNoiseReduction() {
    return paramDenoise * 0.8f;
}

// 高斯核大小定义
static const int GAUSSIAN_KERNEL_SIZE = 7;
static const int GAUSSIAN_RADIUS = (GAUSSIAN_KERNEL_SIZE - 1) / 2;

// 预计算的高斯权重（sigma = 1.0）
static const float g_gaussianWeights[49] = {
    0.0003f, 0.0013f, 0.0044f, 0.0095f, 0.0044f, 0.0013f, 0.0003f,
    0.0013f, 0.0060f, 0.0187f, 0.0383f, 0.0187f, 0.0060f, 0.0013f,
    0.0044f, 0.0187f, 0.0571f, 0.1102f, 0.0571f, 0.0187f, 0.0044f,
    0.0095f, 0.0383f, 0.1102f, 0.2106f, 0.1102f, 0.0383f, 0.0095f,
    0.0044f, 0.0187f, 0.0571f, 0.1102f, 0.0571f, 0.0187f, 0.0044f,
    0.0013f, 0.0060f, 0.0187f, 0.0383f, 0.0187f, 0.0060f, 0.0013f,
    0.0003f, 0.0013f, 0.0044f, 0.0095f, 0.0044f, 0.0013f, 0.0003f
};

static const float2 g_gaussianOffsets[49] = {
    float2(-3, -3), float2(-2, -3), float2(-1, -3), float2(0, -3), float2(1, -3), float2(2, -3), float2(3, -3),
    float2(-3, -2), float2(-2, -2), float2(-1, -2), float2(0, -2), float2(1, -2), float2(2, -2), float2(3, -2),
    float2(-3, -1), float2(-2, -1), float2(-1, -1), float2(0, -1), float2(1, -1), float2(2, -1), float2(3, -1),
    float2(-3, 0), float2(-2, 0), float2(-1, 0), float2(0, 0), float2(1, 0), float2(2, 0), float2(3, 0),
    float2(-3, 1), float2(-2, 1), float2(-1, 1), float2(0, 1), float2(1, 1), float2(2, 1), float2(3, 1),
    float2(-3, 2), float2(-2, 2), float2(-1, 2), float2(0, 2), float2(1, 2), float2(2, 2), float2(3, 2),
    float2(-3, 3), float2(-2, 3), float2(-1, 3), float2(0, 3), float2(1, 3), float2(2, 3), float2(3, 3)
};

// 7x7 Sobel算子
static const float g_sobelX[49] = {
    -1, -4, -5, -6, -5, -4, -1,
    -4, -16, -20, -24, -20, -16, -4,
    -5, -20, -25, -30, -25, -20, -5,
    -6, -24, -30, 0, -30, -24, -6,
    -5, -20, -25, -30, -25, -20, -5,
    -4, -16, -20, -24, -20, -16, -4,
    -1, -4, -5, -6, -5, -4, -1
};

static const float g_sobelY[49] = {
    -1, -4, -5, -6, -5, -4, -1,
    -4, -16, -20, -24, -20, -16, -4,
    -5, -20, -25, -30, -25, -20, -5,
    -6, -24, -30, 0, -30, -24, -6,
    -5, -20, -25, -30, -25, -20, -5,
    -4, -16, -20, -24, -20, -16, -4,
    -1, -4, -5, -6, -5, -4, -1
};

// 7x7 Scharr算子
static const float g_scharrX[49] = {
    -1, -4, -5, 0, 5, 4, 1,
    -6, -24, -30, 0, 30, 24, 6,
    -15, -60, -75, 0, 75, 60, 15,
    -20, -80, -100, 0, 100, 80, 20,
    -15, -60, -75, 0, 75, 60, 15,
    -6, -24, -30, 0, 30, 24, 6,
    -1, -4, -5, 0, 5, 4, 1
};

static const float g_scharrY[49] = {
    -1, -6, -15, -20, -15, -6, -1,
    -4, -24, -60, -80, -60, -24, -4,
    -5, -30, -75, -100, -75, -30, -5,
    0, 0, 0, 0, 0, 0, 0,
    5, 30, 75, 100, 75, 30, 5,
    4, 24, 60, 80, 60, 24, 4,
    1, 6, 15, 20, 15, 6, 1
};

// 完整的拉普拉斯滤波 - 7x7核
static const float g_laplacianKernel7x7[49] = {
    -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, 48, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1
};

static const float2 g_laplacianOffsets7x7[49] = {
    float2(-3, -3), float2(-2, -3), float2(-1, -3), float2(0, -3), float2(1, -3), float2(2, -3), float2(3, -3),
    float2(-3, -2), float2(-2, -2), float2(-1, -2), float2(0, -2), float2(1, -2), float2(2, -2), float2(3, -2),
    float2(-3, -1), float2(-2, -1), float2(-1, -1), float2(0, -1), float2(1, -1), float2(2, -1), float2(3, -1),
    float2(-3, 0), float2(-2, 0), float2(-1, 0), float2(0, 0), float2(1, 0), float2(2, 0), float2(3, 0),
    float2(-3, 1), float2(-2, 1), float2(-1, 1), float2(0, 1), float2(1, 1), float2(2, 1), float2(3, 1),
    float2(-3, 2), float2(-2, 2), float2(-1, 2), float2(0, 2), float2(1, 2), float2(2, 2), float2(3, 2),
    float2(-3, 3), float2(-2, 3), float2(-1, 3), float2(0, 3), float2(1, 3), float2(2, 3), float2(3, 3)
};

// 改进的梯度计算 - 使用7x7 Sobel + Scharr + 高斯平滑
struct ImprovedGradientResult {
    float2 gradient;
    float magnitude;
    float direction;
    float coherence;  // 梯度一致性
    float complexity; // 纹理复杂度
    float textureEnergy; // 纹理能量
    float edgeStrength;  // 边缘强度
    float confidence;    // 梯度置信度
    float2 directionField; // 局部梯度方向场
    float smoothness;    // 平滑度（用于低频掩码）
};

// 前置声明
ImprovedGradientResult CalculateImprovedGradient(float2 uv, float2 texelSize);

// 高斯平滑 + 梯度 (SoG)
float3 GaussianSmooth(float2 uv, float2 texelSize, float sigma) {
    float3 result = 0;
    float totalWeight = 0;
    
    for (int idx_loop = 0; idx_loop < 49; idx_loop++) {
        float2 offset = g_gaussianOffsets[idx_loop] * texelSize;
        float3 sample = INPUT.SampleLevel(sam, uv + offset, 0).rgb;
        float weight = exp(-(g_gaussianOffsets[idx_loop].x * g_gaussianOffsets[idx_loop].x + g_gaussianOffsets[idx_loop].y * g_gaussianOffsets[idx_loop].y) / (2.0f * sigma * sigma));
        result += sample * weight;
        totalWeight += weight;
    }
    
    return result / totalWeight;
}

// 7x7梯度计算
float3 Gradient7x7(float2 uv, float2 texelSize, const float kernel[49]) {
    float3 result = 0;
    
    for (int idx_loop = 0; idx_loop < 49; idx_loop++) {
        float2 offset = g_gaussianOffsets[idx_loop] * texelSize;
        float3 sample = INPUT.SampleLevel(sam, uv + offset, 0).rgb;
        result += sample * kernel[idx_loop];
    }
    
    return result / 120.0f; // 归一化系数
}

// 方向一致性计算 - 使用简化的梯度计算，避免递归
float2 ComputeDirectionField(float2 uv, float2 texelSize, float2 gradient) {
    if (length(gradient) < 0.001f) {
        return float2(1, 0);
    }
    
    float2 dir = normalize(gradient);
    
    // 局部邻域方向平滑 - 使用简化的3x3 Sobel计算邻域梯度
    float2 avgDir = 0;
    float count = 0;
    
    // 使用简化的3x3 Sobel算子进行邻域梯度计算
    static const float g_sobelX_3x3[9] = {
        -1, 0, 1,
        -2, 0, 2,
        -1, 0, 1
    };
    static const float g_sobelY_3x3[9] = {
        -1, -2, -1,
        0, 0, 0,
        1, 2, 1
    };
    static const float2 g_offsets_3x3[9] = {
        float2(-1, -1), float2(0, -1), float2(1, -1),
        float2(-1, 0), float2(0, 0), float2(1, 0),
        float2(-1, 1), float2(0, 1), float2(1, 1)
    };
    
    for (int dy_loop = -1; dy_loop <= 1; dy_loop++) {
        for (int dx_loop = -1; dx_loop <= 1; dx_loop++) {
            if (dx_loop == 0 && dy_loop == 0) continue;
            
            float2 offset = float2(dx_loop, dy_loop) * texelSize * 2.0f;
            float2 neighborUV = uv + offset;
            
            // 计算邻域点的简化梯度
            float3 gradX = 0, gradY = 0;
            for (int i_loop = 0; i_loop < 9; i_loop++) {
                float2 sampleOffset = neighborUV + g_offsets_3x3[i_loop] * texelSize;
                float3 sample = INPUT.SampleLevel(sam, sampleOffset, 0).rgb;
                gradX += sample * g_sobelX_3x3[i_loop];
                gradY += sample * g_sobelY_3x3[i_loop];
            }
            gradX /= 4.0f;
            gradY /= 4.0f;
            
            float gradX_lum = dot(gradX, lumWeights);
            float gradY_lum = dot(gradY, lumWeights);
            float2 neighborGrad = float2(gradX_lum, gradY_lum);
            
            if (length(neighborGrad) > 0.01f) {
                avgDir += normalize(neighborGrad);
                count += 1.0f;
            }
        }
    }
    
    if (count > 0) {
        avgDir = normalize(avgDir / count);
        // 混合原始方向和平均方向
        return lerp(dir, avgDir, 0.3f);
    }
    
    return dir;
}

ImprovedGradientResult CalculateImprovedGradient(float2 uv, float2 texelSize) {
    ImprovedGradientResult result;
    
    // 使用SoG (Smooth of Gradient) 方法 - 先高斯平滑再求梯度
    float3 smoothed = GaussianSmooth(uv, texelSize, 1.0f);
    
    // 计算7x7 Sobel梯度
    float3 sobelX = 0, sobelY = 0;
    for (int idx_loop = 0; idx_loop < 49; idx_loop++) {
        float2 offset = g_gaussianOffsets[idx_loop] * texelSize;
        float3 sample = INPUT.SampleLevel(sam, uv + offset, 0).rgb;
        sobelX += sample * g_sobelX[idx_loop];
        sobelY += sample * g_sobelY[idx_loop];
    }
    sobelX /= 120.0f;
    sobelY /= 120.0f;
    
    // 计算7x7 Scharr梯度
    float3 scharrX = 0, scharrY = 0;
    for (int idx_loop = 0; idx_loop < 49; idx_loop++) {
        float2 offset = g_gaussianOffsets[idx_loop] * texelSize;
        float3 sample = INPUT.SampleLevel(sam, uv + offset, 0).rgb;
        scharrX += sample * g_scharrX[idx_loop];
        scharrY += sample * g_scharrY[idx_loop];
    }
    scharrX /= 480.0f;
    scharrY /= 480.0f;
    
    // 混合Sobel和Scharr结果以获得更平滑的梯度
    float3 gradX = lerp(sobelX, scharrX, 0.5f);
    float3 gradY = lerp(sobelY, scharrY, 0.5f);
    
    // 转换为亮度梯度
    float gradX_lum = dot(gradX, lumWeights);
    float gradY_lum = dot(gradY, lumWeights);
    
    result.gradient = float2(gradX_lum, gradY_lum);
    result.magnitude = length(result.gradient);
    result.direction = atan2(result.gradient.y, result.gradient.x);
    
    // 计算方向场（用于后续方向性处理）
    result.directionField = ComputeDirectionField(uv, texelSize, result.gradient);
    
    // 计算梯度一致性 - 用于区分边缘和纹理
    float2 orthoDir = float2(-result.gradient.y, result.gradient.x);
    if (result.magnitude > 0.001f) {
        orthoDir = normalize(orthoDir);
    } else {
        orthoDir = float2(1, 0);
    }
    
    // 计算纹理复杂度 - 使用7x7邻域方差
    float variance = 0;
    float centerLum = dot(INPUT.SampleLevel(sam, uv, 0).rgb, lumWeights);
    for (int y_idx_loop = -3; y_idx_loop <= 3; y_idx_loop++) {
        for (int x_idx_loop = -3; x_idx_loop <= 3; x_idx_loop++) {
            float2 offset = float2(x_idx_loop, y_idx_loop) * texelSize;
            float lum = dot(INPUT.SampleLevel(sam, uv + offset, 0).rgb, lumWeights);
            variance += (lum - centerLum) * (lum - centerLum);
        }
    }
    variance /= 49.0f;
    result.complexity = sqrt(variance);
    
    // 计算纹理能量
    float textureEnergy = 0;
    for (int y_idx_loop = -3; y_idx_loop <= 3; y_idx_loop++) {
        for (int x_idx_loop = -3; x_idx_loop <= 3; x_idx_loop++) {
            float2 offset = float2(x_idx_loop, y_idx_loop) * texelSize;
            float lum = dot(INPUT.SampleLevel(sam, uv + offset, 0).rgb, lumWeights);
            textureEnergy += abs(lum - centerLum);
        }
    }
    result.textureEnergy = textureEnergy / 49.0f;
    
    // 计算边缘强度
    result.edgeStrength = max(abs(gradX_lum), abs(gradY_lum));
    
    // 计算梯度一致性
    float orthoChange = 0;
    float alongChange = 0;
    float3 center = INPUT.SampleLevel(sam, uv, 0).rgb;
    
    // 沿梯度方向的变化
    float3 along1 = INPUT.SampleLevel(sam, uv + result.directionField * texelSize * 3.0f, 0).rgb;
    float3 along2 = INPUT.SampleLevel(sam, uv - result.directionField * texelSize * 3.0f, 0).rgb;
    alongChange = abs(dot(along1 - center, lumWeights)) + abs(dot(along2 - center, lumWeights));
    
    // 正交方向的变化
    float3 ortho1 = INPUT.SampleLevel(sam, uv + float2(-result.directionField.y, result.directionField.x) * texelSize * 3.0f, 0).rgb;
    float3 ortho2 = INPUT.SampleLevel(sam, uv - float2(-result.directionField.y, result.directionField.x) * texelSize * 3.0f, 0).rgb;
    orthoChange = abs(dot(ortho1 - center, lumWeights)) + abs(dot(ortho2 - center, lumWeights));
    
    result.coherence = saturate(orthoChange / (alongChange + 0.001f));
    
    // 梯度置信度 - 基于幅度、一致性和复杂度
    result.confidence = saturate(result.magnitude * 5.0f) * result.coherence * (1.0f - saturate(result.complexity * 2.0f));
    
    // 计算平滑度 - 用于低频掩码
    result.smoothness = 1.0f - saturate(result.magnitude * 10.0f);
    
    return result;
}

// 自适应高斯模糊 - 根据纹理特征调整模糊强度
float3 AdaptiveGaussianBlur(float2 uv, float2 texelSize, ImprovedGradientResult grad, float baseSigma = 1.0f) {
    // 根据纹理复杂度和边缘强度调整模糊强度
    float adaptiveSigma = baseSigma;
    
    // 纹理复杂度高时增加模糊（去噪），边缘强度高时减少模糊（保持边缘）
    adaptiveSigma = lerp(adaptiveSigma * 0.5f, adaptiveSigma * 2.0f, grad.complexity);
    adaptiveSigma = lerp(adaptiveSigma * 1.5f, adaptiveSigma * 0.3f, saturate(grad.edgeStrength * 5.0f));
    
    // 使用梯度置信度进一步调整
    adaptiveSigma = lerp(adaptiveSigma * 1.2f, adaptiveSigma * 0.8f, grad.confidence);
    
    // 确保sigma在合理范围内
    adaptiveSigma = clamp(adaptiveSigma, 0.5f, 3.0f);
    
    float3 result = 0;
    float totalWeight = 0;
    
    // 使用预计算的7x7高斯核
    for (int idx_loop = 0; idx_loop < 49; idx_loop++) {
        float2 offset = g_gaussianOffsets[idx_loop] * texelSize * (adaptiveSigma / baseSigma);
        float3 sample = INPUT.SampleLevel(sam, uv + offset, 0).rgb;
        float weight = g_gaussianWeights[idx_loop];
        result += sample * weight;
        totalWeight += weight;
    }
    
    return result / totalWeight;
}

// 强高斯模糊 - 用于纯净低频层
float3 StrongGaussianBlur(float2 uv, float2 texelSize, float sigma = 2.5f) {
    float3 result = 0;
    float totalWeight = 0;
    
    for (int idx_loop = 0; idx_loop < 49; idx_loop++) {
        float2 offset = g_gaussianOffsets[idx_loop] * texelSize;
        float3 sample = INPUT.SampleLevel(sam, uv + offset, 0).rgb;
        float weight = exp(-(g_gaussianOffsets[idx_loop].x * g_gaussianOffsets[idx_loop].x + g_gaussianOffsets[idx_loop].y * g_gaussianOffsets[idx_loop].y) / (2.0f * sigma * sigma));
        result += sample * weight;
        totalWeight += weight;
    }
    
    return result / totalWeight;
}

float3 FullLaplacianFilter7x7(float2 uv, float2 texelSize) {
    float3 result = 0;
    
    for (int idx_loop = 0; idx_loop < 49; idx_loop++) {
        float2 offset = g_laplacianOffsets7x7[idx_loop] * texelSize;
        float3 sample = INPUT.SampleLevel(sam, uv + offset, 0).rgb;
        float weight = g_laplacianKernel7x7[idx_loop];
        result += sample * weight;
    }
    
    return result / 48.0f; // 归一化
}

// 拉普拉斯-高斯金字塔结构 - 增加更多层级
struct ExtendedLaplacianPyramid {
    float3 level0; // 原始图像
    float3 level1; // 第1层低频 (高斯模糊)
    float3 level2; // 第2层低频
    float3 level3; // 第3层低频
    float3 level4; // 第4层低频
    float3 level5; // 第5层低频
    float3 level6; // 第6层低频
    float3 detail1; // 第1层细节 (level0 - level1)
    float3 detail2; // 第2层细节 (level1 - level2)
    float3 detail3; // 第3层细节 (level2 - level3)
    float3 detail4; // 第4层细节 (level3 - level4)
    float3 detail5; // 第5层细节 (level4 - level5)
    float3 detail6; // 第6层细节 (level5 - level6)
};

ExtendedLaplacianPyramid CreateExtendedLaplacianPyramid(float2 uv, float2 texelSize, ImprovedGradientResult grad) {
    ExtendedLaplacianPyramid pyramid;
    
    pyramid.level0 = INPUT.SampleLevel(sam, uv, 0).rgb;
    
    // 使用自适应多尺度高斯模糊
    pyramid.level1 = AdaptiveGaussianBlur(uv, texelSize, grad, 0.5f);
    pyramid.level2 = AdaptiveGaussianBlur(uv, texelSize, grad, 0.8f);
    pyramid.level3 = AdaptiveGaussianBlur(uv, texelSize, grad, 1.2f);
    pyramid.level4 = AdaptiveGaussianBlur(uv, texelSize, grad, 1.8f);
    pyramid.level5 = AdaptiveGaussianBlur(uv, texelSize, grad, 2.5f);
    pyramid.level6 = AdaptiveGaussianBlur(uv, texelSize, grad, 3.2f);
    
    // 对所有低频层使用强高斯模糊重滤波，确保其"纯净"
    pyramid.level1 = StrongGaussianBlur(uv, texelSize, 2.5f);
    pyramid.level2 = StrongGaussianBlur(uv, texelSize, 2.5f);
    pyramid.level3 = StrongGaussianBlur(uv, texelSize, 2.5f);
    pyramid.level4 = StrongGaussianBlur(uv, texelSize, 2.5f);
    pyramid.level5 = StrongGaussianBlur(uv, texelSize, 2.5f);
    pyramid.level6 = StrongGaussianBlur(uv, texelSize, 2.5f);
    
    // 重新计算各层细节（基于纯净的低频层）
    pyramid.detail1 = pyramid.level0 - pyramid.level1;
    pyramid.detail2 = pyramid.level1 - pyramid.level2;
    pyramid.detail3 = pyramid.level2 - pyramid.level3;
    pyramid.detail4 = pyramid.level3 - pyramid.level4;
    pyramid.detail5 = pyramid.level4 - pyramid.level5;
    pyramid.detail6 = pyramid.level5 - pyramid.level6;
    
    return pyramid;
}

// 改进的多尺度拉普拉斯
struct ImprovedMultiScaleLaplacian {
    float3 ultraFineDetails; // 超细细节 (3x3)
    float3 fineDetails;      // 细节 (5x5)
    float3 mediumDetails;    // 中等细节 (7x7)
    float3 coarseDetails;    // 粗略细节 (9x9)
    float3 ultraCoarseDetails; // 超粗略细节 (11x11)
};

ImprovedMultiScaleLaplacian CalculateImprovedMultiScaleLaplacian(float2 uv, float2 texelSize) {
    ImprovedMultiScaleLaplacian result;
    
    float3 center = INPUT.SampleLevel(sam, uv, 0).rgb;
    
    // 3x3 拉普拉斯（超细细节）
    float3 up = INPUT.SampleLevel(sam, uv + float2(0, -texelSize.y), 0).rgb;
    float3 down = INPUT.SampleLevel(sam, uv + float2(0, texelSize.y), 0).rgb;
    float3 left = INPUT.SampleLevel(sam, uv + float2(-texelSize.x, 0), 0).rgb;
    float3 right = INPUT.SampleLevel(sam, uv + float2(texelSize.x, 0), 0).rgb;
    result.ultraFineDetails = 4.0f * center - (up + down + left + right);
    
    // 5x5 拉普拉斯（细节）
    static const float g_laplacian5x5[25] = {
        -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1,
        -1, -1, 24, -1, -1,
        -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1
    };
    static const float2 g_offsets5x5[25] = {
        float2(-2, -2), float2(-1, -2), float2(0, -2), float2(1, -2), float2(2, -2),
        float2(-2, -1), float2(-1, -1), float2(0, -1), float2(1, -1), float2(2, -1),
        float2(-2, 0), float2(-1, 0), float2(0, 0), float2(1, 0), float2(2, 0),
        float2(-2, 1), float2(-1, 1), float2(0, 1), float2(1, 1), float2(2, 1),
        float2(-2, 2), float2(-1, 2), float2(0, 2), float2(1, 2), float2(2, 2)
    };
    
    float3 lap5x5 = 0;
    for (int idx_loop = 0; idx_loop < 25; idx_loop++) {
        float3 sample = INPUT.SampleLevel(sam, uv + g_offsets5x5[idx_loop] * texelSize, 0).rgb;
        lap5x5 += sample * g_laplacian5x5[idx_loop];
    }
    result.fineDetails = lap5x5 / 24.0f;
    
    // 7x7 拉普拉斯（中等细节）
    result.mediumDetails = FullLaplacianFilter7x7(uv, texelSize);
    
    // 9x9 高斯差分（粗略细节）
    float3 blur9 = AdaptiveGaussianBlur(uv, texelSize, CalculateImprovedGradient(uv, texelSize), 1.8f);
    float3 blur7 = AdaptiveGaussianBlur(uv, texelSize, CalculateImprovedGradient(uv, texelSize), 1.2f);
    result.coarseDetails = blur7 - blur9;
    
    // 11x11 高斯差分（超粗略细节）
    float3 blur11 = AdaptiveGaussianBlur(uv, texelSize, CalculateImprovedGradient(uv, texelSize), 2.5f);
    result.ultraCoarseDetails = blur9 - blur11;
    
    return result;
}

// 改进的频率自适应融合
float3 ImprovedAdaptiveFrequencyFusion(ExtendedLaplacianPyramid pyramid, 
                                      float2 uv, float2 texelSize, ImprovedGradientResult grad) {
    // 创建低频掩码 - 在平滑区域为1，边缘纹理区域为0
    float lowFreqMask = grad.smoothness;
    
    // 根据梯度特征调整不同层级的权重
    float detail1Weight, detail2Weight, detail3Weight, detail4Weight, detail5Weight, detail6Weight;
    
    // 根据纹理复杂度和边缘强度调整细节权重
    float textureFactor = lerp(0.5f, 1.5f, grad.complexity);
    float edgeFactor = lerp(0.3f, 1.2f, grad.edgeStrength);
    float confidenceFactor = lerp(0.7f, 1.3f, grad.confidence);
    
    // 高频细节（超细和细）权重 - 在平滑区域置零
    detail1Weight = GetHighFreqBoost() * textureFactor * 0.4f * confidenceFactor;
    detail2Weight = GetHighFreqBoost() * textureFactor * 0.3f * confidenceFactor;
    
    // 在平滑区域将高频细节权重置零
    detail1Weight = lerp(detail1Weight, 0.0f, lowFreqMask);
    detail2Weight = lerp(detail2Weight, 0.0f, lowFreqMask);
    
    // 中频细节权重
    detail3Weight = GetMidFreqBoost() * edgeFactor * 0.5f;
    
    // 低频细节权重
    detail4Weight = GetLowFreqBoost() * 0.3f;
    detail5Weight = GetLowFreqBoost() * 0.2f;
    detail6Weight = GetLowFreqBoost() * 0.1f;
    
    // 基于梯度一致性的调整
    float consistencyFactor = lerp(0.7f, 1.3f, grad.coherence);
    
    // 频率融合
    float3 enhanced = 
        pyramid.level6 + // 保留最底层的低频信息
        pyramid.detail1 * detail1Weight * consistencyFactor +
        pyramid.detail2 * detail2Weight * consistencyFactor +
        pyramid.detail3 * detail3Weight * edgeFactor +
        pyramid.detail4 * detail4Weight +
        pyramid.detail5 * detail5Weight +
        pyramid.detail6 * detail6Weight;

    return enhanced;
}

// 改进的自适应锐化 - 基于拉普拉斯锐化优化边缘
float3 ImprovedAdaptiveSharpen(float2 uv, float2 texelSize, float3 baseColor, 
                              ImprovedGradientResult grad) {
    float3 center = INPUT.SampleLevel(sam, uv, 0).rgb;
    
    // 改进的多尺度锐化
    ImprovedMultiScaleLaplacian laplacian = CalculateImprovedMultiScaleLaplacian(uv, texelSize);
    
    // 根据梯度特征调整锐化强度
    float adaptiveSharp = paramSharpness;
    
    // 根据梯度置信度、边缘强度和纹理复杂度调整锐化
    adaptiveSharp = lerp(adaptiveSharp * 0.3f, adaptiveSharp, grad.confidence);
    adaptiveSharp = lerp(adaptiveSharp, adaptiveSharp * 0.5f, grad.complexity); // 纹理区域减少锐化
    adaptiveSharp = lerp(adaptiveSharp, adaptiveSharp * 1.5f, grad.edgeStrength); // 边缘区域增强锐化
    
    // 根据梯度一致性进一步调整
    adaptiveSharp = lerp(adaptiveSharp * 0.5f, adaptiveSharp, grad.coherence);
    
    // 拉普拉斯锐化优化边缘
    float3 laplacianSharpen = center + 
                             laplacian.mediumDetails * adaptiveSharp * 0.3f +
                             laplacian.fineDetails * adaptiveSharp * 0.4f +
                             laplacian.ultraFineDetails * adaptiveSharp * 0.3f;
    
    // 振铃控制
    float3 diff = laplacianSharpen - center;
    float maxSharpen = 0.15f + grad.confidence * 0.1f; // 高置信度区域允许更多锐化
    float3 clampedDiff = clamp(diff, -maxSharpen, maxSharpen);
    
    return clamp(clampedDiff + center, 0, 1);
}

// 改进的噪声检测
float DetectImprovedNoise(float2 uv, float2 texelSize) {
    float3 center = INPUT.SampleLevel(sam, uv, 0).rgb;
    float3 blurred = AdaptiveGaussianBlur(uv, texelSize, CalculateImprovedGradient(uv, texelSize), 2.0f);
    
    float3 noise = abs(center - blurred);
    float noiseEnergy = length(noise);
    
    ImprovedGradientResult grad = CalculateImprovedGradient(uv, texelSize);
    float edgeMask = saturate(grad.magnitude * 3.0f);
    
    float adjustedNoise = noiseEnergy * (1.0f - edgeMask * 0.3f);
    
    return saturate(adjustedNoise - GetNoiseThreshold()) / (1.0f - GetNoiseThreshold());
}

// 改进的自适应去噪系统
float3 ImprovedAdaptiveDenoise(float2 uv, float2 texelSize, float3 color, ImprovedGradientResult grad) {
    float noiseLevel = DetectImprovedNoise(uv, texelSize);
    
    float3 denoised = color;
    if (noiseLevel > 0.08f) {
        // 使用双边滤波进行去噪
        float3 center = INPUT.SampleLevel(sam, uv, 0).rgb;
        float centerLum = dot(center, lumWeights);
        
        float3 filtered = center;
        float totalWeight = 1.0f;
        
        // 7x7双边滤波
        for (int y_idx_loop = -3; y_idx_loop <= 3; y_idx_loop++) {
            for (int x_idx_loop = -3; x_idx_loop <= 3; x_idx_loop++) {
                if (x_idx_loop == 0 && y_idx_loop == 0) continue;
                
                float2 offset = float2(x_idx_loop, y_idx_loop) * texelSize;
                float3 sample = INPUT.SampleLevel(sam, uv + offset, 0).rgb;
                float sampleLum = dot(sample, lumWeights);
                
                float spatialWeight = exp(-(x_idx_loop*x_idx_loop + y_idx_loop*y_idx_loop) / (2.0f * 1.5f * 1.5f));
                float colorWeight = exp(-abs(sampleLum - centerLum) * abs(sampleLum - centerLum) / (2.0f * 0.03f * 0.03f));
                
                float weight = spatialWeight * colorWeight;
                filtered += sample * weight;
                totalWeight += weight;
            }
        }
        
        denoised = filtered / totalWeight;
    }
    
    float reductionStrength = GetSpatialNoiseReduction() * noiseLevel;
    
    // 根据梯度特征调整去噪强度
    float adaptiveFactor = lerp(0.3f, 1.0f, grad.confidence);
    adaptiveFactor = lerp(adaptiveFactor, adaptiveFactor * 0.5f, grad.complexity);
    
    return lerp(color, denoised, reductionStrength * adaptiveFactor);
}

// 改进的形态学抗锯齿 - 使用梯度方向场
float3 ImprovedMorphologicalAA(float2 uv, float2 texelSize, ImprovedGradientResult grad) {
    float3 center = INPUT.SampleLevel(sam, uv, 0).rgb;
    
    // 使用双线性插值进行梯度采样，避免量化误差
    float2 pixelPos = uv / texelSize;
    float2 pixelCenter = floor(pixelPos - 0.5f) + 0.5f;
    float2 subPixel = pixelPos - pixelCenter;
    
    // 使用预计算的方向场进行抗锯齿
    float2 perpDir = float2(-grad.directionField.y, grad.directionField.x);
    if (length(perpDir) > 0.001f) {
        perpDir = normalize(perpDir);
    } else {
        perpDir = float2(1, 0);
    }
    
    // 沿正交方向采样进行抗锯齿
    float3 sample1 = INPUT.SampleLevel(sam, uv + perpDir * texelSize * 0.5f, 0).rgb;
    float3 sample2 = INPUT.SampleLevel(sam, uv - perpDir * texelSize * 0.5f, 0).rgb;
    
    float3 bilinearAA = (sample1 + sample2 + center) / 3.0f;
    
    // 根据梯度置信度和一致性应用AA
    float confidence = grad.confidence * grad.coherence * (1.0f - grad.complexity * 1.5f);
    float aaStrength = saturate(confidence) * paramAAStrength * GetAAQuality() * GetMorphAA() * 0.8f;
    
    return lerp(center, bilinearAA, aaStrength);
}

// 柔化函数 - 用于减少噪点
float3 ApplySoftness(float3 color, float2 uv, float2 texelSize, float softness) {
    if (softness <= 0.0f) return color;
    
    float3 result = color;
    
    // 柔化处理 - 使用小范围的高斯模糊
    float3 center = color;
    float3 total = center;
    float totalWeight = 1.0f;
    
    // 3x3 柔化核
    float3 sample1 = INPUT.SampleLevel(sam, uv + float2(texelSize.x, 0), 0).rgb;
    float3 sample2 = INPUT.SampleLevel(sam, uv + float2(-texelSize.x, 0), 0).rgb;
    float3 sample3 = INPUT.SampleLevel(sam, uv + float2(0, texelSize.y), 0).rgb;
    float3 sample4 = INPUT.SampleLevel(sam, uv + float2(0, -texelSize.y), 0).rgb;
    
    total += sample1 * 0.1f;
    total += sample2 * 0.1f;
    total += sample3 * 0.1f;
    total += sample4 * 0.1f;
    totalWeight += 0.4f;
    
    // 对角线采样
    float3 sample5 = INPUT.SampleLevel(sam, uv + float2(texelSize.x, texelSize.y), 0).rgb;
    float3 sample6 = INPUT.SampleLevel(sam, uv + float2(-texelSize.x, texelSize.y), 0).rgb;
    float3 sample7 = INPUT.SampleLevel(sam, uv + float2(texelSize.x, -texelSize.y), 0).rgb;
    float3 sample8 = INPUT.SampleLevel(sam, uv + float2(-texelSize.x, -texelSize.y), 0).rgb;
    
    total += sample5 * 0.05f;
    total += sample6 * 0.05f;
    total += sample7 * 0.05f;
    total += sample8 * 0.05f;
    totalWeight += 0.2f;
    
    float3 softResult = total / totalWeight;
    
    // 根据softness参数混合
    return lerp(color, softResult, softness * 0.5f);
}

// 主像素着色器：改进版本
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
    for (int j_idx_loop = 0; j_idx_loop < 4; j_idx_loop++) {
        for (int i_idx_loop = 0; i_idx_loop < 4; i_idx_loop++) {
            float2 offset = float2((i_idx_loop - 1) * inputPt.x, (j_idx_loop - 1) * inputPt.y);
            samples[j_idx_loop][i_idx_loop] = INPUT.SampleLevel(sam, baseUV + offset, 0).rgb;
        }
    }

    float3 bicubicResult = 0;
    for (int j_idx_loop = 0; j_idx_loop < 4; j_idx_loop++) {
        float3 rowResult = 0;
        for (int i_idx_loop = 0; i_idx_loop < 4; i_idx_loop++) {
            rowResult += samples[j_idx_loop][i_idx_loop] * rowWeights[i_idx_loop];
        }
        bicubicResult += rowResult * colWeights[j_idx_loop];
    }

    // 改进的7x7梯度计算
    ImprovedGradientResult improvedGrad = CalculateImprovedGradient(pos, inputPt);
    
    // 创建扩展的拉普拉斯-高斯金字塔（使用纯净的低频层）
    ExtendedLaplacianPyramid extendedPyramid = CreateExtendedLaplacianPyramid(pos, inputPt, improvedGrad);
    
    // 改进的频率自适应融合（使用低频掩码）
    float3 frequencyFused = ImprovedAdaptiveFrequencyFusion(extendedPyramid, pos, inputPt, improvedGrad);
    
    // 改进的多尺度拉普拉斯细节提取
    ImprovedMultiScaleLaplacian multiLaplacian = CalculateImprovedMultiScaleLaplacian(pos, inputPt);
    float3 laplacianEnhanced = frequencyFused + 
                              multiLaplacian.mediumDetails * GetHighFreqBoost() * 0.5f +
                              multiLaplacian.fineDetails * GetHighFreqBoost() * 0.4f +
                              multiLaplacian.ultraFineDetails * GetHighFreqBoost() * 0.3f;
    
    // 改进的自适应去噪系统
    float3 denoisedResult = ImprovedAdaptiveDenoise(pos, inputPt, laplacianEnhanced, improvedGrad);

    // 改进的形态学抗锯齿
    float3 improvedMorphAAResult = ImprovedMorphologicalAA(pos, inputPt, improvedGrad);
    
    // 根据梯度特征混合结果
    float aaFactor = saturate(improvedGrad.confidence * 2.0f);
    float3 antiAliased = lerp(denoisedResult, improvedMorphAAResult, 
                             aaFactor * paramAAStrength * GetAAQuality() * 0.8f);

    // 改进的自适应锐化
    float3 sharpened = ImprovedAdaptiveSharpen(pos, inputPt, antiAliased, improvedGrad);
    
    // 应用柔化以减少噪点
    float3 softened = ApplySoftness(sharpened, pos, inputPt, paramSoftness);
    
    // 最终输出
    float3 finalColor = softened;
    
    return float4(saturate(finalColor), 1.0);
}