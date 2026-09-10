#include "glss/glss.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "glss/frame_interpolator.h"
#include "glss/super_resolution.h"
#include "glss/vulkan_context.h"
#include "glss/window_capture.h"

namespace glss {

class GLSSPipelineImpl : public GLSSPipeline {
public:
    GLSSPipelineImpl() = default;
    ~GLSSPipelineImpl() override { Stop(); }

    bool Initialize(const Config& config) override {
        config_ = config;
        output_frames_.clear();
        metrics_ = PerformanceMetrics{};
        prev_capture_ts_ns_ = 0;
        has_prev_capture_ts_ = false;

        vulkan_ctx_ = std::make_shared<VulkanContext>();
        if (!vulkan_ctx_->Initialize(config_.vulkan_device_index)) {
            std::cerr << "[GLSS] Vulkan 设备不可用，回退到 CPU 插帧/超分实现。" << std::endl;
        }

        if (config_.headless) {
            capture_ = WindowCapture::CreateSyntheticCapture();
        } else {
            capture_ = WindowCapture::CreatePlatformCapture();
        }
        if (!capture_ || !capture_->Initialize(config_.target_window_id)) {
            return false;
        }

        const uint32_t init_w =
            config_.headless ? WindowCapture::kSyntheticWidth : 1920;
        const uint32_t init_h =
            config_.headless ? WindowCapture::kSyntheticHeight : 1080;

        interpolator_ = std::make_unique<FrameInterpolator>(vulkan_ctx_);
        interpolator_->Initialize(init_w, init_h);

        sr_engine_ = std::make_unique<SuperResolutionEngine>(vulkan_ctx_);
        sr_engine_->Initialize(config_.method, config_.scale_factor, config_.sharpness);

        std::cout << "\n========================================================" << std::endl;
        std::cout << "  GLSS 现代跨平台 Vulkan 窗口超分与插帧管线已就绪！" << std::endl;
        std::cout << "  - 捕获后端: "
                  << (config_.headless ? "合成测试画面 (headless)" : "平台窗口捕获") << std::endl;
        std::cout << "  - 缩放倍率: " << config_.scale_factor << "x" << std::endl;
        const bool interp_enabled =
            config_.enable_frame_interpolation && config_.interpolation_multiplier >= 2;
        std::cout << "  - 帧插值生成: "
                  << (interp_enabled ? ("已启用 (" +
                                        std::to_string(config_.interpolation_multiplier) +
                                        "x 模式)")
                                     : "禁用")
                  << std::endl;
        std::cout << "========================================================\n" << std::endl;

        return true;
    }

    void Start() override { running_ = true; }

    void Stop() override {
        running_ = false;
        if (capture_) {
            capture_->Release();
        }
    }

    bool Step() override {
        if (!running_) {
            return false;
        }

        using Clock = std::chrono::steady_clock;
        auto ElapsedMs = [](Clock::time_point a, Clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };

        // 1. Capture.
        const auto t0 = Clock::now();
        FrameBuffer raw_frame;
        if (!capture_->CaptureFrame(raw_frame)) {
            return false;
        }
        const auto t1 = Clock::now();

        // 2. Interpolate. For an Nx multiplier we emit N frames per input frame:
        //    interpolated frames at t = k/N for k = 1..N-1, then the current
        //    frame. N <= 1 (or interpolation disabled) emits just the current
        //    frame, i.e. interpolation is effectively off.
        std::vector<FrameBuffer> emitted;
        const auto t2 = Clock::now();
        const int multiplier = config_.interpolation_multiplier;
        if (config_.enable_frame_interpolation && multiplier >= 2) {
            interpolator_->PushSourceFrame(raw_frame);
            for (int k = 1; k < multiplier; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(multiplier);
                FrameBuffer interpolated;
                if (interpolator_->GenerateInterpolatedFrame(t, interpolated)) {
                    emitted.push_back(std::move(interpolated));
                }
            }
        }
        emitted.push_back(raw_frame);
        const auto t3 = Clock::now();

        // 3. Upscale every emitted frame.
        std::vector<FrameBuffer> upscaled;
        upscaled.reserve(emitted.size());
        for (const FrameBuffer& frame : emitted) {
            FrameBuffer up;
            if (!sr_engine_->Upscale(frame, up)) {
                up = FrameBuffer{};
            }
            upscaled.push_back(std::move(up));
        }
        const auto t4 = Clock::now();

        output_frames_ = std::move(upscaled);
        const auto t5 = Clock::now();

        // 4. Metrics (all values are computed from real work / timestamps).
        metrics_.capture_time_ms = ElapsedMs(t0, t1);
        metrics_.interpolation_time_ms = ElapsedMs(t2, t3);
        metrics_.upscale_time_ms = ElapsedMs(t3, t4);
        metrics_.present_time_ms = ElapsedMs(t4, t5);
        metrics_.input_frames += 1;
        metrics_.output_frames += output_frames_.size();

        if (has_prev_capture_ts_ && raw_frame.timestamp_ns > prev_capture_ts_ns_) {
            const double dt_s =
                static_cast<double>(raw_frame.timestamp_ns - prev_capture_ts_ns_) / 1e9;
            if (dt_s > 0.0) {
                metrics_.input_fps = 1.0 / dt_s;
            }
        }
        prev_capture_ts_ns_ = raw_frame.timestamp_ns;
        has_prev_capture_ts_ = true;

        if (metrics_.input_fps > 0.0 && metrics_.input_frames > 0 &&
            metrics_.output_frames > 0) {
            metrics_.output_fps =
                metrics_.input_fps * static_cast<double>(metrics_.output_frames) /
                static_cast<double>(metrics_.input_frames);
        }

        return true;
    }

    PerformanceMetrics GetMetrics() const override { return metrics_; }

    void UpdateConfig(const Config& config) override {
        config_ = config;
        if (sr_engine_) {
            sr_engine_->Initialize(config_.method, config_.scale_factor, config_.sharpness);
        }
    }

    size_t OutputFrameCount() const override { return output_frames_.size(); }

    const FrameBuffer& OutputFrame(size_t index) const override {
        static const FrameBuffer kEmpty;
        if (index >= output_frames_.size()) {
            return kEmpty;
        }
        return output_frames_[index];
    }

private:
    Config config_;
    PerformanceMetrics metrics_;
    std::atomic<bool> running_{false};

    std::shared_ptr<VulkanContext> vulkan_ctx_;
    std::unique_ptr<WindowCapture> capture_;
    std::unique_ptr<FrameInterpolator> interpolator_;
    std::unique_ptr<SuperResolutionEngine> sr_engine_;

    std::vector<FrameBuffer> output_frames_;
    uint64_t prev_capture_ts_ns_ = 0;
    bool has_prev_capture_ts_ = false;
};

std::unique_ptr<GLSSPipeline> CreateGLSSPipeline() {
    return std::make_unique<GLSSPipelineImpl>();
}

} // namespace glss
