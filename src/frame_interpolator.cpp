#include "glss/frame_interpolator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

#include "glss/vulkan_interp.h"
#include "shaders/interp_spv.h"

namespace glss {
namespace {

inline int Luma(const uint8_t* rgba) {
    // Rec.601 integer luma on the tightly packed RGBA8 pixel.
    return (299 * rgba[0] + 587 * rgba[1] + 114 * rgba[2]) / 1000;
}

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

} // namespace

FrameInterpolator::FrameInterpolator(std::shared_ptr<VulkanContext> ctx)
    : vk_ctx_(std::move(ctx)) {}

FrameInterpolator::~FrameInterpolator() = default;

bool FrameInterpolator::Initialize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    has_previous_frame_ = false;

    const size_t block_count = static_cast<size_t>(BlocksX()) * BlocksY();
    motion_vectors_.assign(block_count * 2, 0.0f);

    if (!gpu_attempted_) {
        gpu_attempted_ = true;
        if (vk_ctx_ && vk_ctx_->IsAvailable()) {
            gpu_ = std::make_unique<VulkanInterpCompute>(*vk_ctx_);
            if (gpu_->Initialize(kInterpSpirv, kInterpSpirvSize)) {
                std::cout << "[GLSS Interpolator] GPU 计算后端已启用 (Vulkan compute SPIR-V 插帧)。"
                          << std::endl;
            } else {
                std::cout << "[GLSS Interpolator] GPU 计算后端不可用，使用 CPU 路径。"
                          << std::endl;
                gpu_.reset();
            }
        } else {
            std::cout << "[GLSS Interpolator] Vulkan 上下文不可用，使用 CPU 路径。" << std::endl;
        }
    }

    std::cout << "[GLSS Interpolator] 帧插值引擎初始化完成 (分辨率: "
              << width << "x" << height << ", 宏块网格: "
              << BlocksX() << "x" << BlocksY() << ", 后端: "
              << (UsingGpu() ? "GPU Vulkan" : "CPU") << ")" << std::endl;
    return true;
}

bool FrameInterpolator::UsingGpu() const {
    return gpu_ != nullptr && gpu_->IsReady();
}

void FrameInterpolator::ComputeMotionEstimation(const FrameBuffer& prev, const FrameBuffer& curr) {
    const uint32_t bx_count = BlocksX();
    const uint32_t by_count = BlocksY();

    if (prev.data.size() != curr.data.size() || prev.width != width_ || prev.height != height_ ||
        prev.data.size() < prev.ByteSize() || bx_count == 0 || by_count == 0) {
        motion_vectors_.assign(static_cast<size_t>(bx_count) * by_count * 2, 0.0f);
        return;
    }

    motion_vectors_.assign(static_cast<size_t>(bx_count) * by_count * 2, 0.0f);

    const int w = static_cast<int>(width_);
    const int h = static_cast<int>(height_);
    const int B = static_cast<int>(kBlockSize);
    const int R = kSearchRadius;

    // Precompute luma planes so the SAD inner loop is a pure byte diff.
    std::vector<uint8_t> luma_prev(static_cast<size_t>(w) * h);
    std::vector<uint8_t> luma_curr(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < luma_prev.size(); ++i) {
        luma_prev[i] = static_cast<uint8_t>(Luma(prev.data.data() + i * 4));
        luma_curr[i] = static_cast<uint8_t>(Luma(curr.data.data() + i * 4));
    }

    for (uint32_t by = 0; by < by_count; ++by) {
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
            const int ox = static_cast<int>(bx) * B;
            const int oy = static_cast<int>(by) * B;

            long best_cost = std::numeric_limits<long>::max();
            int best_dx = 0;
            int best_dy = 0;

            for (int dy = -R; dy <= R; ++dy) {
                for (int dx = -R; dx <= R; ++dx) {
                    long sad = 0;
                    for (int yy = 0; yy < B; ++yy) {
                        const int py = std::min(oy + yy, h - 1);
                        int cy = py + dy;
                        cy = std::clamp(cy, 0, h - 1);
                        const uint8_t* prev_row = luma_prev.data() + static_cast<size_t>(py) * w;
                        const uint8_t* curr_row = luma_curr.data() + static_cast<size_t>(cy) * w;
                        for (int xx = 0; xx < B; ++xx) {
                            const int px = std::min(ox + xx, w - 1);
                            int cx = px + dx;
                            cx = std::clamp(cx, 0, w - 1);
                            sad += std::abs(static_cast<int>(prev_row[px]) -
                                            static_cast<int>(curr_row[cx]));
                        }
                    }
                    const long cost = sad + (std::abs(dx) + std::abs(dy));
                    if (cost < best_cost) {
                        best_cost = cost;
                        best_dx = dx;
                        best_dy = dy;
                    }
                }
            }

            const size_t idx = (static_cast<size_t>(by) * bx_count + bx) * 2;
            motion_vectors_[idx + 0] = static_cast<float>(best_dx);
            motion_vectors_[idx + 1] = static_cast<float>(best_dy);
        }
    }
}

