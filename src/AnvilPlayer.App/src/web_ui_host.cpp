#include "AnvilPlayer/App/web_ui_host.h"

#include <objbase.h>
#include <shlobj.h>
#include <unknwn.h>
#include <wrl.h>
#include <WebView2.h>

#include <atomic>
#include <cstring>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <system_error>
#include <iterator>
#include <utility>

namespace anvil::app {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {

constexpr wchar_t kWebUiVirtualHost[] = L"appassets.anvilplayer.local";
constexpr wchar_t kWebUiBaseUrl[] = L"http://appassets.anvilplayer.local/index.html";
constexpr wchar_t kWebViewBrowserArguments[] =
    L"--allow-running-insecure-content "
    L"--disable-web-security "
    L"--ignore-certificate-errors "
    L"--disable-features=BlockInsecurePrivateNetworkRequests,PrivateNetworkAccessSendPreflights,"
    L"PrivateNetworkAccessRespectPreflightResults";

HRESULT CopyCoTaskMemString(const std::wstring& source, LPWSTR* output) {
    if (!output) {
        return E_POINTER;
    }
    const std::size_t bytes = (source.size() + 1) * sizeof(wchar_t);
    auto* buffer = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!buffer) {
        *output = nullptr;
        return E_OUTOFMEMORY;
    }
    std::memcpy(buffer, source.c_str(), bytes);
    *output = buffer;
    return S_OK;
}

class WebViewEnvironmentOptions final
    : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ICoreWebView2EnvironmentOptions> {
public:
    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* output) override {
        return CopyCoTaskMemString(additionalBrowserArguments_, output);
    }

    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR newValue) override {
        additionalBrowserArguments_ = newValue ? newValue : L"";
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* output) override {
        return CopyCoTaskMemString(language_, output);
    }

    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR newValue) override {
        language_ = newValue ? newValue : L"";
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* output) override {
        return CopyCoTaskMemString(targetCompatibleBrowserVersion_, output);
    }

    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR newValue) override {
        targetCompatibleBrowserVersion_ = newValue ? newValue : L"";
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* allow) override {
        if (!allow) {
            return E_POINTER;
        }
        *allow = allowSingleSignOnUsingOSPrimaryAccount_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL allow) override {
        allowSingleSignOnUsingOSPrimaryAccount_ = allow;
        return S_OK;
    }

private:
    std::wstring additionalBrowserArguments_;
    std::wstring language_;
    std::wstring targetCompatibleBrowserVersion_;
    BOOL allowSingleSignOnUsingOSPrimaryAccount_ = FALSE;
};

std::filesystem::path WebViewUserDataFolder(std::wstring_view profileName) {
    wchar_t localAppData[MAX_PATH]{};
    // An empty profile name maps to the legacy base folder (AnvilPlayer/WebView2)
    // so existing user data (Emby connections, TMDB settings, view state) is
    // preserved when the library window reuses it. Named profiles get a sibling
    // subfolder so two hosts in one process never share a folder.
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData))) {
        auto base = std::filesystem::path(localAppData) / L"AnvilPlayer" / L"WebView2";
        return profileName.empty() ? base : base / profileName;
    }
    auto fallback = std::filesystem::temp_directory_path() / L"AnvilPlayer-WebView2";
    return profileName.empty() ? fallback : fallback / profileName;
}

std::filesystem::path ModuleDirectory() {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (length == 0 || length >= std::size(modulePath)) {
        return {};
    }
    return std::filesystem::path(modulePath).parent_path();
}

std::filesystem::path LocalWebViewRuntimeFolder() {
    // Packaging copies the fixed runtime directly to this deterministic
    // directory. Let WebView2's asynchronous environment creation validate it
    // instead of synchronously probing/iterating the filesystem on the window
    // thread.
    return ModuleDirectory() / L"WebView2Runtime";
}

}  // namespace

struct WebUiHost::Impl {
    struct State {
        HWND parent = nullptr;
        RECT pendingBounds{};
        std::filesystem::path webRoot;
        std::wstring initialUrl;
        std::wstring profileName;
        MessageHandler messageHandler;
        std::filesystem::path browserExecutableFolder;
        std::filesystem::path userDataFolder;
        ComPtr<ICoreWebView2Environment> environment;
        ComPtr<ICoreWebView2Controller> controller;
        ComPtr<ICoreWebView2> webview;
        EventRegistrationToken webMessageToken{};
        EventRegistrationToken certificateErrorToken{};
        bool webMessageRegistered = false;
        bool certificateErrorRegistered = false;
        std::atomic<HRESULT> lastCreateResult{S_OK};
        std::atomic_bool usedDefaultOptionsFallback{false};
        std::atomic_bool allowInsecureCertificates{false};
        std::atomic<std::uint64_t> nextGeneration{0};
        std::atomic<std::uint64_t> activeGeneration{0};

