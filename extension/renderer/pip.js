// GLSS PiP Window Controller
// Synchronizes native video element with the pure GPU WebGL2/WebGPU engine

import { GlssGpuEngine } from "./glss-gpu.js";

const canvas = document.getElementById("glss-canvas");
const hud = document.getElementById("glss-hud");
const hudBackend = document.getElementById("hud-backend");
const hudSrcRes = document.getElementById("hud-src-res");
const hudOutRes = document.getElementById("hud-out-res");
const hudSrcFps = document.getElementById("hud-src-fps");
const hudRenderFps = document.getElementById("hud-render-fps");
const hudGpuTime = document.getElementById("hud-gpu-time");

const controls = document.getElementById("glss-controls");
const btnPlay = document.getElementById("btn-play");
const timeCurrent = document.getElementById("time-current");
const timeDuration = document.getElementById("time-duration");
const progressContainer = document.getElementById("progress-container");
const progressFill = document.getElementById("progress-fill");
const btnHud = document.getElementById("btn-hud");
const btnFullscreen = document.getElementById("btn-fullscreen");

const groupInterp = document.getElementById("group-interp");
const groupMethod = document.getElementById("group-method");
const groupScale = document.getElementById("group-scale");
const sliderSharpness = document.getElementById("slider-sharpness");
const valSharpness = document.getElementById("val-sharpness");

const engine = new GlssGpuEngine(canvas);

let sourceVideo = null;
let rvfcHandle = null;
let rafHandle = null;
let isInterpActive = true;
let isControlsVisible = true;
let hideControlsTimeout = null;

function formatTime(secs) {
  if (isNaN(secs) || secs < 0) return "00:00";
  const m = Math.floor(secs / 60);
  const s = Math.floor(secs % 60);
  return `${m.toString().padStart(2, "0")}:${s.toString().padStart(2, "0")}`;
}

// Auto-hide controls
function resetControlsTimer() {
  controls.classList.remove("autohide");
  clearTimeout(hideControlsTimeout);
  hideControlsTimeout = setTimeout(() => {
    if (!sourceVideo || !sourceVideo.paused) {
      controls.classList.add("autohide");
    }
  }, 2500);
}

document.addEventListener("mousemove", resetControlsTimer);

// Connect to the video element
export function attachSourceVideo(video) {
  sourceVideo = video;
  console.log("[GLSS PiP] 已成功连接源视频元素:", video);

  // Sync play/pause
  const updatePlayBtn = () => {
    btnPlay.textContent = sourceVideo.paused ? "▶ 播放" : "⏸ 暂停";
  };
  sourceVideo.addEventListener("play", updatePlayBtn);
  sourceVideo.addEventListener("pause", updatePlayBtn);
  updatePlayBtn();

  // Sync progress
  sourceVideo.addEventListener("timeupdate", () => {
    if (sourceVideo.duration) {
      const pct = (sourceVideo.currentTime / sourceVideo.duration) * 100;
      progressFill.style.width = `${pct}%`;
      timeCurrent.textContent = formatTime(sourceVideo.currentTime);
      timeDuration.textContent = formatTime(sourceVideo.duration);
    }
  });

  // Source Frame Clock (requestVideoFrameCallback)
  let lastPresentedFrames = 0;
  const onVideoFrame = (now, metadata) => {
    if (!sourceVideo) return;
    engine.pushVideoFrame(sourceVideo);

    if ("requestVideoFrameCallback" in sourceVideo) {
      rvfcHandle = sourceVideo.requestVideoFrameCallback(onVideoFrame);
    }
  };

  if ("requestVideoFrameCallback" in sourceVideo) {
    rvfcHandle = sourceVideo.requestVideoFrameCallback(onVideoFrame);
  } else {
    // Fallback interval
    setInterval(() => {
      if (sourceVideo && !sourceVideo.paused) {
        engine.pushVideoFrame(sourceVideo);
      }
    }, 1000 / 60);
  }

  // Display Render Clock (requestAnimationFrame at 60/120/144Hz display rate)
  let lastTime = performance.now();
  let phase = 0.0;

  const renderLoop = (timestamp) => {
    if (!sourceVideo) return;

    const mult = engine.interpMultiplier;
    if (mult >= 2 && !sourceVideo.paused) {
      const now = performance.now();
      const elapsedSinceSrc = now - engine.lastFrameArrivalTime;
      const expectedInterval = Math.max(16.0, engine.frameDuration || (1000 / 30));
      phase = Math.min(1.0, Math.max(0.0, elapsedSinceSrc / expectedInterval));
      engine.renderFrame(phase);
    } else {
      engine.renderFrame(1.0);
    }

    // Update HUD Stats
    hudBackend.textContent = engine.stats.backend;
    hudSrcRes.textContent = engine.stats.srcResolution;
    hudOutRes.textContent = engine.stats.outResolution;
    hudSrcFps.textContent = engine.stats.srcFps;
    hudRenderFps.textContent = `${engine.stats.renderFps} FPS`;
    hudGpuTime.textContent = `${engine.stats.gpuTimeMs} ms`;

    rafHandle = requestAnimationFrame(renderLoop);
  };
  rafHandle = requestAnimationFrame(renderLoop);
}

