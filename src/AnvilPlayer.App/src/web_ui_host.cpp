#include "AnvilPlayer/App/web_ui_host.h"

#include <objbase.h>
#include <shlobj.h>
#include <unknwn.h>
#include <wrl.h>
#include <WebView2.h>

#include <cstring>
#include <exception>
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

HRESULT CopyCoTaskMemString(const std::wstring& source, LPWSTR* value) {
    if (!value) {
        return E_POINTER;
    }
    const std::size_t bytes = (source.size() + 1) * sizeof(wchar_t);
    auto* buffer = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!buffer) {
        *value = nullptr;
        return E_OUTOFMEMORY;
    }
    std::memcpy(buffer, source.c_str(), bytes);
    *value = buffer;
    return S_OK;
}

class WebViewEnvironmentOptions final
    : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ICoreWebView2EnvironmentOptions> {
public:
    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* value) override {
        return CopyCoTaskMemString(additionalBrowserArguments_, value);
    }

    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value) override {
        additionalBrowserArguments_ = value ? value : L"";
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override {
        return CopyCoTaskMemString(language_, value);
    }

    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override {
        language_ = value ? value : L"";
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* value) override {
        return CopyCoTaskMemString(targetCompatibleBrowserVersion_, value);
    }

    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR value) override {
        targetCompatibleBrowserVersion_ = value ? value : L"";
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
    std::error_code error;
    const auto root = ModuleDirectory() / L"WebView2Runtime";
    if (std::filesystem::exists(root / L"msedgewebview2.exe", error)) {
        return root;
    }

    if (!std::filesystem::exists(root, error)) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (!entry.is_directory(error)) {
            continue;
        }
        const auto candidate = entry.path() / L"msedgewebview2.exe";
        if (std::filesystem::exists(candidate, error)) {
            return entry.path();
        }
    }
    return {};
}

}  // namespace

struct WebUiHost::Impl {
    HWND parent = nullptr;
    RECT pendingBounds{};
    std::filesystem::path webRoot;
    std::wstring initialUrl;
    std::wstring profileName;
    MessageHandler messageHandler;
    std::filesystem::path browserExecutableFolder;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    HRESULT lastCreateResult = S_OK;
    bool usedDefaultOptionsFallback = false;
    bool allowInsecureCertificates = false;

