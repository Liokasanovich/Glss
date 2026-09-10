#include "glss/window_capture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#endif

namespace glss {
namespace {

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// ---------------------------------------------------------------------------
// Synthetic, deterministic capture backend used for headless runs and tests.
// It emits a bright vertical bar translating a fixed number of pixels per
// frame over a static vertical gradient, with timestamps advancing exactly
// 1/60 s per captured frame.
// ---------------------------------------------------------------------------
class SyntheticCapture : public WindowCapture {
public:
    static constexpr uint32_t kWidth = WindowCapture::kSyntheticWidth;
    static constexpr uint32_t kHeight = WindowCapture::kSyntheticHeight;
    static constexpr uint64_t kFrameStepNs = 16666667ULL; // ~1/60 s
    static constexpr int kBarWidth = 24;
    static constexpr int kBarStepPx = 6;

    bool Initialize(uint64_t /*window_handle*/) override {
        frame_index_ = 0;
        return true;
    }

    bool CaptureFrame(FrameBuffer& out_frame) override {
        out_frame.width = kWidth;
        out_frame.height = kHeight;
        out_frame.timestamp_ns = frame_index_ * kFrameStepNs;
        out_frame.data.assign(static_cast<size_t>(kWidth) * kHeight * 4, 0);

        const int bar_x = static_cast<int>((frame_index_ * kBarStepPx) % kWidth);
        const int bar_end = bar_x + kBarWidth;

        for (uint32_t y = 0; y < kHeight; ++y) {
            const uint8_t bg = static_cast<uint8_t>(16 + (y * 48) / kHeight);
            uint8_t* row = out_frame.data.data() + static_cast<size_t>(y) * kWidth * 4;
            for (uint32_t x = 0; x < kWidth; ++x) {
                const bool in_bar =
                    static_cast<int>(x) >= bar_x && static_cast<int>(x) < bar_end;
                const uint8_t v = in_bar ? 235 : bg;
                uint8_t* pixel = row + static_cast<size_t>(x) * 4;
                pixel[0] = v;
                pixel[1] = v;
                pixel[2] = v;
                pixel[3] = 255;
            }
        }

        ++frame_index_;
        return true;
    }

