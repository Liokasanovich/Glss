# GLSS 视频超分辨率与帧插值浏览器扩展 (Pure GPU)

基于 **WebGPU & WebGL2** 的现代浏览器视频增强插件。无需本地复杂环境，直接在 Edge / Chrome 浏览器中对网页视频实现 **纯显卡实时超分辨率（FSR + RCAS / Anime4K）** 与 **运动补偿帧插值（2x / 3x / 4x）** 悬浮独立小窗播放！

---

## 核心特性

- 🚀 **100% 纯 GPU 渲染**：
  - **帧插值 (Frame Interpolation)**：基于 WebGPU / WebGL2 着色器进行时域运动估计与双向补偿，将 `24/30 FPS` 视频实时插帧至 `60 FPS / 120 FPS+`。
  - **超分辨率 (Super Resolution)**：边缘自适应空间放大（EASU）叠加 RCAS（Robust Contrast-Adaptive Sharpening）对比度自适应锐化与二次元线条强化（Anime4K）。
  - **零 CPU 拷贝**：视频帧直通 GPU 显存纹理，全管线在 GPU VRAM 中流转。
- 🪟 **悬浮按钮与独立小窗**：
  - 鼠标移动到任意视频（如 Bilibili、YouTube、网页 HTML5 播放器）上时，自动唤出极简暗黑毛玻璃风格的 `⚡ GLSS 🚀 一键超分插帧` 快捷按钮。
  - 支持 **Document Picture-in-Picture API**（独立脱离浏览器主窗口的小窗，带全套播放控制与性能 HUD），以及页面内浮窗双重保障。
- 📊 **实时性能统计 (HUD)**：
  - 实时显示输入 FPS、输出等效 FPS、源分辨率、放大分辨率及 GPU 渲染耗时（毫秒级）。

---

## 3 步快速安装到 Microsoft Edge 浏览器

1. **打开 Edge 扩展管理页面**：
   - 在 Edge 地址栏输入并回车：`edge://extensions`
2. **开启“开发人员模式”**：
   - 在左侧侧边栏底部，将 **“开发人员模式” (Developer mode)** 开关打开。
3. **加载解压缩的扩展**：
   - 点击页面顶部的 **“加载解压缩的扩展” (Load unpacked)** 按钮。
   - 在文件选择器中，选中本项目中的 `extension` 文件夹：
     `/home/liokasonovich/文档/auto/repos/Glss/extension`
   - 点击确认即可完成安装！

---

## 快速上手与验证

1. **本地快速验证**：
   - 点击浏览器右上角拼图图标中的 **GLSS** 扩展图标。
   - 点击 **“🎬 打开本地快速测试页”**，将打开扩展内置的 `demo.html`。
   - 鼠标悬停在测试视频上，点击右上角 `⚡ GLSS 🚀 一键超分插帧`，即可直观观察到 24 FPS 跑马灯与滚球在 60/120 FPS 下的丝滑插帧效果以及 FSR 锐化超分！
2. **在 Bilibili / YouTube 等视频网站使用**：
   - 打开任意视频播放页面，把鼠标移动到视频画面上，即可看到右上角的浮动按钮，点击立即开启增强小窗。