    bool Create(HWND parentWindow,
                const std::filesystem::path& root,
                std::wstring_view url,
                std::wstring_view profile,
                MessageHandler handler) {
        lastCreateResult = S_OK;
        usedDefaultOptionsFallback = false;
        parent = parentWindow;
        std::error_code error;
        webRoot = std::filesystem::absolute(root, error).lexically_normal();
        if (error) {
            webRoot = root;
        }
        initialUrl = std::wstring{url.empty() ? std::wstring_view{kWebUiBaseUrl} : url};
        profileName = profile;
        messageHandler = std::move(handler);

        RECT client{};
        GetClientRect(parent, &client);
        pendingBounds = client;

        if (!std::filesystem::exists(webRoot / L"index.html", error)) {
            lastCreateResult = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            return false;
        }

        const std::filesystem::path userData = WebViewUserDataFolder(profileName);
        std::filesystem::create_directories(userData, error);
        browserExecutableFolder = LocalWebViewRuntimeFolder();

        const auto environmentOptions = Make<WebViewEnvironmentOptions>();
        if (environmentOptions) {
            environmentOptions->put_AdditionalBrowserArguments(kWebViewBrowserArguments);
        }

        const auto createEnvironment = [this, &userData](ICoreWebView2EnvironmentOptions* options) -> HRESULT {
            return CreateCoreWebView2EnvironmentWithOptions(
                browserExecutableFolder.empty() ? nullptr : browserExecutableFolder.c_str(),
                userData.c_str(),
                options,
                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [this](HRESULT result, ICoreWebView2Environment* createdEnvironment) -> HRESULT {
                        lastCreateResult = result;
                        if (FAILED(result) || !createdEnvironment) {
                            return S_OK;
                        }

                        environment = createdEnvironment;
                        environment->CreateCoreWebView2Controller(
                            parent,
                            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [this](HRESULT controllerResult, ICoreWebView2Controller* createdController) -> HRESULT {
                                    lastCreateResult = controllerResult;
                                    if (FAILED(controllerResult) || !createdController) {
                                        return S_OK;
                                    }

                                    controller = createdController;
                                    controller->get_CoreWebView2(&webview);
                                    controller->put_Bounds(pendingBounds);
                                    controller->put_IsVisible(TRUE);

                                    if (webview) {
                                        ComPtr<ICoreWebView2Settings> settings;
                                        if (SUCCEEDED(webview->get_Settings(&settings)) && settings) {
                                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                                            settings->put_AreDevToolsEnabled(TRUE);
                                            settings->put_IsStatusBarEnabled(FALSE);
                                        }

                                        EventRegistrationToken token{};
                                        webview->add_WebMessageReceived(
                                            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                                [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                                    LPWSTR json = nullptr;
                                                    HRESULT result = S_OK;
                                                    try {
                                                        if (args && SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                                                            if (messageHandler) {
                                                                messageHandler(json);
                                                            }
                                                        }
                                                    } catch (const std::exception& error) {
                                                        std::wstring message = L"Anvil WebView message handler exception: ";
                                                        message += std::wstring(error.what(), error.what() + std::strlen(error.what()));
                                                        message += L"\n";
                                                        OutputDebugStringW(message.c_str());
                                                        result = E_FAIL;
                                                    } catch (...) {
                                                        OutputDebugStringW(L"Anvil WebView message handler unknown exception\n");
                                                        result = E_FAIL;
                                                    }
                                                    CoTaskMemFree(json);
                                                    return result;
                                                })
                                                .Get(),
                                            &token);

                                        ComPtr<ICoreWebView2_14> webview14;
                                        if (SUCCEEDED(webview.As(&webview14)) && webview14) {
                                            EventRegistrationToken certificateToken{};
                                            webview14->add_ServerCertificateErrorDetected(
                                                Callback<ICoreWebView2ServerCertificateErrorDetectedEventHandler>(
                                                    [this](ICoreWebView2*,
                                                           ICoreWebView2ServerCertificateErrorDetectedEventArgs* args) -> HRESULT {
                                                        if (allowInsecureCertificates && args) {
                                                            args->put_Action(COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_ALWAYS_ALLOW);
                                                        }
                                                        return S_OK;
                                                    })
                                                    .Get(),
                                                &certificateToken);
                                        }

                                        ComPtr<ICoreWebView2_3> webview3;
                                        if (SUCCEEDED(webview.As(&webview3)) && webview3) {
                                            webview3->SetVirtualHostNameToFolderMapping(
                                                kWebUiVirtualHost,
                                                webRoot.c_str(),
                                                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                                        }
                                        webview->Navigate(initialUrl.c_str());
                                    }

                                    return S_OK;
                                })
                                .Get());
                        return S_OK;
                    })
                    .Get());
        };

        HRESULT hr = createEnvironment(environmentOptions.Get());
        if (FAILED(hr) && environmentOptions) {
            hr = createEnvironment(nullptr);
            usedDefaultOptionsFallback = SUCCEEDED(hr);
        }

        lastCreateResult = hr;
        return SUCCEEDED(hr);
    }

    void SetAllowInsecureCertificates(const bool allow) {
        allowInsecureCertificates = allow;
        if (allow || !webview) {
            return;
        }

        ComPtr<ICoreWebView2_14> webview14;
        if (SUCCEEDED(webview.As(&webview14)) && webview14) {
            webview14->ClearServerCertificateErrorActions(
                Callback<ICoreWebView2ClearServerCertificateErrorActionsCompletedHandler>(
                    [](HRESULT) -> HRESULT {
                        return S_OK;
                    })
                    .Get());
        }
    }
};

WebUiHost::WebUiHost()
    : impl_(new Impl()) {
}

WebUiHost::~WebUiHost() {
    delete impl_;
}

bool WebUiHost::Create(HWND parent,
                       const std::filesystem::path& webRoot,
                       std::wstring_view initialUrl,
                       std::wstring_view profileName,
                       MessageHandler handler) {
    return impl_->Create(parent, webRoot, initialUrl, profileName, std::move(handler));
}

void WebUiHost::Resize(const RECT bounds) const {
    impl_->pendingBounds = bounds;
    if (impl_->controller) {
        impl_->controller->put_Bounds(bounds);
    }
}

void WebUiHost::PostJson(const std::wstring& json) const {
    if (impl_->webview) {
        impl_->webview->PostWebMessageAsJson(json.c_str());
    }
}

void WebUiHost::SetAllowInsecureCertificates(const bool allow) const {
    impl_->SetAllowInsecureCertificates(allow);
}

bool WebUiHost::Ready() const {
    return impl_->webview != nullptr;
}

HRESULT WebUiHost::LastCreateResult() const {
    return impl_->lastCreateResult;
}

bool WebUiHost::UsedDefaultOptionsFallback() const {
    return impl_->usedDefaultOptionsFallback;
}

}  // namespace anvil::app