    void Release() override { frame_index_ = 0; }

private:
    uint64_t frame_index_ = 0;
};

#if defined(__linux__)
// ---------------------------------------------------------------------------
// Minimal Xlib ABI declarations.
//
// We deliberately do not include <X11/Xlib.h> or link libX11: the library is
// dlopen'd at runtime and only the handful of entry points listed below are
// resolved. The struct layouts mirror the public Xlib ABI for the fields we
// read; everything else is opaque.
// ---------------------------------------------------------------------------
using XDisplay = struct _XDisplay;
using XWindow = unsigned long;
using XDrawable = unsigned long;
using XAtom = unsigned long;
using XBool = int;

struct XImage;

struct XImageFuncs {
    XImage* (*create_image)();
    int (*destroy_image)();
    unsigned long (*get_pixel)();
    int (*put_pixel)();
    XImage* (*sub_image)();
    int (*add_pixel)();
};

struct XImage {
    int width;
    int height;
    int xoffset;
    int format;
    char* data;
    int byte_order;
    int bitmap_unit;
    int bitmap_bit_order;
    int bitmap_pad;
    int depth;
    int bytes_per_line;
    int bits_per_pixel;
    unsigned long red_mask;
    unsigned long green_mask;
    unsigned long blue_mask;
    void* obdata;
    XImageFuncs f;
};

struct XWindowAttributes {
    int x;
    int y;
    int width;
    int height;
    int border_width;
    int depth;
    void* visual;
    XWindow root;
    int c_class;
    int bit_gravity;
    int win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    int save_under;
    unsigned long colormap;
    int map_installed;
    int map_state;
    long all_event_masks;
    long your_event_mask;
    long do_not_propagate_mask;
    int override_redirect;
    void* screen;
};

struct XErrorEvent {
    int type;
    XDisplay* display;
    XWindow resourceid;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
};

using PFN_XErrorHandler = int (*)(XDisplay*, XErrorEvent*);

using PFN_XOpenDisplay = XDisplay* (*)(const char*);
using PFN_XCloseDisplay = int (*)(XDisplay*);
using PFN_XDefaultRootWindow = XWindow (*)(XDisplay*);
using PFN_XDefaultScreen = int (*)(XDisplay*);
using PFN_XGetWindowAttributes = int (*)(XDisplay*, XWindow, XWindowAttributes*);
using PFN_XGetImage = XImage* (*)(XDisplay*, XDrawable, int, int, unsigned int, unsigned int,
                                  unsigned long, int);
using PFN_XDestroyImage = int (*)(XImage*);
using PFN_XFree = int (*)(void*);
using PFN_XQueryTree = int (*)(XDisplay*, XWindow, XWindow*, XWindow*, XWindow**, unsigned int*);
using PFN_XFetchName = int (*)(XDisplay*, XWindow, char**);
using PFN_XGetWindowProperty = int (*)(XDisplay*, XWindow, XAtom, long, long, XBool, XAtom, XAtom*,
                                       int*, unsigned long*, unsigned long*, unsigned char**);
using PFN_XInternAtom = XAtom (*)(XDisplay*, const char*, XBool);
using PFN_XSetErrorHandler = PFN_XErrorHandler (*)(PFN_XErrorHandler);

constexpr int kZPixmap = 2;
constexpr unsigned long kAllPlanes = ~0UL;
constexpr XAtom kXA_WINDOW = 33;
constexpr int kMSBFirst = 1;
constexpr int kIsViewable = 2;

// Xlib's default error handler terminates the process. Capture failures (e.g.
// grabbing an unmapped window) must instead surface as a `false` return, so a
// non-fatal handler is installed that only records the last error code.
int g_x11_last_error = 0;

int X11ErrorHandler(XDisplay* /*display*/, XErrorEvent* event) {
    if (event != nullptr) {
        g_x11_last_error = event->error_code;
    }
    return 0;
}

// Lazily loads libX11 and resolves the entry points. The handle is intentionally
// kept for the lifetime of the process so an open Display stays valid.
struct X11Api {
    void* handle = nullptr;
    bool attempted = false;
    bool loaded = false;

    PFN_XOpenDisplay XOpenDisplay = nullptr;
    PFN_XCloseDisplay XCloseDisplay = nullptr;
    PFN_XDefaultRootWindow XDefaultRootWindow = nullptr;
    PFN_XDefaultScreen XDefaultScreen = nullptr;
    PFN_XGetWindowAttributes XGetWindowAttributes = nullptr;
    PFN_XGetImage XGetImage = nullptr;
    PFN_XDestroyImage XDestroyImage = nullptr;
    PFN_XFree XFree = nullptr;
    PFN_XQueryTree XQueryTree = nullptr;
    PFN_XFetchName XFetchName = nullptr;
    PFN_XGetWindowProperty XGetWindowProperty = nullptr;
    PFN_XInternAtom XInternAtom = nullptr;
    PFN_XSetErrorHandler XSetErrorHandler = nullptr;

