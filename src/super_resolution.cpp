#include "glss/super_resolution.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "glss/vulkan_compute.h"
#include "shaders/upscale_spv.h"

namespace glss {
namespace {

inline int Luma(const uint8_t* rgba) {
    return (299 * rgba[0] + 587 * rgba[1] + 114 * rgba[2]) / 1000;
}

inline float Clamp255(float v) { return std::clamp(v, 0.0f, 255.0f); }

// Edge-safe bilinear sample of one RGBA8 frame at fractional (fx, fy).
inline void SampleBilinear(const FrameBuffer& f, float fx, float fy, float out_rgba[4]) {
    const float max_x = static_cast<float>(f.width - 1);
    const float max_y = static_cast<float>(f.height - 1);
    fx = std::clamp(fx, 0.0f, max_x);
    fy = std::clamp(fy, 0.0f, max_y);

    const uint32_t x0 = static_cast<uint32_t>(fx);
    const uint32_t y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = std::min(x0 + 1, f.width - 1);
    const uint32_t y1 = std::min(y0 + 1, f.height - 1);
    const float dx = fx - static_cast<float>(x0);
    const float dy = fy - static_cast<float>(y0);

    const uint8_t* p = f.data.data();
    const size_t row0 = static_cast<size_t>(y0) * f.width;
    const size_t row1 = static_cast<size_t>(y1) * f.width;
    for (int c = 0; c < 4; ++c) {
        const float v00 = p[(row0 + x0) * 4 + static_cast<size_t>(c)];
        const float v10 = p[(row0 + x1) * 4 + static_cast<size_t>(c)];
        const float v01 = p[(row1 + x0) * 4 + static_cast<size_t>(c)];
        const float v11 = p[(row1 + x1) * 4 + static_cast<size_t>(c)];
        const float top = v00 * (1.0f - dx) + v10 * dx;
        const float bot = v01 * (1.0f - dx) + v11 * dx;
        out_rgba[c] = top * (1.0f - dy) + bot * dy;
    }
}

inline float BilinearLuma(const FrameBuffer& f, float fx, float fy) {
    float rgba[4];
    SampleBilinear(f, fx, fy, rgba);
    return 0.299f * rgba[0] + 0.587f * rgba[1] + 0.114f * rgba[2];
}

} // namespace

SuperResolutionEngine::SuperResolutionEngine(std::shared_ptr<VulkanContext> ctx)
    : vk_ctx_(std::move(ctx)) {}

SuperResolutionEngine::~SuperResolutionEngine() = default;

bool SuperResolutionEngine::Initialize(UpscaleMethod method, float scale_factor, float sharpness) {
    method_ = method;
    scale_factor_ = (scale_factor > 0.0f) ? scale_factor : 1.0f;
    sharpness_ = sharpness;

    // Bring up the GPU compute backend once. Any failure here (no device,
    // shader/pipeline creation error, no host-visible memory) silently keeps us
    // on the CPU path; a runtime dispatch failure is handled per-frame below.
    if (!gpu_attempted_) {
        gpu_attempted_ = true;
        if (vk_ctx_ && vk_ctx_->IsAvailable()) {
            gpu_ = std::make_unique<VulkanCompute>(*vk_ctx_);
            if (gpu_->Initialize(kUpscaleSpirv, kUpscaleSpirvSize)) {
                std::cout << "[GLSS SuperResolution] GPU 计算后端已启用 (Vulkan compute SPIR-V)。"
                          << std::endl;
            } else {
                std::cout << "[GLSS SuperResolution] GPU 计算后端不可用，使用 CPU 路径。"
                          << std::endl;
                gpu_.reset();
            }
        } else {
            std::cout << "[GLSS SuperResolution] Vulkan 上下文不可用，使用 CPU 路径。" << std::endl;
        }
    }

    std::cout << "[GLSS SuperResolution] 初始化超分管线 (方法: "
              << static_cast<int>(method_) << ", 缩放比: " << scale_factor_
              << "x, 锐度: " << sharpness_ << ")" << std::endl;
    return true;
}

bool SuperResolutionEngine::UsingGpu() const {
    return gpu_ != nullptr && gpu_->IsReady();
}

void SuperResolutionEngine::ComputeOutputSize(const FrameBuffer& in, float scale, uint32_t& out_w,
                                              uint32_t& out_h) {
    const float s = (scale > 0.0f) ? scale : 1.0f;
    out_w = static_cast<uint32_t>(std::max(1L, std::lround(static_cast<double>(in.width) * s)));
    out_h = static_cast<uint32_t>(std::max(1L, std::lround(static_cast<double>(in.height) * s)));
}

void SuperResolutionEngine::ApplyBilinear(const FrameBuffer& in, FrameBuffer& out) {
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    ComputeOutputSize(in, scale_factor_, out_w, out_h);

    out.width = out_w;
    out.height = out_h;
    out.timestamp_ns = in.timestamp_ns;
    out.data.assign(static_cast<size_t>(out_w) * out_h * 4, 0);

    const float x_scale = static_cast<float>(in.width) / static_cast<float>(out_w);
    const float y_scale = static_cast<float>(in.height) / static_cast<float>(out_h);

    for (uint32_t y = 0; y < out_h; ++y) {
        const float src_y = (static_cast<float>(y) + 0.5f) * y_scale - 0.5f;
        for (uint32_t x = 0; x < out_w; ++x) {
            const float src_x = (static_cast<float>(x) + 0.5f) * x_scale - 0.5f;
            float rgba[4];
            SampleBilinear(in, src_x, src_y, rgba);
            uint8_t* pixel = out.data.data() + (static_cast<size_t>(y) * out_w + x) * 4;
            for (int c = 0; c < 4; ++c) {
                pixel[c] = static_cast<uint8_t>(Clamp255(rgba[c]));
            }
        }
    }
}

// Edge-adaptive (EASU-inspired) upscale. A spatial Gaussian kernel is
// modulated by a luma range term against the bilinear reference luma, so taps
// on the far side of an edge are down-weighted and edges stay crisp instead of
// being blurred like plain bilinear.
void SuperResolutionEngine::ApplyFSR(const FrameBuffer& in, FrameBuffer& out) {
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    ComputeOutputSize(in, scale_factor_, out_w, out_h);

    out.width = out_w;
    out.height = out_h;
    out.timestamp_ns = in.timestamp_ns;
    out.data.assign(static_cast<size_t>(out_w) * out_h * 4, 0);

    const int w = static_cast<int>(in.width);
    const int h = static_cast<int>(in.height);
    const float x_scale = static_cast<float>(in.width) / static_cast<float>(out_w);
    const float y_scale = static_cast<float>(in.height) / static_cast<float>(out_h);

    constexpr float kSigmaSpatial = 1.0f;
    constexpr float kSigmaRange = 24.0f;
    const float spatial_norm = 1.0f / (2.0f * kSigmaSpatial * kSigmaSpatial);
    const float range_norm = 1.0f / (2.0f * kSigmaRange * kSigmaRange);

    const uint8_t* src = in.data.data();

    for (uint32_t y = 0; y < out_h; ++y) {
        const float src_y = (static_cast<float>(y) + 0.5f) * y_scale - 0.5f;
        const int cy = std::clamp(static_cast<int>(std::lround(src_y)), 0, h - 1);
        for (uint32_t x = 0; x < out_w; ++x) {
            const float src_x = (static_cast<float>(x) + 0.5f) * x_scale - 0.5f;
            const int cx = std::clamp(static_cast<int>(std::lround(src_x)), 0, w - 1);
            const float luma_ref = BilinearLuma(in, src_x, src_y);

            float accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float weight_sum = 0.0f;

            for (int j = -1; j <= 2; ++j) {
                const int sy = std::clamp(cy + j, 0, h - 1);
                const float dy = static_cast<float>(sy) - src_y;
                for (int i = -1; i <= 2; ++i) {
                    const int sx = std::clamp(cx + i, 0, w - 1);
                    const float dx = static_cast<float>(sx) - src_x;

                    const uint8_t* pixel = src + (static_cast<size_t>(sy) * w + sx) * 4;
                    const float luma = static_cast<float>(Luma(pixel));
                    const float d = luma - luma_ref;

                    const float spatial = std::exp(-(dx * dx + dy * dy) * spatial_norm);
                    const float range = std::exp(-(d * d) * range_norm);
                    const float weight = spatial * range;

                    weight_sum += weight;
                    for (int c = 0; c < 4; ++c) {
                        accum[c] += static_cast<float>(pixel[c]) * weight;
                    }
                }
            }

            if (weight_sum <= 0.0f) {
                weight_sum = 1.0f;
            }
            uint8_t* dst = out.data.data() + (static_cast<size_t>(y) * out_w + x) * 4;
            for (int c = 0; c < 4; ++c) {
                dst[c] = static_cast<uint8_t>(Clamp255(accum[c] / weight_sum));
            }
        }
    }
}

// Robust Contrast-Adaptive Sharpening. A 3x3 luma min/max drives an adaptive
// amount that protects high-contrast edges while boosting low-contrast detail.
void SuperResolutionEngine::ApplyRCAS(FrameBuffer& inout, float sharpness) {
    if (inout.data.empty() || inout.width < 3 || inout.height < 3 || sharpness <= 0.0f) {
        return;
    }

    const uint32_t w = inout.width;
    const uint32_t h = inout.height;
    const std::vector<uint8_t> src = inout.data;

    std::vector<float> luma(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < luma.size(); ++i) {
        luma[i] = static_cast<float>(Luma(src.data() + i * 4));
    }

    for (uint32_t y = 1; y + 1 < h; ++y) {
        for (uint32_t x = 1; x + 1 < w; ++x) {
            float min_luma = 255.0f;
            float max_luma = 0.0f;
            float neighbor_sum = 0.0f;
            for (int jj = -1; jj <= 1; ++jj) {
                for (int ii = -1; ii <= 1; ++ii) {
                    if (ii == 0 && jj == 0) {
                        continue;
                    }
                    const float l = luma[(static_cast<size_t>(y + jj) * w) + (x + ii)];
                    min_luma = std::min(min_luma, l);
                    max_luma = std::max(max_luma, l);
                    neighbor_sum += l;
                }
            }

            const float center = luma[static_cast<size_t>(y) * w + x];
            const float blur = neighbor_sum / 8.0f;
            const float detail = center - blur;
            const float contrast = max_luma - min_luma;
            const float adapt = std::clamp(1.0f - contrast / 255.0f, 0.0f, 1.0f);
            const float delta = sharpness * adapt * detail;

            const size_t base = (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const float v = static_cast<float>(src[base + static_cast<size_t>(c)]) + delta;
                inout.data[base + static_cast<size_t>(c)] =
                    static_cast<uint8_t>(Clamp255(v));
            }
        }
    }
}

bool SuperResolutionEngine::Upscale(const FrameBuffer& input_frame, FrameBuffer& output_frame) {
    if (input_frame.data.empty() || input_frame.width == 0 || input_frame.height == 0 ||
        input_frame.data.size() < input_frame.ByteSize()) {
        return false;
    }

    // GPU path: one compute dispatch performs the selected upscale kernel
    // (bilinear or edge-adaptive FSR) followed by RCAS. The mode mirrors the CPU
    // dispatch table below so `--method` is honoured on the GPU too.
    if (UsingGpu()) {
        const uint32_t mode = (method_ == UpscaleMethod::Bilinear) ? 0u : 1u;
        ComputeOutputSize(input_frame, scale_factor_, output_frame.width, output_frame.height);
        output_frame.timestamp_ns = input_frame.timestamp_ns;
        if (gpu_->Dispatch(input_frame, output_frame, sharpness_, mode)) {
            return !output_frame.data.empty();
        }
        // Runtime failure (e.g. device lost): fall back to CPU for this frame.
        std::cerr << "[GLSS SuperResolution] GPU 派发失败，本帧回退到 CPU 路径。" << std::endl;
        output_frame = FrameBuffer{};
    }

    switch (method_) {
    case UpscaleMethod::Bilinear:
        ApplyBilinear(input_frame, output_frame);
        break;
    case UpscaleMethod::FSR_1_0:
    case UpscaleMethod::Anime4K:
    case UpscaleMethod::CustomShader:
    default:
        ApplyFSR(input_frame, output_frame);
        break;
    }

    if (sharpness_ > 0.0f && !output_frame.data.empty()) {
        ApplyRCAS(output_frame, sharpness_);
    }
    return !output_frame.data.empty();
}

} // namespace glss
