# GLSS (Gaming Lossless Scaling & Super-Resolution)

基于现代 **Vulkan API** 构建的跨平台（**Windows & Linux**）窗口超分辨率放大与**帧插值（Frame Interpolation / 帧生成）**工具。

> [!NOTE]
> 原 Magpie 及自有着色器代码已完整迁移并安全归档至 [`legacy-magpie`](https://github.com/Liokasanovich/Glss/tree/legacy-magpie) 分支。  
> 本 `main` 分支重构为面向未来的全平台 Vulkan 纯净高性能架构。

---

## 核心技术特性

* 🚀 **Vulkan 计算后端**：超分辨率由真实的 Vulkan Compute Pipeline 执行（内嵌预编译 SPIR-V，运行时动态加载 `libvulkan`，无需链接期依赖）；无可用 GPU 时自动回退到等价的 CPU 实现。
* ⚡ **帧插值 (Frame Interpolation)**：基于 Vulkan Compute GPU 运动补偿与时域双帧历史的纯 GPU 插帧（内嵌 SPIR-V，自动回退到 CPU SAD 块匹配），默认 `2x`（`60 FPS -> 120 FPS`），并支持任意 `Nx`（`--multiplier N`）。
* 💎 **超分辨率缩放 (Super Resolution)**：`Bilinear` 与边缘自适应的 `FSR` 路径，叠加 RCAS 对比度自适应锐化。`Anime4K` / `CustomShader` 目前复用 FSR 路径（自定义着色器尚未实现）。
* 🖥️ **跨平台窗口捕获**：Linux 通过 `dlopen` 动态加载 X11（无链接期依赖）；Windows 使用 DXGI Desktop Duplication。无显示环境可使用 `--headless` 合成画面源。
* 🛠️ **第一性与最简原则**：零繁重框架依赖，纯 C++17 构建，单文件极简 CMake，开箱即用。

---

## 3 步快速构建与运行

```bash
# 1. 创建构建目录
mkdir build && cd build

# 2. 编译工程
cmake .. && cmake --build . -j$(nproc)

# 3. 运行体验（默认 headless 自验，无需显示服务器）
./glss
```

### 命令行用法

```bash
./glss --help                 # 查看全部选项
./glss --list-windows         # 列出可捕获窗口
./glss --window 0x400001 --scale 1.5 --sharpness 0.8   # 捕获真实窗口
./glss --headless --frames 5 --multiplier 2            # 合成画面 2x 插帧自验
./glss --headless --frames 3 --multiplier 3            # 3x 插帧
./glss --headless --method bilinear --no-interp        # 仅双线性超分
```

### 运行测试

```bash
ctest --test-dir build --output-on-failure
```

---

## 平台与实现说明

| 能力 | Linux | Windows |
| :--- | :--- | :--- |
| 窗口捕获 | X11（`dlopen`，运行期加载） | DXGI Desktop Duplication |
| Vulkan 后端 | 运行期 `dlopen("libvulkan.so.1")` | 运行期 `LoadLibrary("vulkan-1.dll")` |
| 超分 | Vulkan Compute (纯 GPU) + CPU 回退 | Vulkan Compute (纯 GPU) + CPU 回退 |
| 帧插值 | Vulkan Compute (纯 GPU) + CPU 回退 | Vulkan Compute (纯 GPU) + CPU 回退 |

* **Wayland 捕获**：尚未实现（当前为 X11；Wayland 下可通过 XWayland 或后续 Portal 后端支持）。
* **Vulkan 依赖**：仅使用内嵌 SPIR-V 计算着色器，构建期不依赖 Vulkan SDK / `glslangValidator`。