    bool Load() {
        if (attempted) {
            return loaded;
        }
        attempted = true;

        const char* candidates[] = {"libX11.so.6", "libX11.so"};
        for (const char* name : candidates) {
            handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
            if (handle != nullptr) {
                break;
            }
        }
        if (handle == nullptr) {
            return false;
        }

        XOpenDisplay = reinterpret_cast<PFN_XOpenDisplay>(dlsym(handle, "XOpenDisplay"));
        XCloseDisplay = reinterpret_cast<PFN_XCloseDisplay>(dlsym(handle, "XCloseDisplay"));
        XDefaultRootWindow =
            reinterpret_cast<PFN_XDefaultRootWindow>(dlsym(handle, "XDefaultRootWindow"));
        XDefaultScreen = reinterpret_cast<PFN_XDefaultScreen>(dlsym(handle, "XDefaultScreen"));
        XGetWindowAttributes =
            reinterpret_cast<PFN_XGetWindowAttributes>(dlsym(handle, "XGetWindowAttributes"));
        XGetImage = reinterpret_cast<PFN_XGetImage>(dlsym(handle, "XGetImage"));
        XDestroyImage = reinterpret_cast<PFN_XDestroyImage>(dlsym(handle, "XDestroyImage"));
        XFree = reinterpret_cast<PFN_XFree>(dlsym(handle, "XFree"));
        XQueryTree = reinterpret_cast<PFN_XQueryTree>(dlsym(handle, "XQueryTree"));
        XFetchName = reinterpret_cast<PFN_XFetchName>(dlsym(handle, "XFetchName"));
        XGetWindowProperty =
            reinterpret_cast<PFN_XGetWindowProperty>(dlsym(handle, "XGetWindowProperty"));
        XInternAtom = reinterpret_cast<PFN_XInternAtom>(dlsym(handle, "XInternAtom"));
        XSetErrorHandler =
            reinterpret_cast<PFN_XSetErrorHandler>(dlsym(handle, "XSetErrorHandler"));

        loaded = XOpenDisplay != nullptr && XCloseDisplay != nullptr &&
                 XDefaultRootWindow != nullptr && XDefaultScreen != nullptr &&
                 XGetWindowAttributes != nullptr && XGetImage != nullptr && XFree != nullptr &&
                 XQueryTree != nullptr && XFetchName != nullptr && XGetWindowProperty != nullptr &&
                 XInternAtom != nullptr;
        if (loaded && XSetErrorHandler != nullptr) {
            XSetErrorHandler(&X11ErrorHandler);
        }
        return loaded;
    }
};

X11Api& X11() {
    static X11Api api;
    return api;
}

void DestroyXImage(XImage* image) {
    if (image == nullptr) {
        return;
    }
    X11Api& api = X11();
    if (api.XDestroyImage != nullptr) {
        api.XDestroyImage(image);
        return;
    }
    // Fallback for the unlikely case the symbol is unavailable.
    if (image->data != nullptr && api.XFree != nullptr) {
        api.XFree(image->data);
    }
    if (api.XFree != nullptr) {
        api.XFree(image);
    }
}

uint32_t ReadPixel32(const uint8_t* p, int byte_order) {
    if (byte_order == kMSBFirst) {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    }
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t ReadPixel24(const uint8_t* p, int byte_order) {
    if (byte_order == kMSBFirst) {
        return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) |
               static_cast<uint32_t>(p[2]);
    }
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16);
}

uint8_t ExtractChannel(uint32_t pixel, unsigned long mask) {
    if (mask == 0UL) {
        return 0;
    }
    int shift = 0;
    while (shift < 32 && ((mask >> shift) & 1UL) == 0UL) {
        ++shift;
    }
    const unsigned long max_value = mask >> shift;
    if (max_value == 0UL) {
        return 0;
    }
    const unsigned long value = (static_cast<unsigned long>(pixel) & mask) >> shift;
    return static_cast<uint8_t>((value * 255UL + max_value / 2UL) / max_value);
}

class X11Capture : public WindowCapture {
public:
    X11Capture() = default;
    ~X11Capture() override { Release(); }

    bool Initialize(uint64_t window_handle) override {
        Release();

        X11Api& api = X11();
        if (!api.Load()) {
            return false;
        }

        display_ = api.XOpenDisplay(std::getenv("DISPLAY"));
        if (display_ == nullptr) {
            return false;
        }

        const XWindow window = window_handle != 0 ? static_cast<XWindow>(window_handle)
                                                  : api.XDefaultRootWindow(display_);
        if (window == 0) {
            CloseDisplay();
            return false;
        }

        XWindowAttributes attr;
        if (api.XGetWindowAttributes(display_, window, &attr) == 0 || attr.width <= 0 ||
            attr.height <= 0 || attr.map_state != kIsViewable) {
            CloseDisplay();
            return false;
        }

        window_ = window;
        width_ = static_cast<uint32_t>(attr.width);
        height_ = static_cast<uint32_t>(attr.height);
        return true;
    }