        std::uint64_t BeginGeneration() noexcept {
            const std::uint64_t generation = nextGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
            activeGeneration.store(generation, std::memory_order_release);
            return generation;
        }

        bool IsCurrent(const std::uint64_t generation) const noexcept {
            return generation != 0 && activeGeneration.load(std::memory_order_acquire) == generation;
        }

        void InvalidateGeneration() noexcept {
            activeGeneration.store(0, std::memory_order_release);
        }
    };

    std::shared_ptr<State> state = std::make_shared<State>();

    ~Impl() {
        Shutdown();
    }

    static void CloseController(ICoreWebView2Controller* controller) noexcept {
        if (!controller) {
            return;
        }
        controller->put_IsVisible(FALSE);
        controller->Close();
    }

    static HRESULT HandleWebMessage(const std::weak_ptr<State>& weakState,
                                    const std::uint64_t generation,
                                    ICoreWebView2WebMessageReceivedEventArgs* args) {
        const auto current = weakState.lock();
        if (!current || !current->IsCurrent(generation) || !args) {
            return S_OK;
        }

        LPWSTR rawJson = nullptr;
        if (FAILED(args->get_WebMessageAsJson(&rawJson)) || !rawJson) {
            return S_OK;
        }
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> jsonOwner(rawJson, &CoTaskMemFree);

        try {
            const std::wstring json{rawJson};
            if (!current->IsCurrent(generation)) {
                return S_OK;
            }

            const MessageHandler handler = current->messageHandler;
            if (handler) {
                handler(json);
            }
            return S_OK;
        } catch (const std::exception& error) {
            std::wstring message = L"Anvil WebView message handler exception: ";
            message += std::wstring(error.what(), error.what() + std::strlen(error.what()));
            message += L"\n";
            OutputDebugStringW(message.c_str());
            return E_FAIL;
        } catch (...) {
            OutputDebugStringW(L"Anvil WebView message handler unknown exception\n");
            return E_FAIL;
        }
    }

    static HRESULT CompleteController(const std::weak_ptr<State>& weakState,
                                      const std::uint64_t generation,
                                      const HRESULT result,
                                      ICoreWebView2Controller* createdController) {
        const auto current = weakState.lock();
        if (!current || !current->IsCurrent(generation)) {
            CloseController(createdController);
            return S_OK;
        }

        current->lastCreateResult.store(result, std::memory_order_release);
        if (FAILED(result) || !createdController) {
            CloseController(createdController);
            return S_OK;
        }

        ComPtr<ICoreWebView2Controller> controller = createdController;
        ComPtr<ICoreWebView2> webview;
        const HRESULT webviewResult = controller->get_CoreWebView2(&webview);
        if (FAILED(webviewResult) || !webview) {
            current->lastCreateResult.store(FAILED(webviewResult) ? webviewResult : E_NOINTERFACE,
                                            std::memory_order_release);
            CloseController(controller.Get());
            return S_OK;
        }
        if (!current->IsCurrent(generation)) {
            CloseController(controller.Get());
            return S_OK;
        }

        current->controller = controller;
        current->webview = webview;
        controller->put_Bounds(current->pendingBounds);
        controller->put_IsVisible(TRUE);

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(webview->get_Settings(&settings)) && settings) {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDevToolsEnabled(TRUE);
            settings->put_IsStatusBarEnabled(FALSE);
        }

