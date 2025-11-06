#include "pch.h"
#include "AppSettings.h"
#include "App.h"
#include "AutoStartHelper.h"
#include "CommonSharedConstants.h"
#include "JsonHelper.h"
#include "LocalizationService.h"
#include "Logger.h"
#include "MainWindow.h"
#include "Profile.h"
#include "resource.h"
#include "ScalingMode.h"
#include "ScalingModesService.h"
#include "ShortcutHelper.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <rapidjson/prettywriter.h>
#include <ShellScalingApi.h>
#include <ShlObj.h>

using namespace winrt;
using namespace winrt::Magpie;

namespace Magpie {

// 如果配置文件和已发布的正式版本不再兼容，应提高此版本号
static constexpr uint32_t CONFIG_VERSION = 4;

_AppSettingsData::_AppSettingsData() {}

_AppSettingsData::~_AppSettingsData() {}

// 将热键存储为 uint32_t
// 不能存储为字符串，因为某些键的字符相同，如句号和小键盘的点
static uint32_t EncodeShortcut(const Shortcut& shortcut) noexcept {
    uint32_t value = shortcut.code;
    if (shortcut.win) {
        value |= 0x100;
    }
    if (shortcut.ctrl) {
        value |= 0x200;
    }
    if (shortcut.alt) {
        value |= 0x400;
    }
    if (shortcut.shift) {
        value |= 0x800;
    }
    return value;
}

static void DecodeShortcut(uint32_t value, Shortcut& shortcut) noexcept {
    if (value > 0xfff) {
        return;
    }

    shortcut.code = value & 0xff;
    shortcut.win = value & 0x100;
    shortcut.ctrl = value & 0x200;
    shortcut.alt = value & 0x400;
    shortcut.shift = value & 0x800;
}

static void WriteProfile(rapidjson::PrettyWriter<rapidjson::StringBuffer>& writer, const Profile& profile) noexcept {
    writer.StartObject();
    if (!profile.name.empty()) {
        writer.Key("name");
        writer.String(StrHelper::UTF16ToUTF8(profile.name).c_str());
        writer.Key("packaged");
        writer.Bool(profile.isPackaged);
        writer.Key("pathRule");
        writer.String(StrHelper::UTF16ToUTF8(profile.pathRule).c_str());
        writer.Key("classNameRule");
        writer.String(StrHelper::UTF16ToUTF8(profile.classNameRule).c_str());
        writer.Key("launcherPath");
        writer.String(StrHelper::UTF16ToUTF8(profile.launcherPath.native()).c_str());
        writer.Key("autoScale");
        writer.Uint((uint32_t)profile.autoScale);
        writer.Key("launchParameters");
        writer.String(StrHelper::UTF16ToUTF8(profile.launchParameters).c_str());
    }

    writer.Key("scalingMode");
    writer.Int(profile.scalingMode);
    writer.Key("captureMethod");
    writer.Uint((uint32_t)profile.captureMethod);
    writer.Key("multiMonitorUsage");
    writer.Uint((uint32_t)profile.multiMonitorUsage);

    writer.Key("initialWindowedScaleFactor");
    writer.Uint((uint32_t)profile.initialWindowedScaleFactor);
    writer.Key("customInitialWindowedScaleFactor");
    writer.Double(profile.customInitialWindowedScaleFactor);

    writer.Key("graphicsCardId");
    writer.StartObject();
    writer.Key("idx");
    writer.Int(profile.graphicsCardId.idx);
    writer.Key("vendorId");
    writer.Uint(profile.graphicsCardId.vendorId);
    writer.Key("deviceId");
    writer.Uint(profile.graphicsCardId.deviceId);
    writer.EndObject();
    writer.Key("frameRateLimiterEnabled");
    writer.Bool(profile.isFrameRateLimiterEnabled);
    writer.Key("maxFrameRate");
    writer.Double(profile.maxFrameRate);

    writer.Key("3DGameMode");
    writer.Bool(profile.Is3DGameMode());
    writer.Key("captureTitleBar");
    writer.Bool(profile.IsCaptureTitleBar());
    writer.Key("adjustCursorSpeed");
    writer.Bool(profile.IsAdjustCursorSpeed());
    writer.Key("disableDirectFlip");
    writer.Bool(profile.IsDirectFlipDisabled());

    writer.Key("cursorScaling");
    writer.Uint((uint32_t)profile.cursorScaling);
    writer.Key("customCursorScaling");
    writer.Double(profile.customCursorScaling);
    writer.Key("cursorInterpolationMode");
    writer.Uint((uint32_t)profile.cursorInterpolationMode);
    writer.Key("autoHideCursorEnabled");
    writer.Bool(profile.isAutoHideCursorEnabled);
    writer.Key("autoHideCursorDelay");
    writer.Double(profile.autoHideCursorDelay);

    writer.Key("croppingEnabled");
    writer.Bool(profile.isCroppingEnabled);
    writer.Key("cropping");
    writer.StartObject();
    writer.Key("left");
    writer.Double(profile.cropping.Left);
    writer.Key("top");
    writer.Double(profile.cropping.Top);
    writer.Key("right");
    writer.Double(profile.cropping.Right);
    writer.Key("bottom");
    writer.Double(profile.cropping.Bottom);
    writer.EndObject();

    writer.EndObject();
}

static void ReplaceIcon(HINSTANCE hInst, HWND hWnd, bool large) noexcept {
    HICON hIconApp = NULL;
    LoadIconMetric(hInst, MAKEINTRESOURCE(IDI_APP), large ? LIM_LARGE : LIM_SMALL, &hIconApp);
    HICON hIconOld = (HICON)SendMessage(hWnd, WM_SETICON, large ? ICON_BIG : ICON_SMALL, (LPARAM)hIconApp);
    if (hIconOld) {
        DestroyIcon(hIconOld);
    }
}

static HRESULT CALLBACK TaskDialogCallback(
    HWND hWnd,
    UINT msg,
    WPARAM /*wParam*/,
    LPARAM /*lParam*/,
    LONG_PTR /*lpRefData*/
) {
    if (msg == TDN_CREATED) {
        HINSTANCE hInst = GetModuleHandle(nullptr);
        ReplaceIcon(hInst, hWnd, true);
        ReplaceIcon(hInst, hWnd, false);

        INT_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);
        SetWindowLongPtr(hWnd, GWL_STYLE, style & ~WS_SYSMENU);
    }

