#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

#include "glss/glss.h"

namespace glss {

struct WindowInfo {
    uint64_t handle;
    std::string title;
    uint32_t width;
    uint32_t height;
    bool is_focused;
};

// Cross-platform window capture.
//
// Backends are selected at runtime:
//   - Linux: X11 loaded dynamically through dlopen (no libX11 link dependency).
//   - Windows: DXGI Desktop Duplication / D3D11.
//   - Other:  no platform backend (CreatePlatformCapture returns nullptr).
//
// A deterministic synthetic backend is always available for headless runs and
// tests; it does not touch any display server.
class WindowCapture {
public:
    // Geometry emitted by the synthetic backend. Exposed so callers (e.g. the
    // pipeline) can size their processing buffers without hardcoding values.
    static constexpr uint32_t kSyntheticWidth = 256;
    static constexpr uint32_t kSyntheticHeight = 144;

    virtual ~WindowCapture() = default;

    virtual bool Initialize(uint64_t window_handle) = 0;
    virtual bool CaptureFrame(FrameBuffer& out_frame) = 0;
    virtual void Release() = 0;

    static std::unique_ptr<WindowCapture> CreatePlatformCapture();
    static std::unique_ptr<WindowCapture> CreateSyntheticCapture();
    static std::vector<WindowInfo> ListWindows();
    static Platform CurrentPlatform();
};

} // namespace glss