    bool CaptureFrame(FrameBuffer& out_frame) override {
        if (display_ == nullptr || window_ == 0) {
            return false;
        }
        X11Api& api = X11();

        // Re-query geometry so window resizes are picked up.
        XWindowAttributes attr;
        if (api.XGetWindowAttributes(display_, window_, &attr) == 0 || attr.width <= 0 ||
            attr.height <= 0 || attr.map_state != kIsViewable) {
            return false;
        }
        width_ = static_cast<uint32_t>(attr.width);
        height_ = static_cast<uint32_t>(attr.height);

        g_x11_last_error = 0;
        image_ = api.XGetImage(display_, static_cast<XDrawable>(window_), 0, 0,
                               static_cast<unsigned int>(width_),
                               static_cast<unsigned int>(height_), kAllPlanes, kZPixmap);
        if (image_ == nullptr) {
            return false;
        }

        out_frame.width = width_;
        out_frame.height = height_;
        out_frame.timestamp_ns = NowNs();
        out_frame.data.assign(out_frame.ByteSize(), 0);

        bool ok = false;
        const int bpp = image_->bits_per_pixel;
        if ((bpp == 32 || bpp == 24) && image_->data != nullptr && image_->bytes_per_line > 0) {
            const int bytes_per_pixel = bpp / 8;
            for (uint32_t y = 0; y < height_; ++y) {
                const uint8_t* src = reinterpret_cast<const uint8_t*>(image_->data) +
                                     static_cast<size_t>(y) * static_cast<size_t>(image_->bytes_per_line);
                uint8_t* dst = out_frame.data.data() + static_cast<size_t>(y) * width_ * 4;
                for (uint32_t x = 0; x < width_; ++x) {
                    const uint8_t* p = src + static_cast<size_t>(x) * bytes_per_pixel;
                    const uint32_t pixel = bpp == 32 ? ReadPixel32(p, image_->byte_order)
                                                     : ReadPixel24(p, image_->byte_order);
                    dst[static_cast<size_t>(x) * 4 + 0] = ExtractChannel(pixel, image_->red_mask);
                    dst[static_cast<size_t>(x) * 4 + 1] = ExtractChannel(pixel, image_->green_mask);
                    dst[static_cast<size_t>(x) * 4 + 2] = ExtractChannel(pixel, image_->blue_mask);
                    dst[static_cast<size_t>(x) * 4 + 3] = 255;
                }
            }
            ok = true;
        }

        DestroyXImage(image_);
        image_ = nullptr;
        return ok;
    }

    void Release() override {
        if (image_ != nullptr) {
            DestroyXImage(image_);
            image_ = nullptr;
        }
        CloseDisplay();
        window_ = 0;
        width_ = 0;
        height_ = 0;
    }

