// GLSS Browser Extension - Background Service Worker
// Manages permissions, cross-origin media headers, and extension states.

chrome.runtime.onInstalled.addListener(() => {
  console.log("[GLSS Extension] 已成功安装并初始化。");
});

// 动态移除媒体资源的限制性 CORS 响应头，确保 WebGPU/WebGL 纹理能够无污染读取跨域视频画面
const RULE_ID_CORS = 1;

async function setupCorsRules() {
  try {
    const rules = [
      {
        id: RULE_ID_CORS,
        priority: 1,
        action: {
          type: "modifyHeaders",
          responseHeaders: [
            {
              header: "Access-Control-Allow-Origin",
              operation: "set",
              value: "*"
            },
            {
              header: "Access-Control-Allow-Methods",
              operation: "set",
              value: "GET, HEAD, OPTIONS"
            }
          ]
        },
        condition: {
          urlFilter: "*",
          resourceTypes: ["media", "xmlhttprequest", "other"]
        }
      }
    ];

    await chrome.declarativeNetRequest.updateDynamicRules({
      removeRuleIds: [RULE_ID_CORS],
      addRules: rules
    });
    console.log("[GLSS Extension] 媒体 CORS 解除规则已生效。");
  } catch (err) {
    console.warn("[GLSS Extension] 配置 CORS 规则警告:", err);
  }
}

setupCorsRules();

// 处理来自 content script 或 popup 的消息
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message.type === "GET_EXTENSION_INFO") {
    sendResponse({
      name: "GLSS 视频超分辨率与插帧",
      version: chrome.runtime.getManifest().version,
      status: "active"
    });
  }
  return true;
});