    return S_OK;
}

static void ShowErrorMessage(const wchar_t* mainInstruction, const wchar_t* content) noexcept {
    ResourceLoader resourceLoader =
        ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
    const hstring errorStr = resourceLoader.GetString(L"AppSettings_Dialog_Error");
    const hstring exitStr = resourceLoader.GetString(L"AppSettings_Dialog_Exit");

    TASKDIALOG_BUTTON button{ IDCANCEL, exitStr.c_str() };
    TASKDIALOGCONFIG tdc{
        .cbSize = sizeof(TASKDIALOGCONFIG),
        .dwFlags = TDF_SIZE_TO_CONTENT,
        .pszWindowTitle = errorStr.c_str(),
        .pszMainIcon = TD_ERROR_ICON,
        .pszMainInstruction = mainInstruction,
        .pszContent = content,
        .cButtons = 1,
        .pButtons = &button,
        .pfCallback = TaskDialogCallback
    };
    TaskDialogIndirect(&tdc, nullptr, nullptr, nullptr);
}

AppSettings::~AppSettings() {}

bool AppSettings::Initialize() noexcept {
    Logger& logger = Logger::Get();

    _isPortableMode = Win32Helper::FileExists(StrHelper::Concat(
        CommonSharedConstants::CONFIG_DIR, L"\\", CommonSharedConstants::CONFIG_FILENAME).c_str());

    std::filesystem::path existingConfigPath;
    if (!_UpdateConfigPath(&existingConfigPath)) {
        logger.Error("_UpdateConfigPath 失败");
        return false;
    }

    logger.Info(StrHelper::Concat("便携模式: ", _isPortableMode ? "是" : "否"));

    if (existingConfigPath.empty()) {
        logger.Info("不存在配置文件");
        _SetDefaultScalingModes();
        _SetDefaultShortcuts();
        SaveAsync();
        return true;
    }

    std::string configText;
    if (!Win32Helper::ReadTextFile(existingConfigPath.c_str(), configText)) {
        logger.Error("读取配置文件失败");
        ResourceLoader resourceLoader =
            ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
        hstring title = resourceLoader.GetString(L"AppSettings_ErrorDialog_ReadFailed");
        hstring content = resourceLoader.GetString(L"AppSettings_ErrorDialog_ConfigLocation");
        ShowErrorMessage(title.c_str(),
            fmt::format(fmt::runtime(std::wstring_view(content)), existingConfigPath.native()).c_str());
        return false;
    }

    if (configText.empty()) {
        Logger::Get().Info("配置文件为空");
        _SetDefaultScalingModes();
        _SetDefaultShortcuts();
        SaveAsync();
        return true;
    }

    rapidjson::Document doc;
    doc.ParseInsitu(configText.data());
    if (doc.HasParseError()) {
        Logger::Get().Error(fmt::format("解析配置失败\n\t错误码: {}", (int)doc.GetParseError()));
        ResourceLoader resourceLoader =
            ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
        hstring title = resourceLoader.GetString(L"AppSettings_ErrorDialog_NotValidJson");
        hstring content = resourceLoader.GetString(L"AppSettings_ErrorDialog_ConfigLocation");
        ShowErrorMessage(title.c_str(),
            fmt::format(fmt::runtime(std::wstring_view(content)), existingConfigPath.native()).c_str());
        return false;
    }

    if (!doc.IsObject()) {
        Logger::Get().Error("配置文件根元素不是 Object");
        ResourceLoader resourceLoader =
            ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
        hstring title = resourceLoader.GetString(L"AppSettings_ErrorDialog_ParseFailed");
        hstring content = resourceLoader.GetString(L"AppSettings_ErrorDialog_ConfigLocation");
        ShowErrorMessage(title.c_str(),
            fmt::format(fmt::runtime(std::wstring_view(content)), existingConfigPath.native()).c_str());
        return false;
    }

    _LoadSettings(((const rapidjson::Document&)doc).GetObj());

    if (_SetDefaultShortcuts() || !Win32Helper::FileExists(_configPath.c_str())) {
        SaveAsync();
    }

    return true;
}

bool AppSettings::Save() noexcept {
    _UpdateWindowPlacement();
    return _Save(*this);
}

fire_and_forget AppSettings::SaveAsync() noexcept {
    _UpdateWindowPlacement();

    _AppSettingsData data = *this;
    co_await resume_background();

    _Save(data);
}

void AppSettings::IsPortableMode(bool value) noexcept {
    if (_isPortableMode == value) {
        return;
    }

    if (!value) {
        if (!DeleteFile((_configDir / CommonSharedConstants::CONFIG_FILENAME).c_str())) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) {
                Logger::Get().Win32Error("删除本地配置文件失败");
                return;
            }
        }
    }

    _isPortableMode = value;

    if (_UpdateConfigPath()) {
        Logger::Get().Info(value ? "已开启便携模式" : "已关闭便携模式");
        SaveAsync();
    } else {
        Logger::Get().Error(value ? "开启便携模式失败" : "关闭便携模式失败");
        _isPortableMode = !value;
    }
}

void AppSettings::Language(int value) {
    if (_language == value) {
        return;
    }

    _language = value;
    SaveAsync();
}

void AppSettings::Theme(AppTheme value) {
    if (_theme == value) {
        return;
    }

    _theme = value;
    ThemeChanged.Invoke(value);

    SaveAsync();
}

void AppSettings::SetShortcut(ShortcutAction action, const Shortcut& value) {
    if (_shortcuts[(size_t)action] == value) {
        return;
    }

    _shortcuts[(size_t)action] = value;
    Logger::Get().Info(fmt::format("热键 {} 已更改为 {}", ShortcutHelper::ToString(action), StrHelper::UTF16ToUTF8(value.ToString())));
    ShortcutChanged.Invoke(action);

    SaveAsync();
}