    static std::vector<WindowInfo> ListAllWindows() {
        std::vector<WindowInfo> result;
        X11Api& api = X11();
        if (!api.Load()) {
            return result;
        }

        XDisplay* display = api.XOpenDisplay(std::getenv("DISPLAY"));
        if (display == nullptr) {
            return result;
        }

        const XWindow root = api.XDefaultRootWindow(display);
        XWindow root_return = 0;
        XWindow parent_return = 0;
        XWindow* children = nullptr;
        unsigned int child_count = 0;

        if (api.XQueryTree(display, root, &root_return, &parent_return, &children, &child_count) != 0) {
            // Resolve the focused window through _NET_ACTIVE_WINDOW when present.
            XWindow active = 0;
            const XAtom active_atom = api.XInternAtom(display, "_NET_ACTIVE_WINDOW", 1);
            if (active_atom != 0) {
                XAtom actual_type = 0;
                int actual_format = 0;
                unsigned long items = 0;
                unsigned long bytes_after = 0;
                unsigned char* prop = nullptr;
                if (api.XGetWindowProperty(display, root, active_atom, 0, 1, 0, kXA_WINDOW,
                                           &actual_type, &actual_format, &items, &bytes_after,
                                           &prop) == 0) {
                    if (prop != nullptr && items >= 1 && actual_format == 32) {
                        active = static_cast<XWindow>(*reinterpret_cast<unsigned long*>(prop));
                    }
                    if (prop != nullptr) {
                        api.XFree(prop);
                    }
                }
            }

            for (unsigned int i = 0; i < child_count; ++i) {
                const XWindow window = children[i];

                char* name = nullptr;
                if (api.XFetchName(display, window, &name) == 0 || name == nullptr) {
                    if (name != nullptr) {
                        api.XFree(name);
                    }
                    continue;
                }
                std::string title(name);
                api.XFree(name);
                if (title.empty()) {
                    continue;
                }

                XWindowAttributes attr;
                if (api.XGetWindowAttributes(display, window, &attr) == 0 || attr.width <= 0 ||
                    attr.height <= 0) {
                    continue;
                }

                WindowInfo info;
                info.handle = static_cast<uint64_t>(window);
                info.title = std::move(title);
                info.width = static_cast<uint32_t>(attr.width);
                info.height = static_cast<uint32_t>(attr.height);
                info.is_focused = window == active;
                result.push_back(std::move(info));
            }

            if (children != nullptr) {
                api.XFree(children);
            }
        }

        api.XCloseDisplay(display);
        return result;
    }

private:
    void CloseDisplay() {
        if (display_ != nullptr) {
            X11Api& api = X11();
            if (api.XCloseDisplay != nullptr) {
                api.XCloseDisplay(display_);
            }
            display_ = nullptr;
        }
    }

    XDisplay* display_ = nullptr;
    XWindow window_ = 0;
    XImage* image_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};
#endif // __linux__

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// Windows DXGI Desktop Duplication backend (D3D11).
// ---------------------------------------------------------------------------
template <typename T>
void SafeRelease(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

class DxgiCapture : public WindowCapture {
public:
    DxgiCapture() = default;
    ~DxgiCapture() override { Release(); }

    bool Initialize(uint64_t window_handle) override {
        Release();
        target_hwnd_ = reinterpret_cast<HWND>(static_cast<uintptr_t>(window_handle));

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                       ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr,
                                       &context_);
        if (FAILED(hr) || device_ == nullptr) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr,
                                   &context_);
        }
        if (FAILED(hr) || device_ == nullptr || context_ == nullptr) {
            Release();
            return false;
        }