void FrameInterpolator::BlendFramesWithMotionCompensation(const FrameBuffer& f0,
                                                          const FrameBuffer& f1, float t,
                                                          FrameBuffer& out) {
    if (f0.data.size() != f1.data.size() || f0.width != width_ || f0.height != height_ ||
        f0.data.size() < f0.ByteSize()) {
        out = FrameBuffer{};
        return;
    }

    t = std::clamp(t, 0.0f, 1.0f);

    out.width = width_;
    out.height = height_;
    out.timestamp_ns = static_cast<uint64_t>(
        std::llround(static_cast<double>(f0.timestamp_ns) * (1.0 - t) +
                     static_cast<double>(f1.timestamp_ns) * t));
    out.data.assign(out.ByteSize(), 0);

    const uint32_t bx_count = BlocksX();
    uint8_t* dst = out.data.data();

    for (uint32_t y = 0; y < height_; ++y) {
        const uint32_t by = std::min(y / kBlockSize, BlocksY() - 1);
        for (uint32_t x = 0; x < width_; ++x) {
            const uint32_t bx = std::min(x / kBlockSize, bx_count - 1);
            const size_t mv_idx = (static_cast<size_t>(by) * bx_count + bx) * 2;
            const float vx = motion_vectors_[mv_idx + 0];
            const float vy = motion_vectors_[mv_idx + 1];

            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);

            const float sx0 = fx - t * vx;
            const float sy0 = fy - t * vy;
            const float sx1 = fx + (1.0f - t) * vx;
            const float sy1 = fy + (1.0f - t) * vy;

            float prev_sample[4];
            float curr_sample[4];
            SampleBilinear(f0, sx0, sy0, prev_sample);
            SampleBilinear(f1, sx1, sy1, curr_sample);

            uint8_t* pixel = dst + (static_cast<size_t>(y) * width_ + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const float v = (1.0f - t) * prev_sample[c] + t * curr_sample[c];
                pixel[c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
            }
        }
    }
}

bool FrameInterpolator::PushSourceFrame(const FrameBuffer& frame) {
    if (frame.data.empty() || frame.data.size() < frame.ByteSize()) {
        return false;
    }

    if (frame.width != width_ || frame.height != height_) {
        Initialize(frame.width, frame.height);
    }

    if (!has_previous_frame_) {
        prev_frame_ = frame;
        curr_frame_ = frame;
        has_previous_frame_ = true;
        return true;
    }

    prev_frame_ = std::move(curr_frame_);
    curr_frame_ = frame;

    if (!UsingGpu()) {
        ComputeMotionEstimation(prev_frame_, curr_frame_);
    }
    return true;
}

bool FrameInterpolator::GenerateInterpolatedFrame(float t, FrameBuffer& out_frame) {
    if (!has_previous_frame_ || prev_frame_.data.empty() || curr_frame_.data.empty()) {
        return false;
    }

    // 优先使用 GPU Vulkan 计算管线
    if (UsingGpu()) {
        if (gpu_->Dispatch(prev_frame_, curr_frame_, t, out_frame, 0, 4)) {
            return true;
        }
        std::cerr << "[GLSS Interpolator] GPU 插帧调度失败，回退至 CPU 路径。" << std::endl;
    }

    // CPU 回退
    if (motion_vectors_.empty() || motion_vectors_[0] == 0.0f) {
        ComputeMotionEstimation(prev_frame_, curr_frame_);
    }
    BlendFramesWithMotionCompensation(prev_frame_, curr_frame_, t, out_frame);
    return !out_frame.data.empty();
}

bool FrameInterpolator::GetMotionVector(uint32_t block_x, uint32_t block_y, float& vx,
                                        float& vy) const {
    const uint32_t bx_count = BlocksX();
    const uint32_t by_count = BlocksY();
    if (block_x >= bx_count || block_y >= by_count) {
        return false;
    }
    if (motion_vectors_.empty()) {
        return false;
    }
    const size_t idx = (static_cast<size_t>(block_y) * bx_count + block_x) * 2;
    if (idx + 1 >= motion_vectors_.size()) {
        return false;
    }
    vx = motion_vectors_[idx + 0];
    vy = motion_vectors_[idx + 1];
    return true;
}

} // namespace glss
