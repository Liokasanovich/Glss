# Glss 跨平台 Vulkan 超分辨率与帧插值系统 - 项目进展看板

> 状态：**自主全周期开发中 (In Active Development)**  
> 团队角色：首席架构师 + Vulkan渲染专家 + 算法工程师 + QA测试工程师

---

## 阶段规划与进展矩阵

| 模块 / 任务阶段 | 状态 | 负责角色 | 说明 |
| :--- | :---: | :---: | :--- |
| **Phase 0: 历史代码安全隔离** | ✅ 完成 | 架构师 | 原 Magpie 及自有着色器完整迁移至 `legacy-magpie` 分支并已推送到 GitHub |
| **Phase 1: 跨平台 CMake 体系重构** | 🔄 进行中 | 架构师 | 建立支持 Linux (GCC/Clang) 与 Windows (MSVC) 的现代化无依赖构建体系 |
| **Phase 2: Vulkan 跨平台后端核心** | 🔄 进行中 | Vulkan 专家 | Vulkan 1.2+ 动态加载、计算管线 (Compute Pipeline)、内存与命令缓冲抽象 |
| **Phase 3: 跨平台窗口捕获系统** | ⏳ 待处理 | 捕获专家 | Linux (X11 XShm + Wayland Portal) 与 Windows (DXGI / Desktop Duplication) |
| **Phase 4: 帧插值引擎 (Frame Interpolation)** | ⏳ 待处理 | 算法专家 | 双缓冲历史帧、光流/运动估计矢量场、计算着色器 2x 帧率时域生成 |
| **Phase 5: 超分辨率放大管线 (Super-Res)** | ⏳ 待处理 | 算法专家 | FSR / 自有着色器计算管线移植与调度 |
| **Phase 6: 最简极速交互与 README** | ⏳ 待处理 | 全员 | 遵循第一性与最简原则，极简 CLI，开箱即用三步启动 |
| **Phase 7: 构建、自愈测试与自动提交** | ⏳ 待处理 | QA 专家 | 跨平台编译验证、测试自愈迭代并提交远程 |

---

## 核心设计原则 (遵循用户意向)
1. **第一性原则**：剥离 Windows 专有的 WinUI、繁重注册表钩子和冗余抽象，直接直击核心——**窗口抓取 -> GPU 计算处理 (插帧+超分) -> 极速呈现**。
2. **最简原则 (KISS)**：对外只暴露一个极简 C/C++ API 和命令行工具，不需要臃肿的后台服务。
3. **Vulkan 原生**：无论是 Linux (Wayland/X11) 还是 Windows，统一采用 Vulkan 计算着色器，一套着色器通吃全平台。