        IDXGIDevice* dxgi_device = nullptr;
        if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice),
                                           reinterpret_cast<void**>(&dxgi_device)))) {
            Release();
            return false;
        }
        IDXGIAdapter* adapter = nullptr;
        hr = dxgi_device->GetAdapter(&adapter);
        SafeRelease(dxgi_device);
        if (FAILED(hr) || adapter == nullptr) {
            Release();
            return false;
        }

        IDXGIOutput* output = nullptr;
        if (!SelectOutput(adapter, &output)) {
            SafeRelease(adapter);
            Release();
            return false;
        }
        SafeRelease(adapter);

        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        SafeRelease(output);
        if (FAILED(hr) || output1 == nullptr) {
            Release();
            return false;
        }

        hr = output1->DuplicateOutput(device_, &duplication_);
        SafeRelease(output1);
        if (FAILED(hr) || duplication_ == nullptr) {
            Release();
            return false;
        }

        DXGI_OUTDUPL_DESC dup_desc;
        duplication_->GetDesc(&dup_desc);
        width_ = dup_desc.ModeDesc.Width;
        height_ = dup_desc.ModeDesc.Height;
        if (width_ == 0 || height_ == 0) {
            Release();
            return false;
        }

        crop_x_ = 0;
        crop_y_ = 0;
        crop_w_ = static_cast<int>(width_);
        crop_h_ = static_cast<int>(height_);
        if (target_hwnd_ != nullptr) {
            RECT client;
            if (GetClientRect(target_hwnd_, &client) && client.right > client.left &&
                client.bottom > client.top) {
                POINT top_left{client.left, client.top};
                if (ClientToScreen(target_hwnd_, &top_left)) {
                    crop_x_ = top_left.x;
                    crop_y_ = top_left.y;
                    crop_w_ = client.right - client.left;
                    crop_h_ = client.bottom - client.top;
                }
            }
        }
        return true;
    }

    bool CaptureFrame(FrameBuffer& out_frame) override {
        if (duplication_ == nullptr || device_ == nullptr || context_ == nullptr) {
            return false;
        }

        DXGI_OUTDUPL_FRAME_INFO frame_info;
        IDXGIResource* resource = nullptr;
        HRESULT hr = duplication_->AcquireNextFrame(500, &frame_info, &resource);
        if (FAILED(hr) || resource == nullptr) {
            return false;
        }

        ID3D11Texture2D* acquired = nullptr;
        hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                      reinterpret_cast<void**>(&acquired));
        SafeRelease(resource);
        if (FAILED(hr) || acquired == nullptr) {
            duplication_->ReleaseFrame();
            return false;
        }

        if (staging_ == nullptr) {
            D3D11_TEXTURE2D_DESC desc;
            acquired->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
            if (FAILED(hr) || staging_ == nullptr) {
                SafeRelease(acquired);
                duplication_->ReleaseFrame();
                return false;
            }
        }

        context_->CopyResource(staging_, acquired);
        SafeRelease(acquired);

        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            duplication_->ReleaseFrame();
            return false;
        }

        const int cw = crop_w_ > 0 ? crop_w_ : static_cast<int>(width_);
        const int ch = crop_h_ > 0 ? crop_h_ : static_cast<int>(height_);
        out_frame.width = static_cast<uint32_t>(cw);
        out_frame.height = static_cast<uint32_t>(ch);
        out_frame.timestamp_ns = NowNs();
        out_frame.data.assign(out_frame.ByteSize(), 0);

        const auto* base = static_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < ch; ++y) {
            const int src_y = crop_y_ + y;
            uint8_t* dst = out_frame.data.data() + static_cast<size_t>(y) * cw * 4;
            if (src_y < 0 || src_y >= static_cast<int>(height_)) {
                continue;
            }
            const uint8_t* src_row =
                base + static_cast<size_t>(src_y) * static_cast<size_t>(mapped.RowPitch);
            for (int x = 0; x < cw; ++x) {
                const int src_x = crop_x_ + x;
                if (src_x < 0 || src_x >= static_cast<int>(width_)) {
                    dst[static_cast<size_t>(x) * 4 + 3] = 255;
                    continue;
                }
                const uint8_t* p = src_row + static_cast<size_t>(src_x) * 4; // BGRA
                dst[static_cast<size_t>(x) * 4 + 0] = p[2];
                dst[static_cast<size_t>(x) * 4 + 1] = p[1];
                dst[static_cast<size_t>(x) * 4 + 2] = p[0];
                dst[static_cast<size_t>(x) * 4 + 3] = 255;
            }
        }

        context_->Unmap(staging_, 0);
        duplication_->ReleaseFrame();
        return true;
    }

    void Release() override {
        SafeRelease(duplication_);
        SafeRelease(staging_);
        SafeRelease(context_);
        SafeRelease(device_);
        target_hwnd_ = nullptr;
        width_ = 0;
        height_ = 0;
        crop_x_ = 0;
        crop_y_ = 0;
        crop_w_ = 0;
        crop_h_ = 0;
    }

    static std::vector<WindowInfo> ListAllWindows() {
        std::vector<WindowInfo> result;
        const HWND foreground = GetForegroundWindow();
        EnumWindows(&DxgiCapture::EnumProc, reinterpret_cast<LPARAM>(&result));
        for (WindowInfo& info : result) {
            info.is_focused =
                reinterpret_cast<HWND>(static_cast<uintptr_t>(info.handle)) == foreground;
        }
        return result;
    }

