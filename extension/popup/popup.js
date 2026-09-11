// GLSS Extension Popup Controller

document.addEventListener("DOMContentLoaded", () => {
  const toggleEnable = document.getElementById("toggle-enable");
  const selectMethod = document.getElementById("select-method");
  const selectInterp = document.getElementById("select-interp");
  const selectScale = document.getElementById("select-scale");
  const btnOpenDemo = document.getElementById("btn-open-demo");

  // Load saved settings
  chrome.storage.local.get(["enabled", "method", "interp", "scale"], (res) => {
    if (res.enabled !== undefined) toggleEnable.checked = res.enabled;
    if (res.method) selectMethod.value = res.method;
    if (res.interp) selectInterp.value = res.interp;
    if (res.scale) selectScale.value = res.scale;
  });

  toggleEnable.addEventListener("change", () => {
    chrome.storage.local.set({ enabled: toggleEnable.checked });
  });

  selectMethod.addEventListener("change", () => {
    chrome.storage.local.set({ method: selectMethod.value });
  });

  selectInterp.addEventListener("change", () => {
    chrome.storage.local.set({ interp: selectInterp.value });
  });

  selectScale.addEventListener("change", () => {
    chrome.storage.local.set({ scale: selectScale.value });
  });

  btnOpenDemo.addEventListener("click", () => {
    chrome.tabs.create({ url: chrome.runtime.getURL("demo/demo.html") });
  });
});
