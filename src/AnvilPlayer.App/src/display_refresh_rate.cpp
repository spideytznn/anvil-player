#include "AnvilPlayer/App/display_refresh_rate.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace anvil::app {
namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\AnvilPlayer";
constexpr wchar_t kRefreshSyncValue[] = L"RefreshRateSyncEnabled";
constexpr wchar_t kMaximumMultipleValue[] = L"RefreshRateMaximumMultiple";
constexpr wchar_t kUiLanguageValue[] = L"UiLanguage";

double EffectiveRefreshRate(const DWORD frequency) {
    // EnumDisplaySettings exposes the common NTSC-compatible modes using
    // their nominal integer names.
    switch (frequency) {
    case 23: return 24000.0 / 1001.0;
    case 29: return 30000.0 / 1001.0;
    case 47: return 48000.0 / 1001.0;
    case 59: return 60000.0 / 1001.0;
    case 71: return 72000.0 / 1001.0;
    case 95: return 96000.0 / 1001.0;
    case 119: return 120000.0 / 1001.0;
    default: return static_cast<double>(frequency);
    }
}

bool IsExactMultiple(const double refreshRate, const double frameRate, const bool allowHigherMultiple) {
    if (refreshRate <= 0.0 || frameRate <= 0.0) return false;
    const double ratio = refreshRate / frameRate;
    const double multiple = std::round(ratio);
    if (multiple < 1.0) return false;
    if (!allowHigherMultiple && multiple != 1.0) return false;
    return std::abs(refreshRate - frameRate * multiple) <= 0.015;
}

}  // namespace

DisplayRefreshRateController::~DisplayRefreshRateController() {
    Restore();
}

bool DisplayRefreshRateController::ApplyForWindow(const HWND window, const double videoFrameRate, const bool useMaximumMultiple) {
    if (appliedRefreshRate_ > 0.0 &&
        std::abs(appliedVideoFrameRate_ - videoFrameRate) < 0.001 &&
        appliedMaximumMultiple_ == useMaximumMultiple) {
        return true;
    }
    Restore();
    if (!window || videoFrameRate <= 0.0) return false;

    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;

    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsExW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &current, 0)) return false;

    std::vector<DEVMODEW> candidates;
    for (DWORD index = 0;; ++index) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExW(monitor.szDevice, index, &mode, 0)) break;
        if (mode.dmPelsWidth == current.dmPelsWidth &&
            mode.dmPelsHeight == current.dmPelsHeight &&
            mode.dmBitsPerPel == current.dmBitsPerPel &&
            IsExactMultiple(EffectiveRefreshRate(mode.dmDisplayFrequency), videoFrameRate, useMaximumMultiple)) {
            candidates.push_back(mode);
        }
    }
    if (candidates.empty()) return false;

    const auto compare = [](const DEVMODEW& left, const DEVMODEW& right) {
        return EffectiveRefreshRate(left.dmDisplayFrequency) < EffectiveRefreshRate(right.dmDisplayFrequency);
    };
    const auto best = useMaximumMultiple
        ? std::max_element(candidates.begin(), candidates.end(), compare)
        : std::min_element(candidates.begin(), candidates.end(), compare);
    if (best->dmDisplayFrequency == current.dmDisplayFrequency) {
        appliedRefreshRate_ = EffectiveRefreshRate(best->dmDisplayFrequency);
        appliedVideoFrameRate_ = videoFrameRate;
        appliedMaximumMultiple_ = useMaximumMultiple;
        return true;
    }

    DEVMODEW target = *best;
    target.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    if (ChangeDisplaySettingsExW(monitor.szDevice, &target, nullptr, CDS_FULLSCREEN, nullptr) != DISP_CHANGE_SUCCESSFUL) {
        return false;
    }
    deviceName_ = monitor.szDevice;
    originalMode_ = current;
    appliedRefreshRate_ = EffectiveRefreshRate(target.dmDisplayFrequency);
    appliedVideoFrameRate_ = videoFrameRate;
    appliedMaximumMultiple_ = useMaximumMultiple;
    return true;
}

void DisplayRefreshRateController::Restore() {
    if (!originalMode_.has_value() || deviceName_.empty()) {
        appliedRefreshRate_ = 0.0;
        appliedVideoFrameRate_ = 0.0;
        return;
    }
    DEVMODEW mode = *originalMode_;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    ChangeDisplaySettingsExW(deviceName_.c_str(), &mode, nullptr, 0, nullptr);
    originalMode_.reset();
    deviceName_.clear();
    appliedRefreshRate_ = 0.0;
    appliedVideoFrameRate_ = 0.0;
}

bool DisplayRefreshRateController::LoadGlobalEnabled() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kRefreshSyncValue, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
}

void DisplayRefreshRateController::SaveGlobalEnabled(const bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const DWORD value = enabled ? 1 : 0;
        RegSetValueExW(key, kRefreshSyncValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}

bool DisplayRefreshRateController::LoadMaximumMultipleEnabled() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kMaximumMultipleValue, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS || value != 0;
}

void DisplayRefreshRateController::SaveMaximumMultipleEnabled(const bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const DWORD value = enabled ? 1 : 0;
        RegSetValueExW(key, kMaximumMultipleValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}

std::wstring DisplayRefreshRateController::LoadUiLanguage() {
    wchar_t value[16]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kUiLanguageValue, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
        return std::wstring(value) == L"en" ? L"en" : L"zh";
    }
    return L"zh";
}

void DisplayRefreshRateController::SaveUiLanguage(const std::wstring& language) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const std::wstring value = language == L"en" ? L"en" : L"zh";
        RegSetValueExW(key, kUiLanguageValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

}  // namespace anvil::app
