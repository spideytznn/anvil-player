#include "AnvilPlayer/App/web_ui_host.h"

#include <objbase.h>
#include <shlobj.h>
#include <unknwn.h>
#include <wrl.h>
#include <WebView2.h>

#include <cstring>
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
constexpr wchar_t kWebUiUrl[] = L"https://appassets.anvilplayer.local/index.html#/library";
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

std::filesystem::path WebViewUserDataFolder() {
    wchar_t localAppData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData))) {
        return std::filesystem::path(localAppData) / L"AnvilPlayer" / L"WebView2";
    }
    return std::filesystem::temp_directory_path() / L"AnvilPlayer-WebView2";
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
    MessageHandler messageHandler;
    std::filesystem::path browserExecutableFolder;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;

    bool Create(HWND parentWindow, const std::filesystem::path& root, MessageHandler handler) {
        parent = parentWindow;
        std::error_code error;
        webRoot = std::filesystem::absolute(root, error).lexically_normal();
        if (error) {
            webRoot = root;
        }
        messageHandler = std::move(handler);

        RECT client{};
        GetClientRect(parent, &client);
        pendingBounds = client;

        if (!std::filesystem::exists(webRoot / L"index.html", error)) {
            return false;
        }

        const std::filesystem::path userData = WebViewUserDataFolder();
        std::filesystem::create_directories(userData, error);
        browserExecutableFolder = LocalWebViewRuntimeFolder();

        const auto environmentOptions = Make<WebViewEnvironmentOptions>();
        if (environmentOptions) {
            environmentOptions->put_AdditionalBrowserArguments(kWebViewBrowserArguments);
        }

        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            browserExecutableFolder.empty() ? nullptr : browserExecutableFolder.c_str(),
            userData.c_str(),
            environmentOptions.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment* createdEnvironment) -> HRESULT {
                    if (FAILED(result) || !createdEnvironment) {
                        return S_OK;
                    }

                    environment = createdEnvironment;
                    environment->CreateCoreWebView2Controller(
                        parent,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT controllerResult, ICoreWebView2Controller* createdController) -> HRESULT {
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
                                                if (args && SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                                                    if (messageHandler) {
                                                        messageHandler(json);
                                                    }
                                                }
                                                CoTaskMemFree(json);
                                                return S_OK;
                                            })
                                            .Get(),
                                        &token);

                                    ComPtr<ICoreWebView2_3> webview3;
                                    if (SUCCEEDED(webview.As(&webview3)) && webview3) {
                                        webview3->SetVirtualHostNameToFolderMapping(
                                            kWebUiVirtualHost,
                                            webRoot.c_str(),
                                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                                    }
                                    webview->Navigate(kWebUiUrl);
                                }

                                return S_OK;
                            })
                            .Get());
                    return S_OK;
                })
                .Get());

        return SUCCEEDED(hr);
    }
};

WebUiHost::WebUiHost()
    : impl_(new Impl()) {
}

WebUiHost::~WebUiHost() {
    delete impl_;
}

bool WebUiHost::Create(HWND parent, const std::filesystem::path& webRoot, MessageHandler handler) {
    return impl_->Create(parent, webRoot, std::move(handler));
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

bool WebUiHost::Ready() const {
    return impl_->webview != nullptr;
}

}  // namespace anvil::app
