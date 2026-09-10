# Glss 跨平台 Vulkan 超分辨率与帧插值系统 - 项目进展看板

> 状态：**核心功能已落地 (Core Landed)**  
> 团队角色：工程技术主管 + 开发工程师 + QA 测试工程师

---

## 阶段规划与进展矩阵

| 模块 / 任务阶段 | 状态 | 负责角色 | 说明 |
| :--- | :---: | :---: | :--- |
| **Phase 0: 历史代码安全隔离** | ✅ 完成 | 架构师 | 原 Magpie 及自有着色器完整迁移至 `legacy-magpie` 分支；当前分支已彻底移除 617 个 WinUI/Magpie 文件 |
| **Phase 1: 跨平台 CMake 体系重构** | ✅ 完成 | 架构师 | 单文件极简 CMake，支持 Linux (GCC/Clang) 与 Windows (MSVC)，无 FetchContent、无 Vulkan 链接期依赖 |
| **Phase 2: Vulkan 跨平台后端核心** | ✅ 完成 | Vulkan 专家 | 运行期动态加载 loader、实例/物理设备/逻辑设备/计算队列，真实枚举 AMD/NVIDIA/Intel 设备 |
| **Phase 3: 跨平台窗口捕获系统** | ✅ 完成 | 捕获专家 | Linux X11（`dlopen`）与 Windows DXGI Desktop Duplication；另有 headless 合成画面源 |
| **Phase 4: 帧插值引擎 (Frame Interpolation)** | ✅ 完成 | 算法专家 | 16x16 块匹配 SAD 运动估计 + 运动补偿混合；支持任意 `Nx`（默认 2x），单元测试验证 |
| **Phase 5: 超分辨率放大管线 (Super-Res)** | ✅ 完成 | 算法专家 | 真实 Vulkan Compute（内嵌 SPIR-V）执行 Bilinear/FSR + RCAS；GPU 结果与 CPU 参考一致，失败自动回退 CPU |
| **Phase 6: 最简极速交互与 README** | ✅ 完成 | 全员 | 极简 CLI、三步构建、README 与实际能力对齐 |
| **Phase 7: 构建、自愈测试与自动提交** | ✅ 完成 | QA 专家 | 独立 QA 干净构建零告警、`ctest` 全绿、ASan/UBSan 无项目级问题 |

---

## 核心设计原则 (遵循用户意向)
1. **第一性原则**：剥离 Windows 专有的 WinUI、繁重注册表钩子和冗余抽象，直接直击核心——**窗口抓取 -> GPU 计算处理 (插帧+超分) -> 极速呈现**。
2. **最简原则 (KISS)**：对外只暴露一个极简 C++ API 和命令行工具，不需要臃肿的后台服务；单文件 CMake，无外部框架。
3. **Vulkan 原生**：统一采用内嵌 SPIR-V 的 Vulkan 计算着色器，构建期无需 Vulkan SDK；运行期动态加载，无链接期依赖。

---

## 已知限制 (Known Limitations)
- **Wayland 原生捕获**尚未实现（Linux 当前为 X11；Wayland 下依赖 XWayland）。
- **帧插值**当前为 CPU 运动补偿实现（算法正确且已测试），尚未迁移到计算着色器。
- **`Anime4K` / `CustomShader`** 目前复用 FSR 路径，自定义着色器加载尚未实现。
- **首帧插值**会输出一帧与输入相同的重复帧（历史帧预热），后续帧正常。
- Windows DXGI 后端已按标准 API 编写，但当前开发/测试环境为 Linux，未经 Windows 实机编译验证。
