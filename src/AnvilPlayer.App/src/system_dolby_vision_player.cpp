#include "AnvilPlayer/App/system_dolby_vision_player.h"

#include <mfapi.h>
#include <mferror.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace anvil::app {

namespace {

std::wstring HResultText(const wchar_t* operation, const HRESULT result) {
    std::wostringstream text;
    text << operation << L" failed hr=0x" << std::hex << std::uppercase
         << static_cast<unsigned long>(result);
    return text.str();
}

}  // namespace

SystemDolbyVisionPlayer::~SystemDolbyVisionPlayer() {
    Stop();
}

HRESULT STDMETHODCALLTYPE SystemDolbyVisionPlayer::Notify::QueryInterface(
    REFIID iid,
    void** object) {
    if (!object) {
        return E_POINTER;
    }
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify)) {
        *object = static_cast<IMFMediaEngineNotify*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE SystemDolbyVisionPlayer::Notify::AddRef() {
    return ++references_;
}

ULONG STDMETHODCALLTYPE SystemDolbyVisionPlayer::Notify::Release() {
    const ULONG remaining = --references_;
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE SystemDolbyVisionPlayer::Notify::EventNotify(
    const DWORD event,
    const DWORD_PTR param1,
    const DWORD param2) {
    if (auto* owner = owner_.load(std::memory_order_acquire)) {
        owner->OnMediaEngineEvent(event, param1, param2);
    }
    return S_OK;
}

void SystemDolbyVisionPlayer::Notify::Detach() {
    owner_.store(nullptr, std::memory_order_release);
}

std::wstring SystemDolbyVisionPlayer::SourceUrl(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    if (UrlIsW(value.c_str(), URLIS_URL)) {
        return value;
    }

    // UrlCreateFromPath does not consistently implement a null-buffer size
    // query across supported shlwapi versions. Paths are bounded by Win32's
    // extended path limit, so allocate that limit once and perform the real
    // conversion directly.
    DWORD length = 32768;
    std::wstring url(length, L'\0');
    const HRESULT result = UrlCreateFromPathW(value.c_str(), url.data(), &length, 0);
    if (FAILED(result)) {
        return {};
    }
    url.resize(length);
    return url;
}

bool SystemDolbyVisionPlayer::Start(
    const HWND videoHost,
    const std::filesystem::path& path,
    const std::chrono::milliseconds position,
    const double volume) {
    Stop();
    if (!videoHost || path.empty()) {
        return false;
    }

    const std::wstring source = SourceUrl(path);
    if (source.empty()) {
        std::scoped_lock lock(mutex_);
        lastError_ = L"Media Foundation could not convert the source path to a URL";
        failed_.store(true, std::memory_order_release);
        return false;
    }

    const HRESULT startupResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(startupResult)) {
        std::scoped_lock lock(mutex_);
        lastError_ = HResultText(L"MFStartup", startupResult);
        failed_.store(true, std::memory_order_release);
        return false;
    }
    mediaFoundationStarted_ = true;

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 4);
    if (SUCCEEDED(result)) {
        notify_.Attach(new Notify(this));
        result = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_.Get());
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT64(
            MF_MEDIA_ENGINE_PLAYBACK_HWND,
            static_cast<UINT64>(reinterpret_cast<ULONG_PTR>(videoHost)));
    }

    Microsoft::WRL::ComPtr<IMFMediaEngineClassFactory> factory;
    if (SUCCEEDED(result)) {
        result = CoCreateInstance(CLSID_MFMediaEngineClassFactory,
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    }
    if (SUCCEEDED(result)) {
        result = factory->CreateInstance(0, attributes.Get(), &engine_);
    }
    if (FAILED(result) || !engine_) {
        {
            std::scoped_lock lock(mutex_);
            lastError_ = HResultText(L"IMFMediaEngine creation", result);
            failed_.store(true, std::memory_order_release);
        }
        Stop();
        failed_.store(true, std::memory_order_release);
        return false;
    }

    pendingPosition_ = std::max(position, std::chrono::milliseconds{0});
    pendingPlay_ = true;
    failed_.store(false, std::memory_order_release);
    ended_.store(false, std::memory_order_release);
    playing_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(mutex_);
        lastError_.clear();
    }

    engine_->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    engine_->SetVolume(std::clamp(volume, 0.0, 1.0));
    BSTR sourceValue = SysAllocStringLen(source.data(), static_cast<UINT>(source.size()));
    if (!sourceValue) {
        {
            std::scoped_lock lock(mutex_);
            lastError_ = L"Media Foundation source URL allocation failed";
        }
        Stop();
        failed_.store(true, std::memory_order_release);
        return false;
    }
    result = engine_->SetSource(sourceValue);
    SysFreeString(sourceValue);
    if (SUCCEEDED(result)) {
        result = engine_->Load();
    }
    if (FAILED(result)) {
        {
            std::scoped_lock lock(mutex_);
            lastError_ = HResultText(L"IMFMediaEngine source load", result);
        }
        Stop();
        failed_.store(true, std::memory_order_release);
        return false;
    }

    active_.store(true, std::memory_order_release);
    return true;
}