void AppSettings::CountdownSeconds(uint32_t value) noexcept {
    if (_countdownSeconds == value) {
        return;
    }

    _countdownSeconds = value;
    CountdownSecondsChanged.Invoke(value);

    SaveAsync();
}

void AppSettings::IsDeveloperMode(bool value) noexcept {
    _isDeveloperMode = value;
    if (!value) {
        _isDebugMode = false;
        _isBenchmarkMode = false;
        _isEffectCacheDisabled = false;
        _isFontCacheDisabled = false;
        _isSaveEffectSources = false;
        _isWarningsAreErrors = false;
        _duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Dynamic;
        _isStatisticsForDynamicDetectionEnabled = false;
        _isFP16Disabled = false;
    }

    SaveAsync();
}

void AppSettings::IsAlwaysRunAsAdmin(bool value) noexcept {
    if (_isAlwaysRunAsAdmin == value) {
        return;
    }

    _isAlwaysRunAsAdmin = value;
    SaveAsync();

    if (AutoStartHelper::IsAutoStartEnabled()) {
        AutoStartHelper::EnableAutoStart(value);
    }
}

void AppSettings::IsShowNotifyIcon(bool value) noexcept {
    if (_isShowNotifyIcon == value) {
        return;
    }

    _isShowNotifyIcon = value;
    IsShowNotifyIconChanged.Invoke(value);

    SaveAsync();
}

void AppSettings::IsSimpleMode(bool value) noexcept {
    if (_isSimpleMode == value) {
        return;
    }

    _isSimpleMode = value;
    SaveAsync();
}

static std::filesystem::path GetSystemScreenshotsDir() noexcept {
    wil::unique_cotaskmem_string folder;
    HRESULT hr = SHGetKnownFolderPath(
        FOLDERID_Screenshots, KF_FLAG_DEFAULT, NULL, folder.put());
    if (SUCCEEDED(hr)) {
        return folder.get();
    }

    hr = SHGetKnownFolderPath(
        FOLDERID_Pictures, KF_FLAG_DEFAULT, NULL, folder.put());
    if (SUCCEEDED(hr)) {
        return StrHelper::Concat(folder.get(), L"\\Screenshots");
    }

    hr = SHGetKnownFolderPath(
        FOLDERID_Profile, KF_FLAG_DEFAULT, NULL, folder.put());
    if (SUCCEEDED(hr)) {
        return StrHelper::Concat(folder.get(), L"\\Pictures\\Screenshots");
    }

    Logger::Get().ComError("SHGetKnownFolderPath 失败", hr);
    return {};
}

static bool IsSubfolder(const std::wstring& sub, const std::wstring& parent) noexcept {
    if (!sub.starts_with(parent)) {
        return false;
    }

    if (parent.size() == sub.size()) {
        return true;
    }

    return sub[parent.size()] == L'\\';
}

// 失败时返回空字符串
std::filesystem::path AppSettings::ScreenshotsDir() const noexcept {
    if (_screenshotsDir.empty()) {
        return GetSystemScreenshotsDir();
    } else if (_screenshotsDir.is_relative()) {
        std::wstring workingDir;
        HRESULT hr = wil::GetCurrentDirectoryW(workingDir);
        if (FAILED(hr)) {
            Logger::Get().ComError("wil::GetCurrentDirectoryW 失败", hr);
            return {};
        }

        if (_screenshotsDir == L".") {
            return std::filesystem::path(std::move(workingDir));
        } else {
            return (std::filesystem::path(std::move(workingDir)) / _screenshotsDir).lexically_normal();
        }
    } else {
        return _screenshotsDir;
    }
}

void AppSettings::ScreenshotsDir(const std::filesystem::path& value) noexcept {
    assert(!value.empty());

    if (value == GetSystemScreenshotsDir()) {
        _screenshotsDir.clear();
    } else {
        std::wstring workingDir;
        HRESULT hr = wil::GetCurrentDirectoryW(workingDir);
        if (FAILED(hr)) {
            Logger::Get().ComError("wil::GetCurrentDirectoryW 失败", hr);
            return;
        }

        if (IsSubfolder(value, workingDir)) {
            if (value.native().size() == workingDir.size()) {
                _screenshotsDir = L".";
            } else {
                _screenshotsDir = StrHelper::Concat(
                    L".",
                    std::wstring(value.native().begin() + workingDir.size(), value.native().end())
                );
            }
        } else {
            _screenshotsDir = value;
        }
    }

    SaveAsync();
}

void AppSettings::_UpdateWindowPlacement() noexcept {
    // Minimal implementation: do not modify stored placement if we can't access window.
    // Keeping this as a no-op avoids surprising side-effects while allowing linking.
}

bool AppSettings::_UpdateConfigPath(std::filesystem::path* existingConfigPath) noexcept {
    // Determine config directory order:
    // 1. Portable: <working dir>\config\config.json
    // 2. Local app data: %LocalAppData%\Magpie\config\v<CONFIG_VERSION>\config.json
    // 3. Local app data fallback: %LocalAppData%\Magpie\config\config.json

    std::wstring workingDir;
    if (FAILED(wil::GetCurrentDirectoryW(workingDir))) {
        Logger::Get().ComError("wil::GetCurrentDirectoryW 失败", E_FAIL);
        return false;
    }

    // Check portable location (relative "config\\config.json")
    std::wstring portableRelative = StrHelper::Concat(CommonSharedConstants::CONFIG_DIR, L"\\", CommonSharedConstants::CONFIG_FILENAME);
    if (Win32Helper::FileExists(portableRelative.c_str())) {
        _isPortableMode = true;
        _configDir = std::filesystem::path(workingDir) / CommonSharedConstants::CONFIG_DIR;
        _configPath = _configDir / CommonSharedConstants::CONFIG_FILENAME;
        if (existingConfigPath) {
            *existingConfigPath = _configPath;
        }
        return true;
    }

    // Non-portable: use LocalAppData\Magpie\config\v<CONFIG_VERSION>
    wil::unique_cotaskmem_string folder;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, NULL, folder.put());
    if (SUCCEEDED(hr)) {
        std::filesystem::path base = std::filesystem::path(folder.get()) / L"Magpie" / CommonSharedConstants::CONFIG_DIR / (L"v" + std::to_wstring(CONFIG_VERSION));
        std::filesystem::path candidate = base / CommonSharedConstants::CONFIG_FILENAME;
        if (Win32Helper::FileExists(candidate.native().c_str())) {
            _isPortableMode = false;
            _configDir = base;
            _configPath = candidate;
            if (existingConfigPath) {
                *existingConfigPath = _configPath;
            }
            return true;
        }

        // fallback: check without versioned folder
        std::filesystem::path fallbackBase = std::filesystem::path(folder.get()) / L"Magpie" / CommonSharedConstants::CONFIG_DIR;
        std::filesystem::path fallback = fallbackBase / CommonSharedConstants::CONFIG_FILENAME;
        if (Win32Helper::FileExists(fallback.native().c_str())) {
            _isPortableMode = false;
            _configDir = fallbackBase;
            _configPath = fallback;
            if (existingConfigPath) {
                *existingConfigPath = _configPath;
            }
            return true;
        }

        // Default to versioned base even if file doesn't exist yet
        _isPortableMode = false;
        _configDir = base;
        _configPath = base / CommonSharedConstants::CONFIG_FILENAME;
        return true;
    }

    Logger::Get().ComError("SHGetKnownFolderPath 失败", hr);
    return false;
}

