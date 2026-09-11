// GLSS Content Script
// Injects floating hover enhancement button on all HTML5 <video> elements
// Launches Document Picture-in-Picture or in-page theater player

(function() {
  if (window.__glssInjected) return;
  window.__glssInjected = true;

  console.log("[GLSS Content Script] 视频增强脚本已加载。");

  // Track active videos and their floating buttons
  const attachedVideos = new WeakSet();

  function attachWidgetToVideo(video) {
    if (attachedVideos.has(video)) return;
    attachedVideos.add(video);

    // Ensure parent allows relative positioning or create wrapper
    let parent = video.parentElement;
    if (!parent) return;

    // Create floating widget
    const widget = document.createElement("div");
    widget.className = "glss-floating-widget";
    widget.innerHTML = `
      <div class="glss-widget-badge">⚡ GLSS</div>
      <button class="glss-widget-btn" title="一键开启 GPU 超分辨率与插帧独立小窗">
        <span>🚀 一键超分插帧</span>
      </button>
    `;

    // Append widget to parent or body
    parent.classList.add("glss-video-wrapper");
    if (getComputedStyle(parent).position === "static") {
      parent.style.position = "relative";
    }
    parent.appendChild(widget);

    // Mouse movement detection for responsive show/hide
    let hideTimer = null;
    const showWidget = () => {
      widget.classList.add("glss-visible");
      clearTimeout(hideTimer);
      hideTimer = setTimeout(() => {
        widget.classList.remove("glss-visible");
      }, 3000);
    };

    video.addEventListener("mousemove", showWidget);
    widget.addEventListener("mouseenter", () => {
      clearTimeout(hideTimer);
      widget.classList.add("glss-visible");
    });
    widget.addEventListener("mouseleave", () => {
      hideTimer = setTimeout(() => {
        widget.classList.remove("glss-visible");
      }, 1000);
    });

    // Click handler: Launch Enhanced PiP
    const launchBtn = widget.querySelector(".glss-widget-btn");
    launchBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      e.preventDefault();
      openEnhancedPlayer(video);
    });
  }

  // Launches Document PiP or In-Page Modal
  async function openEnhancedPlayer(video) {
    window.__glssActiveVideo = video;
    const w = video.videoWidth || 960;
    const h = video.videoHeight || 540;

    // Try modern Document Picture-in-Picture API first
    if ("documentPictureInPicture" in window) {
      try {
        const pipWindow = await window.documentPictureInPicture.requestWindow({
          width: Math.min(1280, Math.max(640, w)),
          height: Math.min(720, Math.max(360, h))
        });

        // Pass video reference
        pipWindow.__glssSourceVideo = video;

        // Copy styles or inject link to pip.css
        const styleLink = pipWindow.document.createElement("link");
        styleLink.rel = "stylesheet";
        styleLink.href = chrome.runtime.getURL("renderer/pip.css");
        pipWindow.document.head.appendChild(styleLink);

        // Fetch and inject pip.html content into pipWindow
        const resp = await fetch(chrome.runtime.getURL("renderer/pip.html"));
        const html = await resp.text();
        const parser = new DOMParser();
        const doc = parser.parseFromString(html, "text/html");

        // Transfer body container
        const container = doc.getElementById("glss-container");
        pipWindow.document.body.appendChild(container);

        // Inject module script
        const script = pipWindow.document.createElement("script");
        script.type = "module";
        script.src = chrome.runtime.getURL("renderer/pip.js");
        pipWindow.document.body.appendChild(script);

        console.log("[GLSS] 已成功打开 Document Picture-in-Picture 小窗。");
        return;
      } catch (err) {
        console.warn("[GLSS] Document PiP 启动异常，降级到页面内浮窗:", err);
      }
    }

    // Fallback: In-Page Floating Window
    openInPageModal(video, w, h);
  }

  function openInPageModal(video, w, h) {
    const existing = document.getElementById("glss-inpage-modal");
    if (existing) existing.remove();

    const modal = document.createElement("div");
    modal.id = "glss-inpage-modal";
    modal.className = "glss-modal-overlay";
    modal.style.width = `${Math.min(960, Math.max(540, w))}px`;
    modal.style.height = `${Math.min(540, Math.max(320, h))}px`;

    modal.innerHTML = `
      <div class="glss-modal-header" id="glss-modal-drag">
        <div class="glss-modal-title">⚡ GLSS 视频增强独立浮窗 (Pure GPU)</div>
        <button class="glss-modal-close" id="glss-modal-close" title="关闭浮窗">✕</button>
      </div>
      <iframe class="glss-modal-iframe" src="${chrome.runtime.getURL("renderer/pip.html")}"></iframe>
    `;

    document.body.appendChild(modal);

    // Pass video reference to iframe when loaded
    const iframe = modal.querySelector(".glss-modal-iframe");
    iframe.addEventListener("load", () => {
      try {
        iframe.contentWindow.__glssSourceVideo = video;
        iframe.contentWindow.postMessage({ type: "GLSS_ATTACH_VIDEO" }, "*");
      } catch (e) {
        console.warn("[GLSS] Iframe 跨域限制:", e);
      }
    });

    modal.querySelector("#glss-modal-close").addEventListener("click", () => {
      modal.remove();
    });

    // Make modal draggable
    const header = modal.querySelector("#glss-modal-drag");
    let isDragging = false;
    let startX, startY, initLeft, initTop;

    header.addEventListener("mousedown", (e) => {
      if (e.target.tagName === "BUTTON") return;
      isDragging = true;
      startX = e.clientX;
      startY = e.clientY;
      const rect = modal.getBoundingClientRect();
      initLeft = rect.left;
      initTop = rect.top;
      e.preventDefault();
    });

    window.addEventListener("mousemove", (e) => {
      if (!isDragging) return;
      modal.style.left = `${initLeft + (e.clientX - startX)}px`;
      modal.style.top = `${initTop + (e.clientY - startY)}px`;
      modal.style.right = "auto";
    });

    window.addEventListener("mouseup", () => {
      isDragging = false;
    });
  }

  // Scan all existing and dynamically inserted videos
  function scanVideos() {
    document.querySelectorAll("video").forEach(attachWidgetToVideo);
  }

  scanVideos();

  // MutationObserver for SPA / dynamically injected players (Bilibili, YouTube, etc.)
  const observer = new MutationObserver(() => {
    scanVideos();
  });
  observer.observe(document.documentElement, { childList: true, subtree: true });

  // Periodically check every 2 seconds for players in shadow DOM or delayed insertion
  setInterval(scanVideos, 2000);
})();
