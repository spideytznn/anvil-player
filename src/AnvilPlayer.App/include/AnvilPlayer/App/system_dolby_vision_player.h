#pragma once

#include <mfmediaengine.h>
#include <wrl/client.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>

namespace anvil::app {

// Hosts the Windows Media Foundation media engine on the player's video HWND.
// This deliberately leaves Dolby Vision parsing, decoding and display
// signaling to the Windows/Dolby system components instead of relabelling the
// player's HDR10 swap chain as Dolby Vision.
class SystemDolbyVisionPlayer {
public:
    SystemDolbyVisionPlayer() = default;
    ~SystemDolbyVisionPlayer();

    SystemDolbyVisionPlayer(const SystemDolbyVisionPlayer&) = delete;
    SystemDolbyVisionPlayer& operator=(const SystemDolbyVisionPlayer&) = delete;

    bool Start(HWND videoHost,
               const std::filesystem::path& path,
               std::chrono::milliseconds position,
               double volume);
    void RequestStop();
    void Stop();
    bool Play();
    bool Pause();
    bool Seek(std::chrono::milliseconds position);
    void SetVolume(double volume);
    void SetPlaybackRate(double rate);

    bool IsActive() const;
    bool IsPlaying() const;
    bool HasFailed() const;
    bool HasEnded() const;
    std::chrono::milliseconds Position() const;
    std::wstring LastError() const;

private:
    class Notify final : public IMFMediaEngineNotify {
    public:
        explicit Notify(SystemDolbyVisionPlayer* owner) : owner_(owner) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override;

        void Detach();

    private:
        std::atomic_ulong references_{1};
        std::atomic<SystemDolbyVisionPlayer*> owner_;
    };

    void OnMediaEngineEvent(DWORD event, DWORD_PTR param1, DWORD param2);
    static std::wstring SourceUrl(const std::filesystem::path& path);

    mutable std::mutex mutex_;
    Microsoft::WRL::ComPtr<IMFMediaEngine> engine_;
    Microsoft::WRL::ComPtr<Notify> notify_;
    std::chrono::milliseconds pendingPosition_{};
    bool pendingPlay_ = false;
    bool mediaFoundationStarted_ = false;
    std::atomic_bool active_{false};
    std::atomic_bool playing_{false};
    std::atomic_bool failed_{false};
    std::atomic_bool ended_{false};
    mutable std::wstring lastError_;
};

}  // namespace anvil::app