bool AppSettings::_Save(const _AppSettingsData& data) noexcept {
    if (!Win32Helper::CreateDir(data._configDir.native(), true)) {
        Logger::Get().Win32Error("创建配置文件夹失败");
        return false;
    }

    rapidjson::StringBuffer json;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(json);
    writer.StartObject();

    writer.Key("language");
    if (_language < 0) {
        writer.String("");
    } else {
        const wchar_t* language = LocalizationService::SupportedLanguages()[_language];
        writer.String(StrHelper::UTF16ToUTF8(language).c_str());
    }

    writer.Key("theme");
    writer.Uint((uint32_t)data._theme);

    writer.Key("windowPos");
    writer.StartObject();
    writer.Key("centerX");
    writer.Double(data._mainWindowCenter.X);
    writer.Key("centerY");
    writer.Double(data._mainWindowCenter.Y);
    writer.Key("width");
    writer.Double(data._mainWindowSizeInDips.Width);
    writer.Key("height");
    writer.Double(data._mainWindowSizeInDips.Height);
    writer.Key("maximized");
    writer.Bool(data._isMainWindowMaximized);
    writer.EndObject();

    writer.Key("shortcuts");
    writer.StartObject();
    writer.Key("scale");
    writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::Scale]));
    writer.Key("windowedModeScale");
    writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::WindowedModeScale]));
    writer.Key("toolbar");
    writer.Uint(EncodeShortcut(data._shortcuts[(size_t)ShortcutAction::Toolbar]));
    writer.EndObject();

    writer.Key("countdownSeconds");
    writer.Uint(data._countdownSeconds);
    writer.Key("developerMode");
    writer.Bool(data._isDeveloperMode);
    writer.Key("debugMode");
    writer.Bool(data._isDebugMode);
    writer.Key("benchmarkMode");
    writer.Bool(data._isBenchmarkMode);
    writer.Key("disableEffectCache");
    writer.Bool(data._isEffectCacheDisabled);
    writer.Key("disableFontCache");
    writer.Bool(data._isFontCacheDisabled);
    writer.Key("saveEffectSources");
    writer.Bool(data._isSaveEffectSources);
    writer.Key("warningsAreErrors");
    writer.Bool(data._isWarningsAreErrors);
    writer.Key("allowScalingMaximized");
    writer.Bool(data._isAllowScalingMaximized);
    writer.Key("simulateExclusiveFullscreen");
    writer.Bool(data._isSimulateExclusiveFullscreen);
    writer.Key("alwaysRunAsAdmin");
    writer.Bool(data._isAlwaysRunAsAdmin);
    writer.Key("showNotifyIcon");
    writer.Bool(data._isShowNotifyIcon);
    writer.Key("inlineParams");
    writer.Bool(data._isInlineParams);
    writer.Key("autoCheckForUpdates");
    writer.Bool(data._isAutoCheckForUpdates);
    writer.Key("checkForPreviewUpdates");
    writer.Bool(data._isCheckForPreviewUpdates);
    writer.Key("updateCheckDate");
    writer.Int64(data._updateCheckDate.time_since_epoch().count());
    writer.Key("duplicateFrameDetectionMode");
    writer.Uint((uint32_t)data._duplicateFrameDetectionMode);
    writer.Key("enableStatisticsForDynamicDetection");
    writer.Bool(data._isStatisticsForDynamicDetectionEnabled);
    writer.Key("minFrameRate");
    writer.Double(data._minFrameRate);
    writer.Key("disableFP16");
    writer.Bool(data._isFP16Disabled);

    writer.Key("simpleMode");
    writer.Bool(data._isSimpleMode);

    ScalingModesService::Get().Export(writer);

    writer.Key("profiles");
    writer.StartArray();
    WriteProfile(writer, data._defaultProfile);
    for (const Profile& rule : data._profiles) {
        WriteProfile(writer, rule);
    }
    writer.EndArray();

    writer.Key("overlay");
    writer.StartObject();
    writer.Key("fullscreenInitialToolbarState");
    writer.Uint((uint32_t)_fullscreenInitialToolbarState);
    writer.Key("windowedInitialToolbarState");
    writer.Uint((uint32_t)_windowedInitialToolbarState);
    writer.Key("screenshotsDir");
    writer.String(StrHelper::UTF16ToUTF8(_screenshotsDir.native()).c_str());
    writer.Key("windows");
    writer.StartObject();
    for (const auto& [name, windowOption] : _overlayOptions.windows) {
        writer.Key(name.c_str());
        writer.StartObject();
        writer.Key("hArea");
        writer.Uint(windowOption.hArea);
        writer.Key("vArea");
        writer.Uint(windowOption.vArea);
        writer.Key("hPos");
        writer.Double(windowOption.hPos);
        writer.Key("vPos");
        writer.Double(windowOption.vPos);
        writer.EndObject();
    }
    writer.EndObject();
    writer.EndObject();

    writer.EndObject();

    auto lock = _saveLock.lock_exclusive();
    if (!Win32Helper::WriteTextFile(data._configPath.c_str(), { json.GetString(), json.GetLength() })) {
        Logger::Get().Error("保存配置失败");
        return false;
    }

    return true;
}

