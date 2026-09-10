#pragma once

#include <cstdint>
#include <memory>

#include "glss/glss.h"
#include "glss/vulkan_context.h"

namespace glss {

class VulkanCompute;

// Super-resolution / upscaling engine.
//
// When a working Vulkan context is supplied the engine runs the embedded
// bilinear + RCAS compute shader on the GPU (see src/vulkan_compute.cpp). If
// the GPU backend cannot be created, or a dispatch fails at runtime, it falls
// back to the CPU implementations below without changing the public interface.
//
// CPU paths:
//   Bilinear     - edge-safe bilinear resample.
//   FSR_1_0      - edge-adaptive (EASU-inspired) bilateral upscale.
//   Anime4K      - falls back to the FSR path (no shader runtime available).
//   CustomShader - falls back to the FSR path.
// All CPU paths then optionally run Robust Contrast-Adaptive Sharpening (RCAS).
class SuperResolutionEngine {
public:
    explicit SuperResolutionEngine(std::shared_ptr<VulkanContext> ctx);
    ~SuperResolutionEngine();

    bool Initialize(UpscaleMethod method, float scale_factor, float sharpness);
    bool Upscale(const FrameBuffer& input_frame, FrameBuffer& output_frame);

    float ScaleFactor() const { return scale_factor_; }

    // True once the Vulkan compute pipeline is built and ready to dispatch.
    bool UsingGpu() const;

private:
    std::shared_ptr<VulkanContext> vk_ctx_;
    UpscaleMethod method_ = UpscaleMethod::FSR_1_0;
    float scale_factor_ = 1.5f;
    float sharpness_ = 0.8f;

    // Declared after vk_ctx_ so it is destroyed first (the backend borrows the
    // context). Null whenever we are on the CPU path.
    std::unique_ptr<VulkanCompute> gpu_;
    bool gpu_attempted_ = false;

    void ApplyBilinear(const FrameBuffer& in, FrameBuffer& out);
    void ApplyFSR(const FrameBuffer& in, FrameBuffer& out);
    void ApplyRCAS(FrameBuffer& inout, float sharpness);

    static void ComputeOutputSize(const FrameBuffer& in, float scale,
                                  uint32_t& out_w, uint32_t& out_h);
};

} // namespace glss