void SystemDolbyVisionPlayer::RequestStop() {
    std::scoped_lock lock(mutex_);
    pendingPlay_ = false;
    if (engine_) {
        engine_->Pause();
    }
    playing_.store(false, std::memory_order_release);
}

void SystemDolbyVisionPlayer::Stop() {
    active_.store(false, std::memory_order_release);
    playing_.store(false, std::memory_order_release);
    pendingPlay_ = false;
    if (notify_) {
        notify_->Detach();
    }
    {
        std::scoped_lock lock(mutex_);
        if (engine_) {
            engine_->Pause();
        }
        engine_.Reset();
    }
    notify_.Reset();
    if (mediaFoundationStarted_) {
        MFShutdown();
        mediaFoundationStarted_ = false;
    }
}

bool SystemDolbyVisionPlayer::Play() {
    std::scoped_lock lock(mutex_);
    pendingPlay_ = true;
    ended_.store(false, std::memory_order_release);
    return engine_ && SUCCEEDED(engine_->Play());
}

bool SystemDolbyVisionPlayer::Pause() {
    std::scoped_lock lock(mutex_);
    pendingPlay_ = false;
    const bool paused = engine_ && SUCCEEDED(engine_->Pause());
    if (paused) {
        playing_.store(false, std::memory_order_release);
    }
    return paused;
}

bool SystemDolbyVisionPlayer::Seek(const std::chrono::milliseconds position) {
    std::scoped_lock lock(mutex_);
    pendingPosition_ = std::max(position, std::chrono::milliseconds{0});
    return engine_ && SUCCEEDED(engine_->SetCurrentTime(
                          static_cast<double>(pendingPosition_.count()) / 1000.0));
}

void SystemDolbyVisionPlayer::SetVolume(const double volume) {
    std::scoped_lock lock(mutex_);
    if (engine_) {
        engine_->SetVolume(std::clamp(volume, 0.0, 1.0));
    }
}

void SystemDolbyVisionPlayer::SetPlaybackRate(const double rate) {
    std::scoped_lock lock(mutex_);
    if (engine_) {
        engine_->SetPlaybackRate(std::clamp(rate, 0.25, 4.0));
    }
}

bool SystemDolbyVisionPlayer::IsActive() const {
    return active_.load(std::memory_order_acquire);
}

bool SystemDolbyVisionPlayer::IsPlaying() const {
    return playing_.load(std::memory_order_acquire);
}

bool SystemDolbyVisionPlayer::HasFailed() const {
    return failed_.load(std::memory_order_acquire);
}

bool SystemDolbyVisionPlayer::HasEnded() const {
    return ended_.load(std::memory_order_acquire);
}

std::chrono::milliseconds SystemDolbyVisionPlayer::Position() const {
    std::scoped_lock lock(mutex_);
    if (!engine_) {
        return pendingPosition_;
    }
    const double seconds = engine_->GetCurrentTime();
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return pendingPosition_;
    }
    return std::chrono::milliseconds{
        static_cast<long long>(std::llround(seconds * 1000.0))};
}

std::wstring SystemDolbyVisionPlayer::LastError() const {
    std::scoped_lock lock(mutex_);
    return lastError_;
}

void SystemDolbyVisionPlayer::OnMediaEngineEvent(
    const DWORD event,
    const DWORD_PTR param1,
    const DWORD param2) {
    (void)param2;
    if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA ||
        event == MF_MEDIA_ENGINE_EVENT_CANPLAY) {
        std::scoped_lock lock(mutex_);
        if (!engine_) {
            return;
        }
        if (pendingPosition_.count() > 0) {
            engine_->SetCurrentTime(static_cast<double>(pendingPosition_.count()) / 1000.0);
        }
        if (pendingPlay_) {
            engine_->Play();
        }
        return;
    }
    if (event == MF_MEDIA_ENGINE_EVENT_PLAYING) {
        playing_.store(true, std::memory_order_release);
        return;
    }
    if (event == MF_MEDIA_ENGINE_EVENT_PAUSE) {
        playing_.store(false, std::memory_order_release);
        return;
    }
    if (event == MF_MEDIA_ENGINE_EVENT_ENDED) {
        playing_.store(false, std::memory_order_release);
        ended_.store(true, std::memory_order_release);
        return;
    }
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
        playing_.store(false, std::memory_order_release);
        failed_.store(true, std::memory_order_release);
        std::scoped_lock lock(mutex_);
        lastError_ = L"Windows Dolby Vision media path failed event_error=" +
                     std::to_wstring(static_cast<unsigned long long>(param1));
    }
}

}  // namespace anvil::app