// 永远不会失败，遇到不合法的配置项时静默忽略
void AppSettings::_LoadSettings(const rapidjson::GenericObject<true, rapidjson::Value>& root) noexcept {
    {
        std::wstring language;
        JsonHelper::ReadString(root, "language", language);
        if (language.empty()) {
            _language = -1;
        } else {
            StrHelper::ToLowerCase(language);
            std::span<const wchar_t*> languages = LocalizationService::SupportedLanguages();
            auto it = std::find(languages.begin(), languages.end(), language);
            if (it == languages.end()) {
                _language = -1;
            } else {
                _language = int(it - languages.begin());
            }
        }
    }

    {
        uint32_t theme = (uint32_t)AppTheme::System;
        JsonHelper::ReadUInt(root, "theme", theme);
        if (theme <= 2) {
            _theme = (AppTheme)theme;
        } else {
            _theme = AppTheme::System;
        }
    }

    auto windowPosNode = root.FindMember("windowPos");
    if (windowPosNode != root.MemberEnd() && windowPosNode->value.IsObject()) {
        auto windowPosObj = windowPosNode->value.GetObj();

        Point center{};
        Size size{};
        if (JsonHelper::ReadFloat(windowPosObj, "centerX", center.X, true) &&
            JsonHelper::ReadFloat(windowPosObj, "centerY", center.Y, true) &&
            JsonHelper::ReadFloat(windowPosObj, "width", size.Width, true) &&
            JsonHelper::ReadFloat(windowPosObj, "height", size.Height, true)) {
            _mainWindowCenter = center;
            _mainWindowSizeInDips = size;
        } else {
            int x = 0;
            int y = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            if (JsonHelper::ReadInt(windowPosObj, "x", x, true) &&
                JsonHelper::ReadInt(windowPosObj, "y", y, true) &&
                JsonHelper::ReadUInt(windowPosObj, "width", width, true) &&
                JsonHelper::ReadUInt(windowPosObj, "height", height, true)) {
                _mainWindowCenter = {
                    x + width / 2.0f,
                    y + height / 2.0f
                };

                const HMONITOR hMon = MonitorFromPoint(
                    { std::lroundf(_mainWindowCenter.X), std::lroundf(_mainWindowCenter.Y) },
                    MONITOR_DEFAULTTOPRIMARY
                );

                UINT dpi = USER_DEFAULT_SCREEN_DPI;
                GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpi, &dpi);
                const float dpiFactor = dpi / float(USER_DEFAULT_SCREEN_DPI);
                _mainWindowSizeInDips = {
                    width / dpiFactor,
                    height / dpiFactor
                };
            }
        }

        JsonHelper::ReadBool(windowPosObj, "maximized", _isMainWindowMaximized);
    }

    auto shortcutsNode = root.FindMember("shortcuts");
    if (shortcutsNode == root.MemberEnd()) {
        shortcutsNode= root.FindMember("hotkeys");
    }
    if (shortcutsNode != root.MemberEnd() && shortcutsNode->value.IsObject()) {
        auto shortcutsObj = shortcutsNode->value.GetObj();

        auto scaleNode = shortcutsObj.FindMember("scale");
        if (scaleNode != shortcutsObj.MemberEnd() && scaleNode->value.IsUint()) {
            DecodeShortcut(scaleNode->value.GetUint(), _shortcuts[(size_t)ShortcutAction::Scale]);
        }

        auto windowedModeScaleNode = shortcutsObj.FindMember("windowedModeScale");
        if (windowedModeScaleNode != shortcutsObj.MemberEnd() && windowedModeScaleNode->value.IsUint()) {
            DecodeShortcut(windowedModeScaleNode->value.GetUint(), _shortcuts[(size_t)ShortcutAction::WindowedModeScale]);
        }

        auto toolbarNode = shortcutsObj.FindMember("toolbar");
        if (toolbarNode == shortcutsObj.MemberEnd()) {
            toolbarNode = shortcutsObj.FindMember("overlay");
        }

        if (toolbarNode != shortcutsObj.MemberEnd() && toolbarNode->value.IsUint()) {
            DecodeShortcut(toolbarNode->value.GetUint(), _shortcuts[(size_t)ShortcutAction::Toolbar]);
        }
    }

    if (!JsonHelper::ReadUInt(root, "countdownSeconds", _countdownSeconds, true)) {
        JsonHelper::ReadUInt(root, "downCount", _countdownSeconds);
    }
    if (_countdownSeconds == 0 || _countdownSeconds > 5) {
        _countdownSeconds = 3;
    }

    JsonHelper::ReadBool(root, "developerMode", _isDeveloperMode);
    JsonHelper::ReadBool(root, "debugMode", _isDebugMode);
    JsonHelper::ReadBool(root, "benchmarkMode", _isBenchmarkMode);
    JsonHelper::ReadBool(root, "disableEffectCache", _isEffectCacheDisabled);
    JsonHelper::ReadBool(root, "disableFontCache", _isFontCacheDisabled);
    JsonHelper::ReadBool(root, "saveEffectSources", _isSaveEffectSources);
    JsonHelper::ReadBool(root, "warningsAreErrors", _isWarningsAreErrors);
    JsonHelper::ReadBool(root, "allowScalingMaximized", _isAllowScalingMaximized);
    JsonHelper::ReadBool(root, "simulateExclusiveFullscreen", _isSimulateExclusiveFullscreen);
    if (!JsonHelper::ReadBool(root, "alwaysRunAsAdmin", _isAlwaysRunAsAdmin, true)) {
        JsonHelper::ReadBool(root, "alwaysRunAsElevated", _isAlwaysRunAsAdmin);
    }
    if (!JsonHelper::ReadBool(root, "showNotifyIcon", _isShowNotifyIcon, true)) {
        JsonHelper::ReadBool(root, "showTrayIcon", _isShowNotifyIcon);
    }
    JsonHelper::ReadBool(root, "inlineParams", _isInlineParams);
    JsonHelper::ReadBool(root, "autoCheckForUpdates", _isAutoCheckForUpdates);
    JsonHelper::ReadBool(root, "checkForPreviewUpdates", _isCheckForPreviewUpdates);

    int64_t d = 0;
    JsonHelper::ReadInt64(root, "updateCheckDate", d);
    using std::chrono::system_clock;
    _updateCheckDate = system_clock::time_point(system_clock::duration(d));

    uint32_t duplicateFrameDetectionMode = (uint32_t)DuplicateFrameDetectionMode::Dynamic;
    JsonHelper::ReadUInt(root, "duplicateFrameDetectionMode", duplicateFrameDetectionMode);
    if (duplicateFrameDetectionMode > 2) {
        duplicateFrameDetectionMode = (uint32_t)DuplicateFrameDetectionMode::Dynamic;
    }
    _duplicateFrameDetectionMode = (::Magpie::DuplicateFrameDetectionMode)duplicateFrameDetectionMode;

    JsonHelper::ReadBool(root, "enableStatisticsForDynamicDetection", _isStatisticsForDynamicDetectionEnabled);
    JsonHelper::ReadFloat(root, "minFrameRate", _minFrameRate);
    JsonHelper::ReadBool(root, "disableFP16", _isFP16Disabled);

    JsonHelper::ReadBool(root, "simpleMode", _isSimpleMode);

    [[maybe_unused]] bool result = ScalingModesService::Get().Import(root, true);
    assert(result);

    auto scaleProfilesNode = root.FindMember("profiles");
    if (scaleProfilesNode == root.MemberEnd()) {
        scaleProfilesNode = root.FindMember("scalingProfiles");
    }
    if (scaleProfilesNode != root.MemberEnd() && scaleProfilesNode->value.IsArray()) {
        auto scaleProfilesArray = scaleProfilesNode->value.GetArray();

        const rapidjson::SizeType size = scaleProfilesArray.Size();
        if (size > 0) {
            if (scaleProfilesArray[0].IsObject()) {
                _LoadProfile(scaleProfilesArray[0].GetObj(), _defaultProfile, true);
            }

            if (size > 1) {
                _profiles.reserve((size_t)size - 1);
                for (rapidjson::SizeType i = 1; i < size; ++i) {
                    if (!scaleProfilesArray[i].IsObject()) {
                        continue;
                    }

                    Profile& rule = _profiles.emplace_back();
                    if (!_LoadProfile(scaleProfilesArray[i].GetObj(), rule)) {
                        _profiles.pop_back();
                        continue;
                    }
                }
            }
        }
    }

    auto overlayNode = root.FindMember("overlay");
    if (overlayNode != root.MemberEnd() && overlayNode->value.IsObject()) {
        auto overlayObj = overlayNode->value.GetObj();

        uint32_t initialToolbarState = (uint32_t)ToolbarState::AutoHide;
        if (JsonHelper::ReadUInt(overlayObj, "fullscreenInitialToolbarState", initialToolbarState, true)) {
            if (initialToolbarState >= (uint32_t)ToolbarState::COUNT) {
                initialToolbarState = (uint32_t)ToolbarState::AutoHide;
            }
            _fullscreenInitialToolbarState = (ToolbarState)initialToolbarState;

            initialToolbarState = (uint32_t)ToolbarState::AutoHide;
            JsonHelper::ReadUInt(overlayObj, "windowedInitialToolbarState", initialToolbarState);
            if (initialToolbarState >= (uint32_t)ToolbarState::COUNT) {
                initialToolbarState = (uint32_t)ToolbarState::AutoHide;
            }
            _windowedInitialToolbarState = (ToolbarState)initialToolbarState;
        } else {
            JsonHelper::ReadUInt(overlayObj, "initialToolbarState", initialToolbarState);
            if (initialToolbarState >= (uint32_t)ToolbarState::COUNT) {
                initialToolbarState = (uint32_t)ToolbarState::AutoHide;
            }
            _fullscreenInitialToolbarState = (ToolbarState)initialToolbarState;
            _windowedInitialToolbarState = (ToolbarState)initialToolbarState;
        }

        std::wstring value;
        JsonHelper::ReadString(overlayObj, "screenshotsDir", value);
        _screenshotsDir = std::move(value);

        auto windowsNode = overlayObj.FindMember("windows");
        if (windowsNode != overlayObj.MemberEnd() && windowsNode->value.IsObject()) {
            auto windowsObj = windowsNode->value.GetObj();

            const rapidjson::SizeType size = windowsObj.MemberCount();
            if (size > 0) {
                _overlayOptions.windows.reserve(size);

                for (const auto& windowOptionPair : windowsObj) {
                    if (!windowOptionPair.value.IsObject()) {
                        continue;
                    }

                    auto windowOptionObj = windowOptionPair.value.GetObj();

                    OverlayWindowOption& windowOption = _overlayOptions.windows[windowOptionPair.name.GetString()];
                    JsonHelper::ReadUInt16(windowOptionObj, "hArea", windowOption.hArea);
                    JsonHelper::ReadUInt16(windowOptionObj, "vArea", windowOption.vArea);
                    JsonHelper::ReadFloat(windowOptionObj, "hPos", windowOption.hPos);
                    JsonHelper::ReadFloat(windowOptionObj, "vPos", windowOption.vPos);
                }
            }
        }
    }
}

