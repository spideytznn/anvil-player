#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace anvil::app {

class DisplayRefreshRateController {
public:
    ~DisplayRefreshRateController();

    bool ApplyForWindow(HWND window, double videoFrameRate, bool useMaximumMultiple);
    void Restore();
    bool Active() const { return originalMode_.has_value(); }
    double AppliedRefreshRate() const { return appliedRefreshRate_; }

    static bool LoadGlobalEnabled();
    static void SaveGlobalEnabled(bool enabled);
    static bool LoadMaximumMultipleEnabled();
    static void SaveMaximumMultipleEnabled(bool enabled);
    static std::wstring LoadUiLanguage();
    static void SaveUiLanguage(const std::wstring& language);

private:
    std::wstring deviceName_;
    std::optional<DEVMODEW> originalMode_;
    double appliedRefreshRate_ = 0.0;
    double appliedVideoFrameRate_ = 0.0;
    bool appliedMaximumMultiple_ = true;
};

}  // namespace anvil::app