private:
    static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lparam) {
        auto* result = reinterpret_cast<std::vector<WindowInfo>*>(lparam);
        if (result == nullptr || !IsWindowVisible(hwnd)) {
            return TRUE;
        }

        const int length = GetWindowTextLengthW(hwnd);
        if (length <= 0) {
            return TRUE;
        }
        std::wstring title(static_cast<size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, title.data(), length + 1);
        if (copied <= 0) {
            return TRUE;
        }
        title.resize(static_cast<size_t>(copied));

        RECT rect;
        if (!GetWindowRect(hwnd, &rect)) {
            return TRUE;
        }
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) {
            return TRUE;
        }

        WindowInfo info;
        info.handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd));
        info.title = WideToUtf8(title);
        info.width = static_cast<uint32_t>(width);
        info.height = static_cast<uint32_t>(height);
        info.is_focused = false;
        if (!info.title.empty()) {
            result->push_back(std::move(info));
        }
        return TRUE;
    }

    // Chooses the DXGI output whose desktop rectangle contains the target
    // window, falling back to the primary output.
    bool SelectOutput(IDXGIAdapter* adapter, IDXGIOutput** out) {
        *out = nullptr;
        if (target_hwnd_ != nullptr) {
            RECT rect;
            if (GetWindowRect(target_hwnd_, &rect)) {
                const LONG center_x = (rect.left + rect.right) / 2;
                const LONG center_y = (rect.top + rect.bottom) / 2;
                for (UINT i = 0;; ++i) {
                    IDXGIOutput* candidate = nullptr;
                    if (adapter->EnumOutputs(i, &candidate) == DXGI_ERROR_NOT_FOUND) {
                        break;
                    }
                    if (candidate == nullptr) {
                        continue;
                    }
                    DXGI_OUTPUT_DESC desc;
                    if (SUCCEEDED(candidate->GetDesc(&desc))) {
                        const RECT& d = desc.DesktopCoordinates;
                        if (center_x >= d.left && center_x < d.right && center_y >= d.top &&
                            center_y < d.bottom) {
                            *out = candidate;
                            return true;
                        }
                    }
                    SafeRelease(candidate);
                }
            }
        }
        return SUCCEEDED(adapter->EnumOutputs(0, out)) && *out != nullptr;
    }

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
    HWND target_hwnd_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int crop_x_ = 0;
    int crop_y_ = 0;
    int crop_w_ = 0;
    int crop_h_ = 0;
};
#endif // _WIN32

} // namespace

std::unique_ptr<WindowCapture> WindowCapture::CreateSyntheticCapture() {
    return std::make_unique<SyntheticCapture>();
}

std::unique_ptr<WindowCapture> WindowCapture::CreatePlatformCapture() {
#if defined(__linux__)
    return std::make_unique<X11Capture>();
#elif defined(_WIN32)
    return std::make_unique<DxgiCapture>();
#else
    return nullptr;
#endif
}

std::vector<WindowInfo> WindowCapture::ListWindows() {
#if defined(__linux__)
    return X11Capture::ListAllWindows();
#elif defined(_WIN32)
    return DxgiCapture::ListAllWindows();
#else
    return {};
#endif
}

Platform WindowCapture::CurrentPlatform() {
#if defined(_WIN32)
    return Platform::Windows_Win32;
#else
    return Platform::Linux_X11;
#endif
}

} // namespace glss