        EventRegistrationToken webMessageToken{};
        const HRESULT webMessageResult = webview->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [weakState, generation](ICoreWebView2*,
                                        ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    return Impl::HandleWebMessage(weakState, generation, args);
                })
                .Get(),
            &webMessageToken);
        if (SUCCEEDED(webMessageResult)) {
            if (!current->IsCurrent(generation)) {
                webview->remove_WebMessageReceived(webMessageToken);
                CloseController(controller.Get());
                return S_OK;
            }
            current->webMessageToken = webMessageToken;
            current->webMessageRegistered = true;
        }

        ComPtr<ICoreWebView2_14> webview14;
        if (SUCCEEDED(webview.As(&webview14)) && webview14) {
            EventRegistrationToken certificateErrorToken{};
            const HRESULT certificateResult = webview14->add_ServerCertificateErrorDetected(
                Callback<ICoreWebView2ServerCertificateErrorDetectedEventHandler>(
                    [weakState, generation](
                        ICoreWebView2*,
                        ICoreWebView2ServerCertificateErrorDetectedEventArgs* args) -> HRESULT {
                        const auto callbackState = weakState.lock();
                        if (callbackState && callbackState->IsCurrent(generation) &&
                            callbackState->allowInsecureCertificates.load(std::memory_order_acquire) && args) {
                            args->put_Action(COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_ALWAYS_ALLOW);
                        }
                        return S_OK;
                    })
                    .Get(),
                &certificateErrorToken);
            if (SUCCEEDED(certificateResult)) {
                if (!current->IsCurrent(generation)) {
                    webview14->remove_ServerCertificateErrorDetected(certificateErrorToken);
                    CloseController(controller.Get());
                    return S_OK;
                }
                current->certificateErrorToken = certificateErrorToken;
                current->certificateErrorRegistered = true;
            }
        }

        ComPtr<ICoreWebView2_3> webview3;
        if (current->IsCurrent(generation) && SUCCEEDED(webview.As(&webview3)) && webview3) {
            webview3->SetVirtualHostNameToFolderMapping(
                kWebUiVirtualHost,
                current->webRoot.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        }
        if (current->IsCurrent(generation)) {
            webview->Navigate(current->initialUrl.c_str());
        }
        return S_OK;
    }

    static HRESULT CompleteEnvironment(const std::weak_ptr<State>& weakState,
                                       const std::uint64_t generation,
                                       const HRESULT result,
                                       ICoreWebView2Environment* createdEnvironment) {
        const auto current = weakState.lock();
        if (!current || !current->IsCurrent(generation)) {
            return S_OK;
        }

        current->lastCreateResult.store(result, std::memory_order_release);
        if (FAILED(result) || !createdEnvironment) {
            return S_OK;
        }

        current->environment = createdEnvironment;
        const HRESULT controllerStartResult = createdEnvironment->CreateCoreWebView2Controller(
            current->parent,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [weakState, generation](HRESULT controllerResult,
                                        ICoreWebView2Controller* createdController) -> HRESULT {
                    return Impl::CompleteController(
                        weakState, generation, controllerResult, createdController);
                })
                .Get());
        if (FAILED(controllerStartResult) && current->IsCurrent(generation)) {
            current->lastCreateResult.store(controllerStartResult, std::memory_order_release);
        }
        return S_OK;
    }

    void Shutdown() noexcept {
        const auto current = state;
        if (!current) {
            return;
        }

        current->InvalidateGeneration();
        current->messageHandler = {};
        current->parent = nullptr;

        const ComPtr<ICoreWebView2> webview = current->webview;
        const ComPtr<ICoreWebView2Controller> controller = current->controller;
        const bool removeWebMessage = current->webMessageRegistered;
        const bool removeCertificateError = current->certificateErrorRegistered;
        const EventRegistrationToken webMessageToken = current->webMessageToken;
        const EventRegistrationToken certificateErrorToken = current->certificateErrorToken;

        current->webMessageRegistered = false;
        current->certificateErrorRegistered = false;
        current->webMessageToken = {};
        current->certificateErrorToken = {};
        current->webview.Reset();
        current->controller.Reset();
        current->environment.Reset();

        if (webview && removeCertificateError) {
            ComPtr<ICoreWebView2_14> webview14;
            if (SUCCEEDED(webview.As(&webview14)) && webview14) {
                webview14->remove_ServerCertificateErrorDetected(certificateErrorToken);
            }
        }
        if (webview && removeWebMessage) {
            webview->remove_WebMessageReceived(webMessageToken);
        }
        CloseController(controller.Get());
    }

    bool Create(HWND parentWindow,
                const std::filesystem::path& root,
                std::wstring_view url,
                std::wstring_view profile,
                MessageHandler handler) {
        Shutdown();
        const auto current = state;
        current->lastCreateResult.store(S_OK, std::memory_order_release);
        current->usedDefaultOptionsFallback.store(false, std::memory_order_release);
        current->parent = parentWindow;

        std::error_code error;
        current->webRoot = std::filesystem::absolute(root, error).lexically_normal();
        if (error) {
            current->webRoot = root;
        }
        current->initialUrl = std::wstring{url.empty() ? std::wstring_view{kWebUiBaseUrl} : url};
        current->profileName = profile;

        RECT client{};
        GetClientRect(current->parent, &client);
        current->pendingBounds = client;

        current->userDataFolder = WebViewUserDataFolder(current->profileName);
        current->browserExecutableFolder = LocalWebViewRuntimeFolder();
        current->messageHandler = std::move(handler);
        const std::uint64_t generation = current->BeginGeneration();
        const std::weak_ptr<State> weakState = current;

        const auto environmentOptions = Make<WebViewEnvironmentOptions>();
        if (environmentOptions) {
            environmentOptions->put_AdditionalBrowserArguments(kWebViewBrowserArguments);
        }

        const auto createEnvironment = [userDataFolder = current->userDataFolder,
                                        weakState,
                                        generation](const std::filesystem::path& browserExecutableFolder,
                                                    ICoreWebView2EnvironmentOptions* options) -> HRESULT {
            return CreateCoreWebView2EnvironmentWithOptions(
                browserExecutableFolder.empty() ? nullptr : browserExecutableFolder.c_str(),
                userDataFolder.c_str(),
                options,
                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [weakState, generation](HRESULT result,
                                            ICoreWebView2Environment* createdEnvironment) -> HRESULT {
                        return Impl::CompleteEnvironment(
                            weakState, generation, result, createdEnvironment);
                    })
                    .Get());
        };

        HRESULT result = createEnvironment(current->browserExecutableFolder,
                                           environmentOptions.Get());
        if (FAILED(result) && !current->browserExecutableFolder.empty()) {
            // Fixed runtime not packaged: immediately fall back to the
            // installed Evergreen runtime without probing either path.
            current->browserExecutableFolder.clear();
            result = createEnvironment({}, environmentOptions.Get());
        }
        if (FAILED(result) && environmentOptions) {
            result = createEnvironment({}, nullptr);
            current->usedDefaultOptionsFallback.store(SUCCEEDED(result), std::memory_order_release);
        }

        current->lastCreateResult.store(result, std::memory_order_release);
        if (FAILED(result)) {
            Shutdown();
            return false;
        }
        return true;
    }

    void Resize(const RECT bounds) const {
        const auto current = state;
        current->pendingBounds = bounds;
        if (current->activeGeneration.load(std::memory_order_acquire) != 0 && current->controller) {
            current->controller->put_Bounds(bounds);
        }
    }

    void PostJson(const std::wstring& json) const {
        const auto current = state;
        if (current->activeGeneration.load(std::memory_order_acquire) != 0 && current->webview) {
            current->webview->PostWebMessageAsJson(json.c_str());
        }
    }

    void SetAllowInsecureCertificates(const bool allow) const {
        const auto current = state;
        current->allowInsecureCertificates.store(allow, std::memory_order_release);
        const std::uint64_t generation = current->activeGeneration.load(std::memory_order_acquire);
        if (allow || generation == 0 || !current->webview) {
            return;
        }

        ComPtr<ICoreWebView2_14> webview14;
        if (SUCCEEDED(current->webview.As(&webview14)) && webview14) {
            const std::weak_ptr<State> weakState = current;
            webview14->ClearServerCertificateErrorActions(
                Callback<ICoreWebView2ClearServerCertificateErrorActionsCompletedHandler>(
                    [weakState, generation](HRESULT) -> HRESULT {
                        const auto callbackState = weakState.lock();
                        if (!callbackState || !callbackState->IsCurrent(generation)) {
                            return S_OK;
                        }
                        return S_OK;
                    })
                    .Get());
        }
    }

    void SetMuted(const bool muted) const {
        const auto current = state;
        if (current->activeGeneration.load(std::memory_order_acquire) == 0 || !current->webview) {
            return;
        }
        ComPtr<ICoreWebView2_8> webview8;
        if (SUCCEEDED(current->webview.As(&webview8)) && webview8) {
            webview8->put_IsMuted(muted ? TRUE : FALSE);
        }
    }

    bool Ready() const {
        const auto current = state;
        return current->activeGeneration.load(std::memory_order_acquire) != 0 && current->webview;
    }

    HRESULT LastCreateResult() const {
        return state->lastCreateResult.load(std::memory_order_acquire);
    }

    bool UsedDefaultOptionsFallback() const {
        return state->usedDefaultOptionsFallback.load(std::memory_order_acquire);
    }
};

