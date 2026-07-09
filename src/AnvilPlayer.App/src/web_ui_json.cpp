#include "AnvilPlayer/App/web_ui_json.h"

#include <cwctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace anvil::app {

std::wstring JsonEscape(const std::wstring& value) {
    std::wostringstream escaped;
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'\\':
            escaped << L"\\\\";
            break;
        case L'"':
            escaped << L"\\\"";
            break;
        case L'\b':
            escaped << L"\\b";
            break;
        case L'\f':
            escaped << L"\\f";
            break;
        case L'\n':
            escaped << L"\\n";
            break;
        case L'\r':
            escaped << L"\\r";
            break;
        case L'\t':
            escaped << L"\\t";
            break;
        default:
            if (ch < 0x20) {
                escaped << L"\\u"
                        << std::hex
                        << std::setw(4)
                        << std::setfill(L'0')
                        << static_cast<int>(ch)
                        << std::dec
                        << std::setfill(L' ');
            } else {
                escaped << ch;
            }
            break;
        }
    }
    return escaped.str();
}

bool MessageContains(std::wstring_view message, const wchar_t* needle) {
    return message.find(needle) != std::wstring_view::npos;
}

std::size_t CountOccurrences(std::wstring_view text, std::wstring_view needle) {
    if (needle.empty()) {
        return 0;
    }

    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::wstring_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::optional<double> ReadJsonNumber(std::wstring_view message, const wchar_t* field) {
    const std::wstring key = L"\"" + std::wstring(field) + L"\":";
    const std::size_t start = message.find(key);
    if (start == std::wstring_view::npos) {
        return std::nullopt;
    }

    std::size_t valueStart = start + key.size();
    while (valueStart < message.size() && std::iswspace(message[valueStart])) {
        ++valueStart;
    }

    std::size_t valueEnd = valueStart;
    while (valueEnd < message.size()) {
        const wchar_t ch = message[valueEnd];
        if ((ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'+' || ch == L'.' || ch == L'e' || ch == L'E') {
            ++valueEnd;
            continue;
        }
        break;
    }

    if (valueEnd <= valueStart) {
        return std::nullopt;
    }

    try {
        return std::stod(std::wstring(message.substr(valueStart, valueEnd - valueStart)));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::wstring> ReadJsonString(std::wstring_view message, const wchar_t* field) {
    const std::wstring key = L"\"" + std::wstring(field) + L"\":";
    const std::size_t start = message.find(key);
    if (start == std::wstring_view::npos) {
        return std::nullopt;
    }

    std::size_t valueStart = start + key.size();
    while (valueStart < message.size() && std::iswspace(message[valueStart])) {
        ++valueStart;
    }
    if (valueStart >= message.size() || message[valueStart] != L'"') {
        return std::nullopt;
    }
    ++valueStart;

    std::wstring value;
    for (std::size_t index = valueStart; index < message.size(); ++index) {
        const wchar_t ch = message[index];
        if (ch == L'"') {
            return value;
        }
        if (ch != L'\\') {
            value.push_back(ch);
            continue;
        }
        if (++index >= message.size()) {
            return std::nullopt;
        }
        const wchar_t escaped = message[index];
        switch (escaped) {
        case L'"': value.push_back(L'"'); break;
        case L'\\': value.push_back(L'\\'); break;
        case L'/': value.push_back(L'/'); break;
        case L'b': value.push_back(L'\b'); break;
        case L'f': value.push_back(L'\f'); break;
        case L'n': value.push_back(L'\n'); break;
        case L'r': value.push_back(L'\r'); break;
        case L't': value.push_back(L'\t'); break;
        case L'u': {
            if (index + 4 >= message.size()) {
                return std::nullopt;
            }
            int code = 0;
            for (int digit = 0; digit < 4; ++digit) {
                const wchar_t hex = message[++index];
                code <<= 4;
                if (hex >= L'0' && hex <= L'9') {
                    code += static_cast<int>(hex - L'0');
                } else if (hex >= L'a' && hex <= L'f') {
                    code += 10 + static_cast<int>(hex - L'a');
                } else if (hex >= L'A' && hex <= L'F') {
                    code += 10 + static_cast<int>(hex - L'A');
                } else {
                    return std::nullopt;
                }
            }
            value.push_back(static_cast<wchar_t>(code));
            break;
        }
        default:
            value.push_back(escaped);
            break;
        }
    }
    return std::nullopt;
}

}  // namespace anvil::app
