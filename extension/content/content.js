// GLSS Content Script - Edge VSR Style Streaming Video Enhancement
// In-Place GPU Super-Resolution & Motion-Compensated Frame Interpolation for Bilibili, YouTube & Streaming Platforms

(async function() {
  if (window.__glssInjected) return;
  window.__glssInjected = true;

  console.log("[GLSS] 正在加载流媒体增强引擎 (Edge VSR 体验)...");

  // Dynamically import pure GPU engine from extension
  let GlssGpuEngine;
  try {
    const gpuUrl = (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.getURL)
      ? chrome.runtime.getURL("renderer/glss-gpu.js")
      : "../renderer/glss-gpu.js";
    const mod = await import(gpuUrl);
    GlssGpuEngine = mod.GlssGpuEngine;
  } catch (err) {
    console.error("[GLSS] 导入 GPU 渲染模块失败:", err);
    return;
  }

  const enhancedVideoSessions = new WeakMap();

  // Find the appropriate player container for streaming platforms
  function findPlatformPlayerContainer(video) {
    // Bilibili
    const bpxWrap = video.closest(".bpx-player-video-wrap") || video.closest(".bilibili-player-video-wrap") || video.closest(".bpx-player-container");
    if (bpxWrap) return bpxWrap;

    // YouTube
    const ytWrap = video.closest(".html5-video-container") || video.closest("#movie_player");
    if (ytWrap) return ytWrap;

    // Demo wrapper
    const demoWrap = video.closest(".video-wrapper");
    if (demoWrap) return demoWrap;

    // Tencent / iQiyi / Youku / Generic
    const genericWrap = video.closest(".txp_video_container") || video.closest(".iqp-player") || video.parentElement;
    return genericWrap || video.parentElement;
  }

  class VideoEnhanceSession {
    constructor(video) {
      this.video = video;
      this.container = findPlatformPlayerContainer(video);
      this.isEnabled = false;
      this.isSplitScreen = false;
      this.splitPos = 0.5;

      this.interpMultiplier = 2; // 2x default (60 FPS)
      this.interpMode = 0; // 0: 金字塔光流
      this.upscaleMethod = "fsr";
      this.scaleFactor = 1.5;
      this.sharpness = 0.8;

      this.canvas = null;
      this.engine = null;
      this.rvfcHandle = null;
      this.rafHandle = null;

      this.pillEl = null;
      this.flyoutEl = null;
      this.splitEl = null;

      this.initUI();
    }

    initUI() {
      if (!this.container) return;
      this.container.classList.add("glss-player-container");
      if (getComputedStyle(this.container).position === "static") {
        this.container.style.position = "relative";
      }

      // 1. Edge-style Capsule Pill
      this.pillEl = document.createElement("div");
      this.pillEl.className = "glss-edge-pill";
      this.pillEl.innerHTML = `
        <div class="glss-pill-icon">⚡</div>
        <span class="glss-pill-title">增强视频</span>
        <div class="glss-pill-switch" title="开启 / 关闭 GLSS 纯显卡视频增强">
          <div class="glss-pill-thumb"></div>
        </div>
      `;
      this.container.appendChild(this.pillEl);

      // Mouse auto-hide logic
      let hideTimer = null;
      const showPill = () => {
        this.pillEl.classList.add("glss-visible");
        clearTimeout(hideTimer);
        hideTimer = setTimeout(() => {
          if (!this.flyoutEl || this.flyoutEl.style.display === "none") {
            this.pillEl.classList.remove("glss-visible");
          }
        }, 3000);
      };

      this.container.addEventListener("mousemove", showPill);
      this.pillEl.addEventListener("mouseenter", () => {
        clearTimeout(hideTimer);
        this.pillEl.classList.add("glss-visible");
      });

      // Switch toggle click
      const switchEl = this.pillEl.querySelector(".glss-pill-switch");
      switchEl.addEventListener("click", (e) => {
        e.stopPropagation();
        this.toggleEnhancement();
      });

      // Clicking text or icon toggles Flyout menu
      this.pillEl.addEventListener("click", (e) => {
        if (e.target.closest(".glss-pill-switch")) return;
        e.stopPropagation();
        this.toggleFlyout();
      });

      // Close flyout on outside click
      document.addEventListener("click", (e) => {
        if (this.flyoutEl && !this.flyoutEl.contains(e.target) && !this.pillEl.contains(e.target)) {
          this.flyoutEl.style.display = "none";
        }
      });
    }

    toggleFlyout() {
      if (!this.flyoutEl) {
        this.createFlyout();
      }
      const isHidden = (this.flyoutEl.style.display === "none");
      this.flyoutEl.style.display = isHidden ? "flex" : "none";
      if (isHidden) {
        this.pillEl.classList.add("glss-visible");
        this.updateFlyoutStats();
      }
    }

    createFlyout() {
      this.flyoutEl = document.createElement("div");
      this.flyoutEl.className = "glss-edge-flyout";
      this.flyoutEl.style.display = "none";
      this.flyoutEl.innerHTML = `
        <div class="glss-flyout-header">
          <span>⚡ 视频增强 (GLSS GPU)</span>
          <span style="font-size: 10px; color: #818cf8;">Pure GPU</span>
        </div>

        <div class="glss-flyout-row">
          <label>超分辨率算法</label>
          <select class="glss-flyout-select" id="glss-opt-method">
            <option value="fsr" selected>FSR 1.0 (EASU + RCAS)</option>
            <option value="anime4k">Anime4K v4.0 (二次元)</option>
            <option value="nis">NIS (NVIDIA Image Scaling)</option>
            <option value="bicubic">Bicubic (16-Tap 双三次)</option>
            <option value="bilinear">Bilinear (双线性原生)</option>
          </select>
        </div>

        <div class="glss-flyout-row">
          <label>帧插值倍率</label>
          <select class="glss-flyout-select" id="glss-opt-interp">
            <option value="1">关闭 (1x)</option>
            <option value="2" selected>2x 插帧 (60FPS)</option>
            <option value="3">3x 插帧 (90FPS)</option>
            <option value="4">4x 插帧 (120FPS+)</option>
          </select>
        </div>

        <div class="glss-flyout-row">
          <label>插帧算法</label>
          <select class="glss-flyout-select" id="glss-opt-interp-mode">
            <option value="0" selected>金字塔光流 (抗重影/推荐)</option>
            <option value="1">宏块 SAD 匹配 (低开销)</option>
            <option value="2">24p 电影去抖动</option>
            <option value="3">时域加权混合</option>
            <option value="4">🔍 运动向量可视化</option>
          </select>
        </div>

        <div class="glss-flyout-row">
          <label>放大倍率</label>
          <select class="glss-flyout-select" id="glss-opt-scale">
            <option value="1.0">1.0x (原尺寸)</option>
            <option value="1.5" selected>1.5x (推荐)</option>
            <option value="2.0">2.0x (超高清)</option>
          </select>
        </div>

        <div class="glss-flyout-row">
          <label>分屏对比模式</label>
          <input type="checkbox" id="glss-opt-split">
        </div>

        <div class="glss-flyout-hud-bar">
          <span>FPS: <b id="glss-hud-fps" class="glss-flyout-hud-val">--</b></span>
          <span>GPU: <b id="glss-hud-time" class="glss-flyout-hud-val">--</b></span>
        </div>

        <button class="glss-flyout-btn" id="glss-btn-pip">
          🪟 弹出独立画中画小窗 (Document PiP)
        </button>
      `;

      this.container.appendChild(this.flyoutEl);

      // Event bindings
      const optMethod = this.flyoutEl.querySelector("#glss-opt-method");
      const optInterp = this.flyoutEl.querySelector("#glss-opt-interp");
      const optInterpMode = this.flyoutEl.querySelector("#glss-opt-interp-mode");
      const optScale = this.flyoutEl.querySelector("#glss-opt-scale");
      const optSplit = this.flyoutEl.querySelector("#glss-opt-split");
      const btnPip = this.flyoutEl.querySelector("#glss-btn-pip");

      optMethod.addEventListener("change", () => {
        this.upscaleMethod = optMethod.value;
        if (this.engine) this.engine.upscaleMethod = this.upscaleMethod;
      });

      optInterp.addEventListener("change", () => {
        this.interpMultiplier = parseInt(optInterp.value, 10);
        if (this.engine) this.engine.interpMultiplier = this.interpMultiplier;
      });

      optInterpMode.addEventListener("change", () => {
        if (this.engine) this.engine.interpMode = parseInt(optInterpMode.value, 10);
      });

      optScale.addEventListener("change", () => {
        this.scaleFactor = parseFloat(optScale.value);
        if (this.engine) {
          this.engine.scaleFactor = this.scaleFactor;
          this.engine.updateDimensions(this.video.videoWidth, this.video.videoHeight);
        }
      });

      optSplit.addEventListener("change", () => {
        this.isSplitScreen = optSplit.checked;
        if (this.engine) this.engine.splitScreen = this.isSplitScreen;
        this.toggleSplitBar(this.isSplitScreen);
      });

      btnPip.addEventListener("click", () => {
        this.openDocumentPiP();
      });
    }

    updateFlyoutStats() {
      if (!this.flyoutEl || !this.engine) return;
      const fpsEl = this.flyoutEl.querySelector("#glss-hud-fps");
      const timeEl = this.flyoutEl.querySelector("#glss-hud-time");
      if (fpsEl) fpsEl.textContent = `${this.engine.stats.srcFps} ➔ ${this.engine.stats.renderFps}`;
      if (timeEl) timeEl.textContent = `${this.engine.stats.gpuTimeMs}ms`;
    }

    toggleEnhancement() {
      this.isEnabled = !this.isEnabled;
      const switchEl = this.pillEl.querySelector(".glss-pill-switch");

      if (this.isEnabled) {
        switchEl.classList.add("active");
        this.startInPlacePipeline();
        console.log("[GLSS] 纯 GPU 视频增强已原位开启。");
      } else {
        switchEl.classList.remove("active");
        this.stopInPlacePipeline();
        console.log("[GLSS] 纯 GPU 视频增强已关闭。");
      }
    }

    startInPlacePipeline() {
      if (!this.canvas) {
        this.canvas = document.createElement("canvas");
        this.canvas.className = "glss-in-place-canvas";
        // Insert right next to video
        this.video.parentElement.insertBefore(this.canvas, this.video.nextSibling);

        this.engine = new GlssGpuEngine(this.canvas);
        this.engine.scaleFactor = this.scaleFactor;
        this.engine.sharpness = this.sharpness;
        this.engine.upscaleMethod = this.upscaleMethod;
        this.engine.interpMultiplier = this.interpMultiplier;
        this.engine.interpMode = this.interpMode;
        this.engine.splitScreen = this.isSplitScreen;
        this.engine.splitPosition = this.splitPos;
      }

      this.canvas.classList.add("glss-active");
      this.video.classList.add("glss-video-enhanced");

      // Video frame clock (requestVideoFrameCallback)
      const onVideoFrame = () => {
        if (!this.isEnabled) return;
        if (this.engine && this.video.videoWidth > 0) {
          this.engine.pushVideoFrame(this.video);
        }
        if ("requestVideoFrameCallback" in this.video) {
          this.rvfcHandle = this.video.requestVideoFrameCallback(onVideoFrame);
        }
      };

      if ("requestVideoFrameCallback" in this.video) {
        this.rvfcHandle = this.video.requestVideoFrameCallback(onVideoFrame);
      }

      // Display render clock
      const renderLoop = () => {
        if (!this.isEnabled) return;

        if (this.engine && this.engine.hasPrevFrame && !this.video.paused) {
          const now = performance.now();
          const elapsed = now - this.engine.lastFrameArrivalTime;
          const interval = Math.max(16.0, this.engine.frameDuration || 33.3);
          const phase = Math.min(1.0, Math.max(0.0, elapsed / interval));
          this.engine.renderFrame(this.interpMultiplier >= 2 ? phase : 1.0);
        } else if (this.engine && this.engine.hasPrevFrame && this.video.paused) {
          this.engine.renderFrame(1.0);
        }

        if (this.flyoutEl && this.flyoutEl.style.display !== "none") {
          this.updateFlyoutStats();
        }

        this.rafHandle = requestAnimationFrame(renderLoop);
      };
      this.rafHandle = requestAnimationFrame(renderLoop);
    }

    stopInPlacePipeline() {
      if (this.canvas) {
        this.canvas.classList.remove("glss-active");
      }
      this.video.classList.remove("glss-video-enhanced");

      if (this.rafHandle) {
        cancelAnimationFrame(this.rafHandle);
        this.rafHandle = null;
      }
      if (this.isSplitScreen) {
        this.toggleSplitBar(false);
      }
    }

    toggleSplitBar(show) {
      if (!show) {
        if (this.splitEl) {
          this.splitEl.remove();
          this.splitEl = null;
        }
        return;
      }

      if (this.splitEl) return;
      this.splitEl = document.createElement("div");
      this.splitEl.className = "glss-split-divider";
      this.splitEl.style.left = "50%";
      this.splitEl.innerHTML = `
        <div class="glss-split-handle">⇄</div>
        <div class="glss-split-label glss-split-label-left">原画 (无插帧)</div>
        <div class="glss-split-label glss-split-label-right">GLSS (超分+插帧)</div>
      `;
      this.container.appendChild(this.splitEl);

      let isDragging = false;
      this.splitEl.addEventListener("mousedown", (e) => {
        isDragging = true;
        e.preventDefault();
      });

      window.addEventListener("mousemove", (e) => {
        if (!isDragging) return;
        const rect = this.container.getBoundingClientRect();
        let ratio = (e.clientX - rect.left) / rect.width;
        ratio = Math.max(0.05, Math.min(0.95, ratio));
        this.splitPos = ratio;
        this.splitEl.style.left = `${(ratio * 100).toFixed(1)}%`;
        if (this.engine) this.engine.splitPosition = ratio;
      });

      window.addEventListener("mouseup", () => {
        isDragging = false;
      });
    }

    async openDocumentPiP() {
      if ("documentPictureInPicture" in window) {
        try {
          const pipWindow = await window.documentPictureInPicture.requestWindow({
            width: Math.min(1280, Math.max(640, this.video.videoWidth || 960)),
            height: Math.min(720, Math.max(360, this.video.videoHeight || 540))
          });

          pipWindow.__glssSourceVideo = this.video;

          const getUrl = (path) => (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.getURL)
            ? chrome.runtime.getURL(path)
            : `../${path}`;

          const styleLink = pipWindow.document.createElement("link");
          styleLink.rel = "stylesheet";
          styleLink.href = getUrl("renderer/pip.css");
          pipWindow.document.head.appendChild(styleLink);

          const resp = await fetch(getUrl("renderer/pip.html"));
          const html = await resp.text();
          const parser = new DOMParser();
          const doc = parser.parseFromString(html, "text/html");

          const container = doc.getElementById("glss-container");
          pipWindow.document.body.appendChild(container);

          const script = pipWindow.document.createElement("script");
          script.type = "module";
          script.src = getUrl("renderer/pip.js");
          pipWindow.document.body.appendChild(script);

          console.log("[GLSS] 已成功唤出 Document PiP 小窗。");
        } catch (err) {
          console.warn("[GLSS] Document PiP 失败:", err);
        }
      }
    }
  }

  // Scan all HTML5 video players on the page
  function scanAndAttach() {
    document.querySelectorAll("video").forEach((video) => {
      if (!enhancedVideoSessions.has(video) && video.videoWidth >= 0) {
        const session = new VideoEnhanceSession(video);
        enhancedVideoSessions.set(video, session);
      }
    });
  }

  scanAndAttach();

  // Watch for dynamic DOM modifications (Bilibili / YouTube navigation without full page reload)
  const observer = new MutationObserver(() => {
    scanAndAttach();
  });
  observer.observe(document.documentElement, { childList: true, subtree: true });

  setInterval(scanAndAttach, 2000);
})();
