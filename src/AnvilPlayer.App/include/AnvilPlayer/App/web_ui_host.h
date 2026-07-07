#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>

namespace anvil::app {

class WebUiHost {
public:
    using MessageHandler = std::function<void(const std::wstring&)>;

    WebUiHost();
    ~WebUiHost();

    WebUiHost(const WebUiHost&) = delete;
    WebUiHost& operator=(const WebUiHost&) = delete;

    bool Create(HWND parent, const std::filesystem::path& webRoot, MessageHandler handler);
    void Resize(RECT bounds) const;
    void PostJson(const std::wstring& json) const;
    bool Ready() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace anvil::app
