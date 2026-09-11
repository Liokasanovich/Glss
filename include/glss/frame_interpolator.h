#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "glss/glss.h"
#include "glss/vulkan_context.h"

namespace glss {

class VulkanInterpCompute;

// Pure GPU Vulkan compute & CPU block-matching frame interpolator.
//
// GPU path: runs an embedded Vulkan compute shader performing motion estimation
// and motion-compensated temporal blend directly in GPU memory.
// CPU fallback: motion estimation on the LUMA channel using 16x16 blocks with
// SAD metric, driving motion-compensated temporal blend.
class FrameInterpolator {
public:
    static constexpr uint32_t kBlockSize = 16;
    static constexpr int kSearchRadius = 16;

    explicit FrameInterpolator(std::shared_ptr<VulkanContext> ctx);
    ~FrameInterpolator();

    bool Initialize(uint32_t width, uint32_t height);

    // 输入新的原生源帧，计算两帧之间的运动矢量并输出插值后的帧
    // 当 multiplier=2 时，每推入 1 帧，可生成 1 个原生帧 + 1 个插值中间帧
    bool PushSourceFrame(const FrameBuffer& frame);

    // 生成插值帧 (t 位于 0.0 到 1.0 之间)
    bool GenerateInterpolatedFrame(float t, FrameBuffer& out_interpolated_frame);

    bool HasValidHistory() const { return has_previous_frame_; }

    // 是否正在使用 GPU 计算后端进行插帧
    bool UsingGpu() const;

    // 运动矢量场，按块行优先排列，每个块包含 (vx, vy) 两个 float。
    const std::vector<float>& MotionVectors() const { return motion_vectors_; }

    // 读取指定块的运动矢量；越界或尚未计算时返回 false。
    bool GetMotionVector(uint32_t block_x, uint32_t block_y, float& vx, float& vy) const;

    uint32_t BlocksX() const { return (width_ + kBlockSize - 1) / kBlockSize; }
    uint32_t BlocksY() const { return (height_ + kBlockSize - 1) / kBlockSize; }

private:
    std::shared_ptr<VulkanContext> vk_ctx_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool has_previous_frame_ = false;

    FrameBuffer prev_frame_;
    FrameBuffer curr_frame_;

    // GPU 计算后端
    std::unique_ptr<VulkanInterpCompute> gpu_;
    bool gpu_attempted_ = false;

    // 运动向量场 (Motion Vector Field: vx, vy)
    std::vector<float> motion_vectors_;

    void ComputeMotionEstimation(const FrameBuffer& prev, const FrameBuffer& curr);
    void BlendFramesWithMotionCompensation(const FrameBuffer& f0, const FrameBuffer& f1,
                                           float t, FrameBuffer& out);
};

} // namespace glss
