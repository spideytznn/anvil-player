#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace anvil::app {

class WebUiHost {
public:
    using MessageHandler = std::function<void(const std::wstring&)>;

    WebUiHost();
    ~WebUiHost();

    WebUiHost(const WebUiHost&) = delete;
    WebUiHost& operator=(const WebUiHost&) = delete;

    // Creates the WebView2 environment/controller. `initialUrl` is the virtual-host
    // URL to navigate to (e.g. "http://appassets.anvilplayer.local/index.html#/library").
    // `profileName` names a distinct WebView2 user-data subfolder so that two hosts
    // (library + player) in the same process do not collide on a shared folder.
    bool Create(HWND parent,
                const std::filesystem::path& webRoot,
                std::wstring_view initialUrl,
                std::wstring_view profileName,
                MessageHandler handler);
    void Resize(RECT bounds) const;
    void PostJson(const std::wstring& json) const;
    void SetAllowInsecureCertificates(bool allow) const;
    bool Ready() const;
    HRESULT LastCreateResult() const;
    bool UsedDefaultOptionsFallback() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace anvil::app