bool AppSettings::_LoadProfile(
    const rapidjson::GenericObject<true, rapidjson::Value>& profileObj,
    Profile& profile,
    bool isDefault
) const noexcept {
    if (!isDefault) {
        if (!JsonHelper::ReadString(profileObj, "name", profile.name, true)) {
            return false;
        }

        std::wstring_view nameView(profile.name);
        StrHelper::Trim(nameView);
        if (nameView.empty()) {
            return false;
        }

        if (!JsonHelper::ReadBool(profileObj, "packaged", profile.isPackaged, true)) {
            return false;
        }

        if (!JsonHelper::ReadString(profileObj, "pathRule", profile.pathRule, true)
            || profile.pathRule.empty()) {
            return false;
        }

        if (!JsonHelper::ReadString(profileObj, "classNameRule", profile.classNameRule, true)
            || profile.classNameRule.empty()) {
            return false;
        }

        std::wstring tmp;
        JsonHelper::ReadString(profileObj, "launcherPath", tmp);
        profile.launcherPath = std::move(tmp);

        if (!profile.launcherPath.empty() && profile.launcherPath.is_relative()) {
            std::filesystem::path exePath(profile.pathRule);
            profile.launcherPath = (exePath.parent_path() / profile.launcherPath).lexically_normal();
        }

        auto autoScaleNode = profileObj.FindMember("autoScale");
        if (autoScaleNode != profileObj.MemberEnd()) {
            if (autoScaleNode->value.IsUint()) {
                uint32_t value = autoScaleNode->value.GetUint();
                if (value >= (uint32_t)AutoScale::COUNT) {
                    value = (uint32_t)AutoScale::Disabled;
                }
                profile.autoScale = (AutoScale)value;
            } else if (autoScaleNode->value.IsBool()) {
                profile.autoScale = autoScaleNode->value.GetBool() ? AutoScale::Fullscreen : AutoScale::Disabled;
            }
        }

        JsonHelper::ReadString(profileObj, "launchParameters", profile.launchParameters);
    }

    JsonHelper::ReadInt(profileObj, "scalingMode", profile.scalingMode);
    if (profile.scalingMode < -1 || profile.scalingMode >= (int)_scalingModes.size()) {
        profile.scalingMode = -1;
    }

    uint32_t captureMethod = (uint32_t)CaptureMethod::GraphicsCapture;
    if (!JsonHelper::ReadUInt(profileObj, "captureMethod", captureMethod, true)) {
        JsonHelper::ReadUInt(profileObj, "captureMode", captureMethod);
    }

    if (captureMethod >= (uint32_t)CaptureMethod::COUNT) {
        captureMethod = (uint32_t)CaptureMethod::GraphicsCapture;
    } else if (captureMethod == (uint32_t)CaptureMethod::DesktopDuplication) {
        if (!Win32Helper::GetOSVersion().Is20H1OrNewer()) {
            captureMethod = (uint32_t)CaptureMethod::GraphicsCapture;
        }
    }
    profile.captureMethod = (CaptureMethod)captureMethod;

    uint32_t multiMonitorUsage = (uint32_t)MultiMonitorUsage::Closest;
    JsonHelper::ReadUInt(profileObj, "multiMonitorUsage", multiMonitorUsage);
    if (multiMonitorUsage >= (uint32_t)MultiMonitorUsage::COUNT) {
        multiMonitorUsage = (uint32_t)MultiMonitorUsage::Closest;
    }
    profile.multiMonitorUsage = (MultiMonitorUsage)multiMonitorUsage;

    uint32_t factor = (uint32_t)InitialWindowedScaleFactor::Auto;
    JsonHelper::ReadUInt(profileObj, "initialWindowedScaleFactor", factor);
    if (factor >= (uint32_t)InitialWindowedScaleFactor::COUNT) {
        factor = (uint32_t)InitialWindowedScaleFactor::Auto;
    }
    profile.initialWindowedScaleFactor = (InitialWindowedScaleFactor)factor;

    JsonHelper::ReadFloat(profileObj, "customInitialWindowedScaleFactor", profile.customInitialWindowedScaleFactor);
    if (profile.customInitialWindowedScaleFactor < 1.0f) {
        profile.customInitialWindowedScaleFactor = 1.0f;
    }

    auto graphicsCardIdNode = profileObj.FindMember("graphicsCardId");
    if (graphicsCardIdNode == profileObj.end()) {
        int graphicsCardIdx = -1;
        if (!JsonHelper::ReadInt(profileObj, "graphicsCard", graphicsCardIdx, true)) {
            uint32_t graphicsAdater = 0;
            JsonHelper::ReadUInt(profileObj, "graphicsAdapter", graphicsAdater);
            graphicsCardIdx = (int)graphicsAdater - 1;
        }
        profile.graphicsCardId.idx = graphicsCardIdx;
    } else if (graphicsCardIdNode->value.IsObject()) {
        auto graphicsCardIdObj = graphicsCardIdNode->value.GetObj();
        auto idxNode = graphicsCardIdObj.FindMember("idx");
        if (idxNode != graphicsCardIdObj.end() && idxNode->value.IsInt()) {
            profile.graphicsCardId.idx = idxNode->value.GetInt();
        }
        auto vendorIdNode = graphicsCardIdObj.FindMember("vendorId");
        if (vendorIdNode != graphicsCardIdObj.end() && vendorIdNode->value.IsUint()) {
            profile.graphicsCardId.vendorId = vendorIdNode->value.GetUint();
        }
        auto deviceIdNode = graphicsCardIdObj.FindMember("deviceId");
        if (deviceIdNode != graphicsCardIdObj.end() && deviceIdNode->value.IsUint()) {
            profile.graphicsCardId.deviceId = deviceIdNode->value.GetUint();
        }
    }

    JsonHelper::ReadBool(profileObj, "frameRateLimiterEnabled", profile.isFrameRateLimiterEnabled);
    JsonHelper::ReadFloat(profileObj, "maxFrameRate", profile.maxFrameRate);
    if (profile.maxFrameRate <= 10.0f - FLOAT_EPSILON<float> || profile.maxFrameRate >= 1000.0f + FLOAT_EPSILON<float>) {
        profile.maxFrameRate = 60.0f;
    }

    JsonHelper::ReadBoolFlag(profileObj, "3DGameMode", ScalingFlags::Is3DGameMode, profile.scalingFlags);
    if (!JsonHelper::ReadBoolFlag(profileObj, "captureTitleBar", ScalingFlags::CaptureTitleBar, profile.scalingFlags, true)) {
        JsonHelper::ReadBoolFlag(profileObj, "reserveTitleBar", ScalingFlags::CaptureTitleBar, profile.scalingFlags);
    }
    JsonHelper::ReadBoolFlag(profileObj, "adjustCursorSpeed", ScalingFlags::AdjustCursorSpeed, profile.scalingFlags);
    JsonHelper::ReadBoolFlag(profileObj, "disableDirectFlip", ScalingFlags::DisableDirectFlip, profile.scalingFlags);

    uint32_t cursorScaling = (uint32_t)CursorScaling::NoScaling;
    JsonHelper::ReadUInt(profileObj, "cursorScaling", cursorScaling);
    if (cursorScaling >= (uint32_t)CursorScaling::COUNT) {
        cursorScaling = (uint32_t)CursorScaling::NoScaling;
    }
    profile.cursorScaling = (CursorScaling)cursorScaling;

    JsonHelper::ReadFloat(profileObj, "customCursorScaling", profile.customCursorScaling);
    if (profile.customCursorScaling < 0) {
        profile.customCursorScaling = 1.0f;
    }

    uint32_t cursorInterpolationMode = (uint32_t)CursorInterpolationMode::NearestNeighbor;
    JsonHelper::ReadUInt(profileObj, "cursorInterpolationMode", cursorInterpolationMode);
    if (cursorInterpolationMode > 1) {
        cursorInterpolationMode = (uint32_t)CursorInterpolationMode::NearestNeighbor;
    }
    profile.cursorInterpolationMode = (CursorInterpolationMode)cursorInterpolationMode;

    JsonHelper::ReadBool(profileObj, "autoHideCursorEnabled", profile.isAutoHideCursorEnabled);
    JsonHelper::ReadFloat(profileObj, "autoHideCursorDelay", profile.autoHideCursorDelay);
    if (profile.autoHideCursorDelay <= 0.1f - FLOAT_EPSILON<float> || profile.autoHideCursorDelay >= 5.0f + FLOAT_EPSILON<float>) {
        profile.autoHideCursorDelay = 3.0f;
    }

    JsonHelper::ReadBool(profileObj, "croppingEnabled", profile.isCroppingEnabled);

    auto croppingNode = profileObj.FindMember("cropping");
    if (croppingNode != profileObj.MemberEnd() && croppingNode->value.IsObject()) {
        auto croppingObj = croppingNode->value.GetObj();

        if (!JsonHelper::ReadFloat(croppingObj, "left", profile.cropping.Left, true)
            || profile.cropping.Left < 0
            || !JsonHelper::ReadFloat(croppingObj, "top", profile.cropping.Top, true)
            || profile.cropping.Top < 0
            || !JsonHelper::ReadFloat(croppingObj, "right", profile.cropping.Right, true)
            || profile.cropping.Right < 0
            || !JsonHelper::ReadFloat(croppingObj, "bottom", profile.cropping.Bottom, true)
            || profile.cropping.Bottom < 0) {
            profile.cropping = {};
        }
    }

    return true;
}

