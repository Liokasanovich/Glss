#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace glss {

enum class Platform {
    Linux_X11,
    Linux_Wayland,
    Windows_Win32
};

enum class UpscaleMethod {
    Bilinear,
    FSR_1_0,
    Anime4K,
    CustomShader
};

// Tightly packed RGBA8 frame. `data` always holds exactly ByteSize() bytes
// (width * height * 4) once populated.
struct FrameBuffer {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t timestamp_ns = 0;
    std::vector<uint8_t> data;

    size_t ByteSize() const { return static_cast<size_t>(width) * height * 4; }
    bool Empty() const { return data.empty(); }
};

struct Config {
    std::string target_window_title;
    uint64_t target_window_id = 0;

    // 超分辨率配置
    UpscaleMethod method = UpscaleMethod::FSR_1_0;
    float scale_factor = 1.5f;
    float sharpness = 0.8f;
    std::string custom_shader_path;

    // 帧插值配置 (Frame Interpolation)
    bool enable_frame_interpolation = true;
    int interpolation_multiplier = 2; // 2x: 60fps -> 120fps
    float motion_vector_threshold = 0.5f;

    // 运行设备
    int vulkan_device_index = 0;
    bool vsync = true;

    // 无显示后端：使用内置的确定性合成画面源，不依赖窗口/显示服务器。
    bool headless = false;
};

struct PerformanceMetrics {
    double capture_time_ms = 0.0;
    double interpolation_time_ms = 0.0;
    double upscale_time_ms = 0.0;
    double present_time_ms = 0.0;
    double input_fps = 0.0;
    double output_fps = 0.0;
    uint64_t input_frames = 0;
    uint64_t output_frames = 0;
};

class GLSSPipeline {
public:
    virtual ~GLSSPipeline() = default;

    virtual bool Initialize(const Config& config) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool Step() = 0; // 单步帧捕获、插帧、超分与呈现

    virtual PerformanceMetrics GetMetrics() const = 0;
    virtual void UpdateConfig(const Config& config) = 0;

    // 最近一次 Step() 产生的输出帧数量与内容。
    virtual size_t OutputFrameCount() const = 0;
    virtual const FrameBuffer& OutputFrame(size_t index) const = 0;
};

std::unique_ptr<GLSSPipeline> CreateGLSSPipeline();

} // namespace glss