WebUiHost::WebUiHost()
    : impl_(std::make_unique<Impl>()) {
}

WebUiHost::~WebUiHost() = default;

bool WebUiHost::Create(HWND parent,
                       const std::filesystem::path& webRoot,
                       std::wstring_view initialUrl,
                       std::wstring_view profileName,
                       MessageHandler handler) {
    return impl_->Create(parent, webRoot, initialUrl, profileName, std::move(handler));
}

void WebUiHost::Shutdown() noexcept {
    impl_->Shutdown();
}

void WebUiHost::Resize(const RECT bounds) const {
    impl_->Resize(bounds);
}

void WebUiHost::PostJson(const std::wstring& json) const {
    impl_->PostJson(json);
}

void WebUiHost::SetAllowInsecureCertificates(const bool allow) const {
    impl_->SetAllowInsecureCertificates(allow);
}

bool WebUiHost::Ready() const {
    return impl_->Ready();
}

void WebUiHost::SetMuted(const bool muted) const {
    impl_->SetMuted(muted);
}

HRESULT WebUiHost::LastCreateResult() const {
    return impl_->LastCreateResult();
}

bool WebUiHost::UsedDefaultOptionsFallback() const {
    return impl_->UsedDefaultOptionsFallback();
}

}  // namespace anvil::app