bool AppSettings::_SetDefaultShortcuts() noexcept {
    bool changed = false;

    Shortcut& scaleShortcut = _shortcuts[(size_t)ShortcutAction::Scale];
    if (scaleShortcut.IsEmpty()) {
        scaleShortcut.alt = true;
        scaleShortcut.shift = true;
        scaleShortcut.code = 'A';

        changed = true;
    }

    Shortcut& windowedModeScaleShortcut = _shortcuts[(size_t)ShortcutAction::WindowedModeScale];
    if (windowedModeScaleShortcut.IsEmpty()) {
        windowedModeScaleShortcut.alt = true;
        windowedModeScaleShortcut.shift = true;
        windowedModeScaleShortcut.code = 'Q';

        changed = true;
    }

    Shortcut& overlayShortcut = _shortcuts[(size_t)ShortcutAction::Toolbar];
    if (overlayShortcut.IsEmpty()) {
        overlayShortcut.alt = true;
        overlayShortcut.shift = true;
        overlayShortcut.code = 'D';

        changed = true;
    }

    return changed;
}

void AppSettings::_SetDefaultScalingModes() noexcept {
    _scalingModes.resize(7);

    auto& lanczos = _scalingModes[0];
    lanczos.name = L"Lanczos";
    auto& lanczosEffect = lanczos.effects.emplace_back();
    lanczosEffect.name = L"Lanczos";
    lanczosEffect.scalingType = ::Magpie::ScalingType::Fit;

    auto& fsr = _scalingModes[1];
    fsr.name = L"FSR";
    fsr.effects.resize(2);
    auto& easu = fsr.effects[0];
    easu.name = L"FSR\\FSR_EASU";
    easu.scalingType = ::Magpie::ScalingType::Fit;
    auto& rcas = fsr.effects[1];
    rcas.name = L"FSR\\FSR_RCAS";
    rcas.parameters[L"sharpness"] = 0.87f;

    auto& fsrcnnx = _scalingModes[2];
    fsrcnnx.name = L"FSRCNNX";
    fsrcnnx.effects.emplace_back().name = L"FSRCNNX\\FSRCNNX";

    auto& acnet = _scalingModes[3];
    acnet.name = L"CuNNy";
    acnet.effects.emplace_back().name = L"CuNNy\\CuNNy";
    acnet.effects.back().scalingType = ::Magpie::ScalingType::Fit;

    auto& cunnyn2 = _scalingModes[4];
    cunnyn2.name = L"CuNNy2";
    cunnyn2.effects.emplace_back().name = L"CuNNy2\\CuNNy2";
    cunnyn2.effects.back().scalingType = ::Magpie::ScalingType::Fit;

    auto& crt = _scalingModes[5];
    crt.name = L"CRT";
    crt.effects.emplace_back().name = L"CRT\\CRT";
    crt.effects.back().scalingType = ::Magpie::ScalingType::Fit;

    auto& xbrz = _scalingModes[6];
    xbrz.name = L"xBRZ";
    xbrz.effects.emplace_back().name = L"xBRZ\\xBRZ";
    xbrz.effects.back().scalingType = ::Magpie::ScalingType::Fit;

    }

} // namespace Magpie

