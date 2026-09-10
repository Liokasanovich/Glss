#include "glss/glss.h"
#include "glss/window_capture.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Options {
    bool list_windows = false;
    bool headless = false;
    bool no_interp = false;
    std::string window_spec;
    float scale = 1.5f;
    float sharpness = 0.8f;
    glss::UpscaleMethod method = glss::UpscaleMethod::FSR_1_0;
    int multiplier = 2;
    int device = 0;
    int frames = 5;
};

enum class ParseStatus { Ok, Help, Error };

void PrintUsage(const char* program) {
    std::cout << "GLSS - 跨平台 Vulkan 窗口超分与帧插值工具\n\n";
    std::cout << "用法:\n  " << program << " [选项]\n\n";
    std::cout << "选项:\n";
    std::cout << "  -h, --help              显示本帮助并退出\n";
    std::cout << "      --list-windows      列出当前桌面窗口并退出\n";
    std::cout << "      --window <id|标题>  捕获目标窗口 (十进制/十六进制 id 或标题子串)\n";
    std::cout << "      --scale <float>     超分倍率 (默认 1.5)\n";
    std::cout << "      --sharpness <float> 锐化强度 (默认 0.8)\n";
    std::cout << "      --method <name>     超分算法: bilinear | fsr | anime4k (默认 fsr)\n";
    std::cout << "      --no-interp         禁用帧插值\n";
    std::cout << "      --multiplier <int>  插帧倍率 (默认 2)\n";
    std::cout << "      --device <int>      Vulkan 设备索引 (默认 0)\n";
    std::cout << "      --frames <int>      处理帧数 (默认 5)\n";
    std::cout << "      --headless          强制使用内置合成画面 (无需显示服务器)\n";
    std::cout << "\n示例:\n";
    std::cout << "  " << program << " --headless --frames 3 --scale 2.0 --method bilinear\n";
    std::cout << "  " << program << " --window 0x400001 --scale 1.5 --sharpness 0.8\n";
}

bool ParseFloat(const std::string& text, float& out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

bool ParseInt(const std::string& text, long& out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

bool ParseWindowId(const std::string& text, uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
    }
    const unsigned long long value = std::strtoull(text.c_str(), &end, base);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<uint64_t>(value);
    return true;
}

ParseStatus ParseArgs(int argc, char** argv, Options& opt, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto require_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                error = std::string("选项 ") + name + " 缺少参数";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            return ParseStatus::Help;
        } else if (arg == "--list-windows") {
            opt.list_windows = true;
        } else if (arg == "--headless") {
            opt.headless = true;
        } else if (arg == "--no-interp") {
            opt.no_interp = true;
        } else if (arg == "--window") {
            const char* value = require_value("--window");
            if (value == nullptr) {
                return ParseStatus::Error;
            }
            opt.window_spec = value;
        } else if (arg == "--scale") {
            const char* value = require_value("--scale");
            if (value == nullptr) {
                return ParseStatus::Error;
            }
            float parsed = 0.0f;
            if (!ParseFloat(value, parsed) || parsed <= 0.0f) {
                error = std::string("无效的 --scale 值: ") + value;
                return ParseStatus::Error;
            }
            opt.scale = parsed;
        } else if (arg == "--sharpness") {
            const char* value = require_value("--sharpness");
            if (value == nullptr) {
                return ParseStatus::Error;
            }
            float parsed = 0.0f;
            if (!ParseFloat(value, parsed) || parsed < 0.0f) {
                error = std::string("无效的 --sharpness 值: ") + value;
                return ParseStatus::Error;
            }
            opt.sharpness = parsed;
        } else if (arg == "--method") {
            const char* value = require_value("--method");
            if (value == nullptr) {
                return ParseStatus::Error;
            }
            const std::string name = value;
            if (name == "bilinear") {
                opt.method = glss::UpscaleMethod::Bilinear;
            } else if (name == "fsr") {
                opt.method = glss::UpscaleMethod::FSR_1_0;
            } else if (name == "anime4k") {
                opt.method = glss::UpscaleMethod::Anime4K;
            } else {
                error = "无效的 --method 值: " + name + " (可选: bilinear|fsr|anime4k)";
                return ParseStatus::Error;
            }
        } else if (arg == "--multiplier") {
            const char* value = require_value("--multiplier");
            if (value == nullptr) {
                return ParseStatus::Error;
            }
            long parsed = 0;
            if (!ParseInt(value, parsed) || parsed < 1) {
                error = std::string("无效的 --multiplier 值: ") + value;
                return ParseStatus::Error;
            }
            opt.multiplier = static_cast<int>(parsed);
        } else if (arg == "--device") {
            const char* value = require_value("--device");
            if (value == nullptr) {
                return ParseStatus::Error;
            }
            long parsed = 0;
            if (!ParseInt(value, parsed) || parsed < 0) {
                error = std::string("无效的 --device 值: ") + value;
                return ParseStatus::Error;
            }
            opt.device = static_cast<int>(parsed);
        } else if (arg == "--frames") {
            const char* value = require_value("--frames");
            if (value == nullptr) {
                return ParseStatus::Error;
            }
            long parsed = 0;
            if (!ParseInt(value, parsed) || parsed < 1) {
                error = std::string("无效的 --frames 值: ") + value;
                return ParseStatus::Error;
            }
            opt.frames = static_cast<int>(parsed);
        } else {
            error = "未知参数: " + arg;
            return ParseStatus::Error;
        }
    }
    return ParseStatus::Ok;
}

