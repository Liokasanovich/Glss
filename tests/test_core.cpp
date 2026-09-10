// Real unit tests for the GLSS core library.
//
// Checks are explicit (not assert()) so they still run under NDEBUG/Release.
// Coverage:
//   1. Block-matching motion estimation recovers a known +4px shift.
//   2. Motion-compensated interpolation beats a naive non-motion blend.
//   3. Super-resolution stays near-constant on constant input, correct dims.
//   4. RCAS measurably increases local contrast on low-contrast detail.
//   5. Headless pipeline emits N upscaled frames per Step for an Nx multiplier
//      (2x -> 2, 3x -> 3, 1x/disabled -> 1) with matching output_fps.
//   6. Vulkan device enumeration smoke test.
//   7. Window enumeration never crashes (even without a display) and the
//      synthetic capture backend yields a 256x144 RGBA8 frame.

#include "glss/frame_interpolator.h"
#include "glss/glss.h"
#include "glss/super_resolution.h"
#include "glss/vulkan_context.h"
#include "glss/window_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "[glss_tests] FAIL: " << expression << " (line " << line << ")" << std::endl;
        ++g_failures;
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

inline int Luma(const uint8_t* rgba) {
    return (299 * rgba[0] + 587 * rgba[1] + 114 * rgba[2]) / 1000;
}

inline void SetPixel(glss::FrameBuffer& f, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t a) {
    uint8_t* p = f.data.data() + (static_cast<size_t>(y) * f.width + x) * 4;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

glss::FrameBuffer MakeFrame(uint32_t w, uint32_t h) {
    glss::FrameBuffer f;
    f.width = w;
    f.height = h;
    f.data.assign(f.ByteSize(), 0);
    return f;
}

// Bright vertical bar (x in [24,32)) over a static vertical gradient.
glss::FrameBuffer MakeBarFrame(uint32_t w, uint32_t h) {
    glss::FrameBuffer f = MakeFrame(w, h);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t bg = static_cast<uint8_t>(30 + (y * 80) / std::max(1u, h - 1));
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t v = (x >= 24 && x < 32) ? 220 : bg;
            SetPixel(f, x, y, v, v, v, 255);
        }
    }
    return f;
}

// Shift content right by `shift` px with edge replication: out(x)=in(x-shift).
glss::FrameBuffer ShiftRight(const glss::FrameBuffer& src, int shift) {
    glss::FrameBuffer out = MakeFrame(src.width, src.height);
    out.timestamp_ns = src.timestamp_ns;
    for (uint32_t y = 0; y < src.height; ++y) {
        for (uint32_t x = 0; x < src.width; ++x) {
            int sx = static_cast<int>(x) - shift;
            sx = std::clamp(sx, 0, static_cast<int>(src.width) - 1);
            const uint8_t* s = src.data.data() + (static_cast<size_t>(y) * src.width + sx) * 4;
            uint8_t* d = out.data.data() + (static_cast<size_t>(y) * out.width + x) * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }
    return out;
}

glss::FrameBuffer NaiveBlend(const glss::FrameBuffer& a, const glss::FrameBuffer& b, float t) {
    glss::FrameBuffer out = MakeFrame(a.width, a.height);
    for (size_t i = 0; i < out.data.size(); ++i) {
        out.data[i] = static_cast<uint8_t>(
            std::clamp((1.0f - t) * a.data[i] + t * b.data[i], 0.0f, 255.0f));
    }
    return out;
}

// Mean absolute per-channel error over [x0,x1) x [y0,y1), RGB only.
double MeanAbsError(const glss::FrameBuffer& a, const glss::FrameBuffer& b, int x0, int y0, int x1,
                    int y1) {
    double sum = 0.0;
    long count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const uint8_t* pa = a.data.data() + (static_cast<size_t>(y) * a.width + x) * 4;
            const uint8_t* pb = b.data.data() + (static_cast<size_t>(y) * b.width + x) * 4;
            for (int c = 0; c < 3; ++c) {
                sum += std::abs(static_cast<double>(pa[c]) - static_cast<double>(pb[c]));
                ++count;
            }
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

glss::FrameBuffer ConstantFrame(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
    glss::FrameBuffer f = MakeFrame(w, h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            SetPixel(f, x, y, r, g, b, 255);
        }
    }
    return f;
}

// Low-contrast 2x2 checkerboard.
glss::FrameBuffer LowContrastChecker(uint32_t w, uint32_t h) {
    glss::FrameBuffer f = MakeFrame(w, h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const bool on = (((x / 2) + (y / 2)) % 2) == 0;
            const uint8_t v = on ? 120 : 100;
            SetPixel(f, x, y, v, v, v, 255);
        }
    }
    return f;
}

