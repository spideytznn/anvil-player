#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace anvil::app {

// Owns the native copy of a pending Emby playback report until the player
// WebView confirms that it has persisted the report in its own profile.
//
// Posting a WebView message is not delivery confirmation: the page may not
// have registered its message listener yet. Attempts may therefore be retried
// freely; Acknowledge is the only operation that consumes the pending report.
class EmbyReportRelayState {
public:
    void Queue(std::wstring reportId, std::wstring reportJson) {
        reportId_ = std::move(reportId);
        reportJson_ = std::move(reportJson);
        deliveryAttempts_ = 0;
    }

    bool HasPending() const noexcept {
        return !reportJson_.empty();
    }

    const std::wstring& ReportId() const noexcept {
        return reportId_;
    }

    const std::wstring& ReportJson() const noexcept {
        return reportJson_;
    }

    int NoteDeliveryAttempt() noexcept {
        return ++deliveryAttempts_;
    }

    int DeliveryAttempts() const noexcept {
        return deliveryAttempts_;
    }

    void RestartDelivery() noexcept {
        deliveryAttempts_ = 0;
    }

    bool Acknowledge(const std::wstring_view reportId) {
        if (!HasPending() || reportId.empty() || reportId != reportId_) {
            return false;
        }
        reportId_.clear();
        reportJson_.clear();
        deliveryAttempts_ = 0;
        return true;
    }

private:
    std::wstring reportId_;
    std::wstring reportJson_;
    int deliveryAttempts_ = 0;
};

}  // namespace anvil::app
