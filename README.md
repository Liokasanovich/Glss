# GLSS (Gaming Lossless Scaling & Super-Resolution)

基于现代 **Vulkan API** 构建的跨平台（**Windows & Linux**）窗口超分辨率放大与**帧插值（Frame Interpolation / 帧生成）**工具。

> [!NOTE]
> 原 Magpie 及自有着色器代码已完整迁移并安全归档至 [`legacy-magpie`](https://github.com/Liokasanovich/Glss/tree/legacy-magpie) 分支。  
> 本 `main` 分支重构为面向未来的全平台 Vulkan 纯净高性能架构。

---

## 核心技术特性

* 🚀 **Vulkan 跨平台计算后端**：统一一套 GPU 计算着色器，通吃 Windows、Linux (X11 & Wayland)。
* ⚡ **帧插值 (Frame Interpolation / Frame Gen)**：基于时域双帧历史与运动矢量估算，实现 `60 FPS -> 120 FPS` 的原生插帧放大。
* 💎 **超分辨率缩放 (Super Resolution)**：边缘保真滤波与自适应锐化（支持 FSR 1.0、Anime4K 以及自定义着色器）。
* 🛠️ **第一性与最简原则**：零繁重框架依赖，纯 C++17 构建，开箱即用。

---

## 3 步快速构建与运行

```bash
# 1. 创建构建目录
mkdir build && cd build

# 2. 编译工程
cmake .. && cmake --build . -j$(nproc)

# 3. 运行体验
./glss
```