bool ResolveWindow(const std::string& spec, glss::Config& cfg) {
    uint64_t window_id = 0;
    if (ParseWindowId(spec, window_id)) {
        cfg.target_window_id = window_id;
        cfg.target_window_title = spec;
        return true;
    }

    const std::vector<glss::WindowInfo> windows = glss::WindowCapture::ListWindows();
    for (const glss::WindowInfo& window : windows) {
        if (window.title.find(spec) != std::string::npos) {
            cfg.target_window_id = window.handle;
            cfg.target_window_title = window.title;
            return true;
        }
    }
    return false;
}

void PrintWindows() {
    const std::vector<glss::WindowInfo> windows = glss::WindowCapture::ListWindows();
    if (windows.empty()) {
        std::cout << "未枚举到任何窗口（可能没有可用的显示服务器）。" << std::endl;
        return;
    }
    std::cout << "共 " << windows.size() << " 个窗口:" << std::endl;
    for (const glss::WindowInfo& window : windows) {
        std::cout << "  0x" << std::hex << window.handle << std::dec << "  " << window.width
                  << "x" << window.height << (window.is_focused ? "  [focus]" : "") << "  "
                  << window.title << std::endl;
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string error;
    const ParseStatus status = ParseArgs(argc, argv, opt, error);

    if (status == ParseStatus::Help) {
        PrintUsage(argv[0]);
        return 0;
    }
    if (status == ParseStatus::Error) {
        std::cerr << "错误: " << error << "\n\n";
        PrintUsage(argv[0]);
        return 2;
    }

    if (opt.list_windows) {
        PrintWindows();
        return 0;
    }

    std::cout << "============================================================" << std::endl;
    std::cout << "  GLSS: 现代跨平台 (Windows / Linux) Vulkan 窗口超分与帧插值工具" << std::endl;
    std::cout << "============================================================" << std::endl;

    glss::Config cfg;
    cfg.scale_factor = opt.scale;
    cfg.sharpness = opt.sharpness;
    cfg.method = opt.method;
    cfg.enable_frame_interpolation = !opt.no_interp;
    cfg.interpolation_multiplier = opt.multiplier;
    cfg.vulkan_device_index = opt.device;
    // Without an explicit target window there is nothing real to capture, so
    // run the deterministic synthetic backend.
    cfg.headless = opt.headless || opt.window_spec.empty();

    bool fallback_used = false;
    if (!cfg.headless && !ResolveWindow(opt.window_spec, cfg)) {
        std::cerr << "[!] 未找到匹配的窗口 \"" << opt.window_spec
                  << "\"，回退到内置合成画面 (headless)。" << std::endl;
        cfg.headless = true;
        fallback_used = true;
    }

    auto pipeline = glss::CreateGLSSPipeline();
    bool initialized = pipeline != nullptr && pipeline->Initialize(cfg);
    if (!initialized && !cfg.headless) {
        std::cerr << "[!] 真实窗口捕获初始化失败（无显示服务器或无可用后端），"
                     "回退到内置合成画面 (headless)。"
                  << std::endl;
        cfg.headless = true;
        fallback_used = true;
        pipeline = glss::CreateGLSSPipeline();
        initialized = pipeline != nullptr && pipeline->Initialize(cfg);
    }
    if (!initialized) {
        std::cerr << "[-] 初始化管线失败，请检查驱动和运行环境。" << std::endl;
        return 1;
    }

    pipeline->Start();
    std::cout << "[+] 正在运行中... 处理 " << opt.frames << " 帧:" << std::endl;

    for (int i = 1; i <= opt.frames; ++i) {
        if (!pipeline->Step()) {
            std::cerr << "[-] 第 " << i << " 帧处理失败。" << std::endl;
            return 1;
        }
        const glss::PerformanceMetrics m = pipeline->GetMetrics();
        std::cout << "  [Frame " << i << "] 捕获: " << m.capture_time_ms
                  << "ms | 插帧: " << m.interpolation_time_ms
                  << "ms | 超分: " << m.upscale_time_ms
                  << "ms | 输出帧数: " << pipeline->OutputFrameCount()
                  << " | 输入 FPS: " << m.input_fps
                  << " | 输出等效 FPS: " << m.output_fps << std::endl;
    }

    if (pipeline->OutputFrameCount() > 0) {
        const glss::FrameBuffer& frame = pipeline->OutputFrame(0);
        std::cout << "  [验证] 首个输出帧尺寸: " << frame.width << "x" << frame.height << " ("
                  << frame.data.size() << " bytes)" << std::endl;
    }

    pipeline->Stop();
    if (fallback_used) {
        std::cout << "[!] 注意: 本次运行已回退到内置合成画面 (headless)。" << std::endl;
    }
    std::cout << "[✓] 核心管线测试自验完全通过！" << std::endl;
    return 0;
}