// Check if opener has passed video
if (window.__glssSourceVideo) {
  attachSourceVideo(window.__glssSourceVideo);
} else if (window.opener && window.opener.__glssActiveVideo) {
  attachSourceVideo(window.opener.__glssActiveVideo);
}

// Window message listener for cross-window attachment
window.addEventListener("message", (event) => {
  if (event.data && event.data.type === "GLSS_ATTACH_VIDEO" && window.__glssSourceVideo) {
    attachSourceVideo(window.__glssSourceVideo);
  }
});

// UI Event Handlers
btnPlay.addEventListener("click", () => {
  if (!sourceVideo) return;
  if (sourceVideo.paused) {
    sourceVideo.play();
  } else {
    sourceVideo.pause();
  }
});

progressContainer.addEventListener("click", (e) => {
  if (!sourceVideo || !sourceVideo.duration) return;
  const rect = progressContainer.getBoundingClientRect();
  const ratio = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
  sourceVideo.currentTime = ratio * sourceVideo.duration;
});

// Interp Buttons
groupInterp.querySelectorAll("button").forEach(btn => {
  btn.addEventListener("click", () => {
    groupInterp.querySelectorAll("button").forEach(b => b.classList.remove("active"));
    btn.classList.add("active");
    engine.interpMultiplier = parseInt(btn.dataset.multiplier, 10);
  });
});

// Method Buttons
groupMethod.querySelectorAll("button").forEach(btn => {
  btn.addEventListener("click", () => {
    groupMethod.querySelectorAll("button").forEach(b => b.classList.remove("active"));
    btn.classList.add("active");
    engine.upscaleMethod = btn.dataset.method;
  });
});

// Scale Buttons
groupScale.querySelectorAll("button").forEach(btn => {
  btn.addEventListener("click", () => {
    groupScale.querySelectorAll("button").forEach(b => b.classList.remove("active"));
    btn.classList.add("active");
    engine.scaleFactor = parseFloat(btn.dataset.scale);
    if (sourceVideo) {
      engine.updateDimensions(sourceVideo.videoWidth, sourceVideo.videoHeight);
    }
  });
});

// Sharpness Slider
sliderSharpness.addEventListener("input", () => {
  const v = parseFloat(sliderSharpness.value);
  valSharpness.textContent = v.toFixed(2);
  engine.sharpness = v;
});

// HUD Toggle
btnHud.addEventListener("click", () => {
  hud.style.display = (hud.style.display === "none") ? "block" : "none";
});

// Fullscreen Toggle
btnFullscreen.addEventListener("click", () => {
  if (!document.fullscreenElement) {
    document.documentElement.requestFullscreen().catch(err => alert(err.message));
  } else {
    document.exitFullscreen();
  }
});

// Keyboard Shortcuts
window.addEventListener("keydown", (e) => {
  if (e.code === "Space") {
    e.preventDefault();
    btnPlay.click();
  } else if (e.key === "o" || e.key === "O") {
    btnHud.click();
  } else if (e.key === "f" || e.key === "F") {
    btnFullscreen.click();
  } else if (e.code === "ArrowLeft" && sourceVideo) {
    sourceVideo.currentTime = Math.max(0, sourceVideo.currentTime - 5);
  } else if (e.code === "ArrowRight" && sourceVideo) {
    sourceVideo.currentTime = Math.min(sourceVideo.duration || 0, sourceVideo.currentTime + 5);
  }
});