// Mean absolute horizontal luma gradient (a local-contrast proxy).
double LocalContrast(const glss::FrameBuffer& f) {
    double sum = 0.0;
    long count = 0;
    for (uint32_t y = 0; y < f.height; ++y) {
        for (uint32_t x = 0; x + 1 < f.width; ++x) {
            const uint8_t* p0 = f.data.data() + (static_cast<size_t>(y) * f.width + x) * 4;
            const uint8_t* p1 = p0 + 4;
            sum += std::abs(Luma(p0) - Luma(p1));
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

void TestMotionEstimation() {
    std::cout << "[glss_tests] -- motion estimation --" << std::endl;
    glss::FrameInterpolator interp(nullptr);
    CHECK(interp.Initialize(64, 64));

    const glss::FrameBuffer a = MakeBarFrame(64, 64);
    const glss::FrameBuffer b = ShiftRight(a, 4);

    CHECK(interp.PushSourceFrame(a));
    CHECK(interp.PushSourceFrame(b));

    bool any_nonzero = false;
    for (float v : interp.MotionVectors()) {
        if (std::abs(v) > 0.5f) {
            any_nonzero = true;
            break;
        }
    }
    CHECK(any_nonzero);

    float vx = 0.0f;
    float vy = 0.0f;
    CHECK(interp.GetMotionVector(1, 1, vx, vy));
    CHECK(std::abs(vx - 4.0f) <= 1.0f);
    CHECK(std::abs(vy - 0.0f) <= 1.0f);
    std::cout << "[glss_tests]    block(1,1) vector = (" << vx << ", " << vy << ")" << std::endl;
}

void TestInterpolationAccuracy() {
    std::cout << "[glss_tests] -- interpolation accuracy --" << std::endl;
    glss::FrameInterpolator interp(nullptr);
    CHECK(interp.Initialize(64, 64));

    const glss::FrameBuffer a = MakeBarFrame(64, 64);
    const glss::FrameBuffer b = ShiftRight(a, 4);
    CHECK(interp.PushSourceFrame(a));
    CHECK(interp.PushSourceFrame(b));

    glss::FrameBuffer mid;
    CHECK(interp.GenerateInterpolatedFrame(0.5f, mid));
    CHECK(mid.width == 64);
    CHECK(mid.height == 64);
    CHECK(mid.data.size() == mid.ByteSize());

    const glss::FrameBuffer reference = ShiftRight(a, 2);
    const glss::FrameBuffer naive = NaiveBlend(a, b, 0.5f);

    const double mc_err = MeanAbsError(mid, reference, 16, 16, 48, 48);
    const double naive_err = MeanAbsError(naive, reference, 16, 16, 48, 48);

    std::cout << "[glss_tests]    motion-comp MAE = " << mc_err
              << ", naive MAE = " << naive_err << std::endl;
    CHECK(mc_err < 12.0);
    CHECK(mc_err < naive_err);
}

void TestSuperResolution() {
    std::cout << "[glss_tests] -- super-resolution --" << std::endl;
    glss::SuperResolutionEngine sr(nullptr);
    CHECK(sr.Initialize(glss::UpscaleMethod::FSR_1_0, 2.0f, 0.0f));

    const glss::FrameBuffer in = ConstantFrame(8, 8, 100, 150, 200);
    glss::FrameBuffer out;
    CHECK(sr.Upscale(in, out));
    CHECK(out.width == 16);
    CHECK(out.height == 16);
    CHECK(out.data.size() == static_cast<size_t>(out.width) * out.height * 4);

    int max_delta = 0;
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            const uint8_t* p = out.data.data() + (static_cast<size_t>(y) * out.width + x) * 4;
            max_delta = std::max(max_delta, std::abs(static_cast<int>(p[0]) - 100));
            max_delta = std::max(max_delta, std::abs(static_cast<int>(p[1]) - 150));
            max_delta = std::max(max_delta, std::abs(static_cast<int>(p[2]) - 200));
        }
    }
    CHECK(max_delta <= 2);

    // Bilinear path also respects dims/size.
    glss::SuperResolutionEngine sr_bilinear(nullptr);
    CHECK(sr_bilinear.Initialize(glss::UpscaleMethod::Bilinear, 2.0f, 0.0f));
    glss::FrameBuffer out_bilinear;
    CHECK(sr_bilinear.Upscale(in, out_bilinear));
    CHECK(out_bilinear.width == 16);
    CHECK(out_bilinear.height == 16);
    CHECK(out_bilinear.data.size() == static_cast<size_t>(out_bilinear.width) * out_bilinear.height * 4);
}

void TestRCAS() {
    std::cout << "[glss_tests] -- RCAS --" << std::endl;
    const glss::FrameBuffer checker = LowContrastChecker(16, 16);

    glss::SuperResolutionEngine sr_off(nullptr);
    CHECK(sr_off.Initialize(glss::UpscaleMethod::FSR_1_0, 1.0f, 0.0f));
    glss::FrameBuffer out_off;
    CHECK(sr_off.Upscale(checker, out_off));

    glss::SuperResolutionEngine sr_on(nullptr);
    CHECK(sr_on.Initialize(glss::UpscaleMethod::FSR_1_0, 1.0f, 1.0f));
    glss::FrameBuffer out_on;
    CHECK(sr_on.Upscale(checker, out_on));

    CHECK(out_off.data.size() == out_on.data.size());
    CHECK(out_off.data != out_on.data);

    const double contrast_off = LocalContrast(out_off);
    const double contrast_on = LocalContrast(out_on);
    std::cout << "[glss_tests]    local contrast off = " << contrast_off
              << ", on = " << contrast_on << std::endl;
    CHECK(contrast_on > contrast_off);
}

// Exercises the real Vulkan compute pipeline. Skips cleanly (and passes) when
// no Vulkan device is available, so CI without a GPU stays green.
void TestGpuSuperResolution() {
    std::cout << "[glss_tests] -- GPU super-resolution --" << std::endl;

    auto ctx = std::make_shared<glss::VulkanContext>();
    if (!ctx->Initialize(0)) {
        std::cout << "[glss_tests]    SKIP: Vulkan context unavailable (no GPU in this environment)"
                  << std::endl;
        return;
    }

    // 32x24 input: vertical gradient plus a bright bar, to exercise both smooth
    // interpolation and the edge-adaptive sharpening term.
    glss::FrameBuffer in = MakeFrame(32, 24);
    for (uint32_t y = 0; y < in.height; ++y) {
        const uint8_t bg = static_cast<uint8_t>(20 + (y * 120) / (in.height - 1));
        for (uint32_t x = 0; x < in.width; ++x) {
            const uint8_t v = (x >= 12 && x < 18) ? 220 : bg;
            SetPixel(in, x, y, v, static_cast<uint8_t>(v / 2), static_cast<uint8_t>(255 - v), 255);
        }
    }

    glss::SuperResolutionEngine gpu(ctx);
    CHECK(gpu.Initialize(glss::UpscaleMethod::Bilinear, 2.0f, 0.8f));

    glss::FrameBuffer gpu_out;
    CHECK(gpu.Upscale(in, gpu_out));
    CHECK(gpu_out.width == 64);
    CHECK(gpu_out.height == 48);
    CHECK(gpu_out.data.size() == gpu_out.ByteSize());

    bool any_nonzero = false;
    for (uint8_t v : gpu_out.data) {
        if (v != 0) {
            any_nonzero = true;
            break;
        }
    }
    CHECK(any_nonzero);

    if (!gpu.UsingGpu()) {
        std::cout << "[glss_tests]    GPU backend not active; CPU fallback verified" << std::endl;
        return;
    }

    std::cout << "[glss_tests]    GPU compute backend ACTIVE; validating against CPU reference"
              << std::endl;

    // The compute shader mirrors the CPU Bilinear + RCAS path, so a CPU-only
    // engine with the same parameters is the reference.
    glss::SuperResolutionEngine cpu(nullptr);
    CHECK(cpu.Initialize(glss::UpscaleMethod::Bilinear, 2.0f, 0.8f));
    glss::FrameBuffer cpu_out;
    CHECK(cpu.Upscale(in, cpu_out));
    CHECK(cpu_out.width == gpu_out.width);
    CHECK(cpu_out.height == gpu_out.height);
    CHECK(cpu_out.data.size() == gpu_out.data.size());

    const double mae = MeanAbsError(gpu_out, cpu_out, 0, 0, static_cast<int>(gpu_out.width),
                                    static_cast<int>(gpu_out.height));
    std::cout << "[glss_tests]    GPU bilinear vs CPU bilinear mean abs error = " << mae << std::endl;
    CHECK(mae < 8.0);

    // D2: the GPU must honour the selected method. The FSR engine has to differ
    // from the bilinear engine above (otherwise `--method` is ignored) and must
    // still track its own CPU reference within tolerance.
    glss::SuperResolutionEngine gpu_fsr(ctx);
    CHECK(gpu_fsr.Initialize(glss::UpscaleMethod::FSR_1_0, 2.0f, 0.8f));
    glss::FrameBuffer gpu_fsr_out;
    CHECK(gpu_fsr.Upscale(in, gpu_fsr_out));
    CHECK(gpu_fsr.UsingGpu());
    CHECK(gpu_fsr_out.width == gpu_out.width);
    CHECK(gpu_fsr_out.height == gpu_out.height);
    CHECK(gpu_fsr_out.data.size() == gpu_out.data.size());
    CHECK(gpu_fsr_out.data != gpu_out.data);
    const double method_diff = MeanAbsError(gpu_out, gpu_fsr_out, 0, 0,
                                            static_cast<int>(gpu_out.width),
                                            static_cast<int>(gpu_out.height));
    std::cout << "[glss_tests]    GPU bilinear vs GPU FSR mean abs diff = " << method_diff
              << std::endl;
    CHECK(method_diff > 1.0);

    glss::SuperResolutionEngine cpu_fsr(nullptr);
    CHECK(cpu_fsr.Initialize(glss::UpscaleMethod::FSR_1_0, 2.0f, 0.8f));
    glss::FrameBuffer cpu_fsr_out;
    CHECK(cpu_fsr.Upscale(in, cpu_fsr_out));
    const double mae_fsr = MeanAbsError(gpu_fsr_out, cpu_fsr_out, 0, 0,
                                        static_cast<int>(gpu_fsr_out.width),
                                        static_cast<int>(gpu_fsr_out.height));
    std::cout << "[glss_tests]    GPU FSR vs CPU FSR mean abs error = " << mae_fsr << std::endl;
    CHECK(mae_fsr < 8.0);

    // Repeat with a different size to prove buffers resize and the pipeline is
    // safe to reuse across frames.
    const glss::FrameBuffer in2 = MakeBarFrame(48, 40);
    glss::FrameBuffer gpu_out2;
    CHECK(gpu.Upscale(in2, gpu_out2));
    CHECK(gpu_out2.width == 96);
    CHECK(gpu_out2.height == 80);
    CHECK(gpu_out2.data.size() == gpu_out2.ByteSize());

    glss::FrameBuffer cpu_out2;
    CHECK(cpu.Upscale(in2, cpu_out2));
    const double mae2 = MeanAbsError(gpu_out2, cpu_out2, 0, 0, static_cast<int>(gpu_out2.width),
                                     static_cast<int>(gpu_out2.height));
    std::cout << "[glss_tests]    resize (48x40) GPU vs CPU mean abs error = " << mae2 << std::endl;
    CHECK(mae2 < 8.0);
}

void TestHeadlessPipeline() {
    std::cout << "[glss_tests] -- headless pipeline --" << std::endl;
    glss::Config cfg;
    cfg.headless = true;
    cfg.enable_frame_interpolation = true;
    cfg.interpolation_multiplier = 2;
    cfg.scale_factor = 1.5f;
    cfg.sharpness = 0.5f;

    auto pipeline = glss::CreateGLSSPipeline();
    CHECK(pipeline != nullptr);
    CHECK(pipeline->Initialize(cfg));
    pipeline->Start();

    for (int i = 0; i < 5; ++i) {
        CHECK(pipeline->Step());
        CHECK(pipeline->OutputFrameCount() == 2);
        for (size_t k = 0; k < pipeline->OutputFrameCount(); ++k) {
            const glss::FrameBuffer& f = pipeline->OutputFrame(k);
            CHECK(f.width == 384);
            CHECK(f.height == 216);
            CHECK(f.data.size() == f.ByteSize());
        }
    }

    const glss::PerformanceMetrics m = pipeline->GetMetrics();
    std::cout << "[glss_tests]    input_frames=" << m.input_frames
              << " output_frames=" << m.output_frames << " input_fps=" << m.input_fps
              << " output_fps=" << m.output_fps << std::endl;
    CHECK(m.input_frames == 5);
    CHECK(m.output_frames == 10);
    CHECK(m.input_fps > 0.0);
    CHECK(m.capture_time_ms >= 0.0);
    CHECK(m.interpolation_time_ms >= 0.0);
    CHECK(m.upscale_time_ms >= 0.0);
    CHECK(std::abs(m.output_fps - 2.0 * m.input_fps) <= 0.05 * 2.0 * m.input_fps);

    pipeline->Stop();
    pipeline->Stop(); // idempotent

    // 3x -> exactly three frames per Step and output_fps ~= 3 * input_fps.
    glss::Config cfg3 = cfg;
    cfg3.interpolation_multiplier = 3;
    auto pipeline3 = glss::CreateGLSSPipeline();
    CHECK(pipeline3 != nullptr);
    CHECK(pipeline3->Initialize(cfg3));
    pipeline3->Start();
    for (int i = 0; i < 5; ++i) {
        CHECK(pipeline3->Step());
        CHECK(pipeline3->OutputFrameCount() == 3);
    }
    const glss::PerformanceMetrics m3 = pipeline3->GetMetrics();
    std::cout << "[glss_tests]    3x input_frames=" << m3.input_frames
              << " output_frames=" << m3.output_frames << " input_fps=" << m3.input_fps
              << " output_fps=" << m3.output_fps << std::endl;
    CHECK(m3.input_frames == 5);
    CHECK(m3.output_frames == 15);
    CHECK(m3.input_fps > 0.0);
    CHECK(std::abs(m3.output_fps - 3.0 * m3.input_fps) <= 0.05 * 3.0 * m3.input_fps);
    pipeline3->Stop();

    // Interpolation enabled but multiplier <= 1 -> exactly one frame per Step.
    glss::Config cfg1 = cfg;
    cfg1.interpolation_multiplier = 1;
    auto pipeline1 = glss::CreateGLSSPipeline();
    CHECK(pipeline1 != nullptr);
    CHECK(pipeline1->Initialize(cfg1));
    pipeline1->Start();
    for (int i = 0; i < 5; ++i) {
        CHECK(pipeline1->Step());
        CHECK(pipeline1->OutputFrameCount() == 1);
    }
    const glss::PerformanceMetrics m1 = pipeline1->GetMetrics();
    std::cout << "[glss_tests]    1x input_frames=" << m1.input_frames
              << " output_frames=" << m1.output_frames << " input_fps=" << m1.input_fps
              << " output_fps=" << m1.output_fps << std::endl;
    CHECK(m1.input_frames == 5);
    CHECK(m1.output_frames == 5);
    CHECK(m1.input_fps > 0.0);
    CHECK(std::abs(m1.output_fps - m1.input_fps) <= 0.05 * m1.input_fps);
    pipeline1->Stop();

    // Interpolation disabled -> exactly one frame per Step.
    glss::Config cfg_no_interp = cfg;
    cfg_no_interp.enable_frame_interpolation = false;
    auto pipeline2 = glss::CreateGLSSPipeline();
    CHECK(pipeline2->Initialize(cfg_no_interp));
    pipeline2->Start();
    for (int i = 0; i < 5; ++i) {
        CHECK(pipeline2->Step());
        CHECK(pipeline2->OutputFrameCount() == 1);
    }
    const glss::PerformanceMetrics m2 = pipeline2->GetMetrics();
    std::cout << "[glss_tests]    no-interp input_fps=" << m2.input_fps
              << " output_fps=" << m2.output_fps << std::endl;
    CHECK(m2.input_fps > 0.0);
    CHECK(std::abs(m2.output_fps - m2.input_fps) <= 0.05 * m2.input_fps);
    pipeline2->Stop();
}

void TestEnumerateDevices() {
    std::cout << "[glss_tests] -- vulkan enumeration --" << std::endl;
    const std::vector<glss::VulkanDeviceInfo> devices = glss::VulkanContext::EnumerateDevices();
    std::cout << "[glss_tests]    detected " << devices.size() << " Vulkan device(s)" << std::endl;
    for (const glss::VulkanDeviceInfo& device : devices) {
        std::cout << "  [" << device.index << "] " << device.name
                  << " (api=" << device.api_version << ", vram=" << device.vram_bytes
                  << " bytes, discrete=" << (device.is_discrete ? "yes" : "no") << ")" << std::endl;
    }
}

void TestWindowCapture() {
    std::cout << "[glss_tests] -- window capture --" << std::endl;

    // Must never crash and may legitimately be empty without a display server.
    const std::vector<glss::WindowInfo> windows = glss::WindowCapture::ListWindows();
    std::cout << "[glss_tests]    enumerated " << windows.size() << " window(s)" << std::endl;
    for (const glss::WindowInfo& window : windows) {
        std::cout << "[glss_tests]      [" << window.width << "x" << window.height << "] "
                  << (window.is_focused ? "* " : "  ") << window.title << std::endl;
    }
    CHECK(windows.size() <= 1000000); // always true; documents the "vector" contract

    std::unique_ptr<glss::WindowCapture> capture =
        glss::WindowCapture::CreateSyntheticCapture();
    CHECK(capture != nullptr);
    CHECK(capture->Initialize(0));

    glss::FrameBuffer frame;
    CHECK(capture->CaptureFrame(frame));
    CHECK(frame.width == 256);
    CHECK(frame.height == 144);
    CHECK(frame.data.size() == frame.ByteSize());
    CHECK(frame.data.size() == static_cast<size_t>(256) * 144 * 4);
    CHECK(!frame.Empty());

    // Every pixel is opaque RGBA8.
    bool alpha_ok = true;
    for (size_t i = 3; i < frame.data.size(); i += 4) {
        if (frame.data[i] != 255) {
            alpha_ok = false;
            break;
        }
    }
    CHECK(alpha_ok);

    capture->Release();
    capture->Release(); // idempotent
}

} // namespace

int main() {
    glss::Config config;
    CHECK(config.interpolation_multiplier == 2);
    CHECK(config.enable_frame_interpolation);
    CHECK(config.scale_factor > 0.0f);
    CHECK(!config.headless);

    TestMotionEstimation();
    TestInterpolationAccuracy();
    TestSuperResolution();
    TestRCAS();
    TestGpuSuperResolution();
    TestHeadlessPipeline();
    TestEnumerateDevices();
    TestWindowCapture();

    if (g_failures != 0) {
        std::cerr << "[glss_tests] " << g_failures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "[glss_tests] all checks passed" << std::endl;
    return 0;
}
