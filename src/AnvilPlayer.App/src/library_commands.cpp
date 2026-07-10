#include "AnvilPlayer/App/library_commands.h"

#include "AnvilPlayer/App/string_util.h"
#include "AnvilPlayer/App/web_ui_json.h"

#include <lm.h>
#include <winhttp.h>
#include <winnetwk.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <deque>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

namespace anvil::app {

std::wstring LowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool IsNetworkMediaPath(const std::filesystem::path& path) {
    const std::wstring value = LowerCopy(path.wstring());
    return value.rfind(L"http://", 0) == 0 ||
           value.rfind(L"https://", 0) == 0;
}

std::filesystem::path NormalizeListPath(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

std::wstring TrimWhitespace(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](const wchar_t ch) {
        return std::iswspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](const wchar_t ch) {
        return std::iswspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::wstring(first, last);
}

bool StartsWithInsensitive(const std::wstring& value, const std::wstring& prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    return LowerCopy(value.substr(0, prefix.size())) == LowerCopy(prefix);
}

std::wstring NormalizeSmbPathText(std::wstring value) {
    value = TrimWhitespace(std::move(value));
    if (value.empty()) {
        return {};
    }

    if (StartsWithInsensitive(value, L"smb://")) {
        value = value.substr(6);
        std::replace(value.begin(), value.end(), L'/', L'\\');
        while (!value.empty() && value.front() == L'\\') {
            value.erase(value.begin());
        }
        return L"\\\\" + value;
    }

    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.rfind(L"\\\\", 0) == 0) {
        return value;
    }
    while (!value.empty() && value.front() == L'\\') {
        value.erase(value.begin());
    }
    return L"\\\\" + value;
}

std::vector<std::wstring> SmbUncParts(const std::wstring& path) {
    std::wstring text = NormalizeSmbPathText(path);
    if (text.rfind(L"\\\\", 0) != 0) {
        return {};
    }
    text.erase(0, 2);

    std::vector<std::wstring> parts;
    std::wstring current;
    for (const wchar_t ch : text) {
        if (ch == L'\\') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::wstring SmbServerRootFromHost(const std::wstring& host) {
    const auto parts = SmbUncParts(host);
    if (parts.empty()) {
        return {};
    }
    return L"\\\\" + parts[0];
}

std::wstring SmbShareRootFromPath(const std::filesystem::path& path) {
    const auto parts = SmbUncParts(path.wstring());
    if (parts.size() < 2) {
        return {};
    }
    return L"\\\\" + parts[0] + L"\\" + parts[1];
}

std::wstring WindowsErrorMessage(const DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring message = length > 0 && buffer ? std::wstring(buffer, length) : L"Windows 错误";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || std::iswspace(message.back()))) {
        message.pop_back();
    }
    return message + L" (" + std::to_wstring(code) + L")";
}

std::optional<std::wstring> ConnectSmbRemote(std::wstring remoteName, const SmbCredentials& credentials) {
    if (remoteName.empty() || (credentials.username.empty() && credentials.password.empty())) {
        return std::nullopt;
    }

    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_ANY;
    resource.lpRemoteName = remoteName.data();
    const wchar_t* username = credentials.username.empty() ? nullptr : credentials.username.c_str();
    const wchar_t* password = credentials.password.empty() ? nullptr : credentials.password.c_str();
    const DWORD result = WNetAddConnection2W(&resource, password, username, CONNECT_TEMPORARY);
    if (result == NO_ERROR || result == ERROR_ALREADY_ASSIGNED || result == ERROR_DEVICE_ALREADY_REMEMBERED) {
        return std::nullopt;
    }
    if (result == ERROR_SESSION_CREDENTIAL_CONFLICT) {
        return L"当前服务器已经用另一个账号建立了 SMB 连接，请先在 Windows 中断开该连接后重试。";
    }
    return WindowsErrorMessage(result);
}

std::optional<std::wstring> ConnectSmbPathForAccess(const std::filesystem::path& path, const SmbCredentials& credentials) {
    const auto shareRoot = SmbShareRootFromPath(path);
    if (shareRoot.empty()) {
        return std::nullopt;
    }
    return ConnectSmbRemote(shareRoot, credentials);
}

bool ContainsInsensitive(const std::wstring& value, const std::wstring& needle) {
    return LowerCopy(value).find(LowerCopy(needle)) != std::wstring::npos;
}

bool IsMediaFilePath(const std::filesystem::path& path) {
    const std::wstring extension = LowerCopy(path.extension().wstring());
    return extension == L".mp4" ||
           extension == L".mkv" ||
           extension == L".mov" ||
           extension == L".m4v" ||
           extension == L".m2ts" ||
           extension == L".ts" ||
           extension == L".webm" ||
           extension == L".avi" ||
           extension == L".wmv" ||
           extension == L".mpg" ||
           extension == L".mpeg";
}

std::vector<std::filesystem::path> MediaFilesInFolder(const std::filesystem::path& mediaPath) {
    std::vector<std::filesystem::path> entries;
    const auto folder = mediaPath.parent_path();
    if (folder.empty()) {
        return entries;
    }

    std::error_code error;
    std::filesystem::directory_iterator iterator(folder, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        std::error_code entryError;
        if (iterator->is_regular_file(entryError)) {
            const auto entryPath = NormalizeListPath(iterator->path());
            if (IsMediaFilePath(entryPath)) {
                entries.push_back(entryPath);
            }
        }
        iterator.increment(error);
    }

    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return LowerCopy(lhs.filename().wstring()) < LowerCopy(rhs.filename().wstring());
    });
    return entries;
}

std::int64_t LastWriteTimeUnixMilliseconds(const std::filesystem::path& path) {
    std::error_code error;
    const auto fileTime = std::filesystem::last_write_time(path, error);
    if (error) {
        return 0;
    }
    const auto systemTime = std::chrono::time_point_cast<std::chrono::milliseconds>(
        fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return systemTime.time_since_epoch().count();
}

std::vector<LocalFolderScanItem> ScanMediaFilesInLocalFolder(const std::filesystem::path& root, bool& truncated) {
    truncated = false;
    std::vector<LocalFolderScanItem> entries;

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;

    while (!error && iterator != end) {
        std::error_code entryError;
        if (iterator.depth() >= kMaxLocalFolderScanDepth && iterator->is_directory(entryError)) {
            iterator.disable_recursion_pending();
        }

        entryError.clear();
        if (iterator->is_regular_file(entryError)) {
            const auto entryPath = NormalizeListPath(iterator->path());
            if (IsMediaFilePath(entryPath)) {
                std::error_code sizeError;
                const auto fileSize = std::filesystem::file_size(entryPath, sizeError);
                entries.push_back(LocalFolderScanItem{
                    entryPath,
                    L"",
                    LastWriteTimeUnixMilliseconds(entryPath),
                    sizeError ? 0 : fileSize,
                });
                if (entries.size() >= kMaxLocalFolderScanItems) {
                    truncated = true;
                    break;
                }
            }
        }

        iterator.increment(error);
    }

    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.modifiedAtMs != rhs.modifiedAtMs) {
            return lhs.modifiedAtMs > rhs.modifiedAtMs;
        }
        return LowerCopy(lhs.path.filename().wstring()) < LowerCopy(rhs.path.filename().wstring());
    });
    return entries;
}

std::vector<SmbDirectoryEntry> ListSmbShares(const std::wstring& host, const SmbCredentials& credentials, std::wstring& errorMessage) {
    const std::wstring serverRoot = SmbServerRootFromHost(host);
    if (serverRoot.empty()) {
        errorMessage = L"请填写有效的 SMB 主机或 IP。";
        return {};
    }

    const auto ipcError = ConnectSmbRemote(serverRoot + L"\\IPC$", credentials);
    SHARE_INFO_1* buffer = nullptr;
    DWORD entriesRead = 0;
    DWORD totalEntries = 0;
    DWORD resumeHandle = 0;
    std::vector<SmbDirectoryEntry> directories;
    NET_API_STATUS status = NERR_Success;
    do {
        status = NetShareEnum(
            const_cast<LPWSTR>(serverRoot.c_str()),
            1,
            reinterpret_cast<LPBYTE*>(&buffer),
            MAX_PREFERRED_LENGTH,
            &entriesRead,
            &totalEntries,
            &resumeHandle);
        if (status == NERR_Success || status == ERROR_MORE_DATA) {
            for (DWORD index = 0; index < entriesRead; ++index) {
                const SHARE_INFO_1& share = buffer[index];
                if (!share.shi1_netname) {
                    continue;
                }
                const std::wstring name = share.shi1_netname;
                const DWORD type = share.shi1_type & STYPE_MASK;
                if (type != STYPE_DISKTREE || (!name.empty() && name.back() == L'$')) {
                    continue;
                }
                directories.push_back(SmbDirectoryEntry{name, std::filesystem::path(serverRoot + L"\\" + name)});
            }
        }
        if (buffer) {
            NetApiBufferFree(buffer);
            buffer = nullptr;
        }
    } while (status == ERROR_MORE_DATA);

    if (status != NERR_Success) {
        errorMessage = ipcError.value_or(WindowsErrorMessage(status));
        return {};
    }

    std::sort(directories.begin(), directories.end(), [](const auto& left, const auto& right) {
        return LowerCopy(left.name) < LowerCopy(right.name);
    });
    return directories;
}

std::vector<SmbDirectoryEntry> ListSmbChildDirectories(const std::filesystem::path& path,
                                                       const SmbCredentials& credentials,
                                                       std::wstring& errorMessage) {
    const auto connectError = ConnectSmbPathForAccess(path, credentials);
    if (connectError) {
        errorMessage = *connectError;
        return {};
    }

    std::vector<SmbDirectoryEntry> directories;
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        path,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    if (error) {
        errorMessage = L"无法读取 SMB 文件夹：" + Utf8ToWide(error.message().c_str());
        return {};
    }

    while (iterator != end) {
        std::error_code entryError;
        if (iterator->is_directory(entryError)) {
            const auto entryPath = NormalizeListPath(iterator->path());
            const auto name = entryPath.filename().empty() ? entryPath.wstring() : entryPath.filename().wstring();
            directories.push_back(SmbDirectoryEntry{name, entryPath});
        }
        iterator.increment(error);
        if (error) {
            break;
        }
    }

    std::sort(directories.begin(), directories.end(), [](const auto& left, const auto& right) {
        return LowerCopy(left.name) < LowerCopy(right.name);
    });
    return directories;
}

std::string Base64Encode(const std::string& input) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    int value = 0;
    int valueBits = -6;
    for (const unsigned char byte : input) {
        value = (value << 8) + byte;
        valueBits += 8;
        while (valueBits >= 0) {
            output.push_back(kAlphabet[(value >> valueBits) & 0x3F]);
            valueBits -= 6;
        }
    }
    if (valueBits > -6) {
        output.push_back(kAlphabet[((value << 8) >> (valueBits + 8)) & 0x3F]);
    }
    while (output.size() % 4) {
        output.push_back('=');
    }
    return output;
}

bool IsUrlSafeByte(const unsigned char byte) {
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9') ||
           byte == '-' ||
           byte == '_' ||
           byte == '.' ||
           byte == '~';
}

std::wstring UrlEncodeUserInfo(const std::wstring& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    const auto bytes = WideToUtf8(value);
    std::wstring output;
    for (const unsigned char byte : bytes) {
        if (IsUrlSafeByte(byte)) {
            output.push_back(static_cast<wchar_t>(byte));
            continue;
        }
        output.push_back(L'%');
        output.push_back(static_cast<wchar_t>(kHex[(byte >> 4) & 0x0F]));
        output.push_back(static_cast<wchar_t>(kHex[byte & 0x0F]));
    }
    return output;
}

int HexValue(const wchar_t ch);

bool IsPercentEscape(const std::wstring& value, const std::size_t index) {
    return index + 2 < value.size() &&
           value[index] == L'%' &&
           HexValue(value[index + 1]) >= 0 &&
           HexValue(value[index + 2]) >= 0;
}

std::wstring UrlEncodePathSegmentPreservingEscapes(const std::wstring& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::wstring output;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const wchar_t ch = value[index];
        if (IsPercentEscape(value, index)) {
            output.push_back(ch);
            output.push_back(value[index + 1]);
            output.push_back(value[index + 2]);
            index += 2;
            continue;
        }
        if ((ch >= L'A' && ch <= L'Z') ||
            (ch >= L'a' && ch <= L'z') ||
            (ch >= L'0' && ch <= L'9') ||
            ch == L'-' ||
            ch == L'_' ||
            ch == L'.' ||
            ch == L'~') {
            output.push_back(ch);
            continue;
        }

        const auto bytes = WideToUtf8(std::wstring(1, ch));
        for (const unsigned char byte : bytes) {
            output.push_back(L'%');
            output.push_back(static_cast<wchar_t>(kHex[(byte >> 4) & 0x0F]));
            output.push_back(static_cast<wchar_t>(kHex[byte & 0x0F]));
        }
    }
    return output;
}

int HexValue(const wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return static_cast<int>(ch - L'0');
    if (ch >= L'a' && ch <= L'f') return 10 + static_cast<int>(ch - L'a');
    if (ch >= L'A' && ch <= L'F') return 10 + static_cast<int>(ch - L'A');
    return -1;
}

std::wstring UrlPercentDecodeUtf8(const std::wstring& value) {
    std::string bytes;
    bytes.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == L'%' && index + 2 < value.size()) {
            const int high = HexValue(value[index + 1]);
            const int low = HexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                bytes.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        if (value[index] < 0x80) {
            bytes.push_back(static_cast<char>(value[index]));
        }
    }
    return Utf8ToWide(bytes.c_str());
}

std::wstring XmlDecode(std::wstring value) {
    const std::pair<const wchar_t*, const wchar_t*> replacements[] = {
        {L"&amp;", L"&"},
        {L"&lt;", L"<"},
        {L"&gt;", L">"},
        {L"&quot;", L"\""},
        {L"&apos;", L"'"},
    };
    for (const auto& [from, to] : replacements) {
        std::size_t pos = 0;
        const std::wstring source = from;
        while ((pos = value.find(source, pos)) != std::wstring::npos) {
            value.replace(pos, source.size(), to);
            pos += std::wcslen(to);
        }
    }
    return value;
}

std::wstring ElementLocalName(std::wstring tagName) {
    if (!tagName.empty() && tagName.front() == L'/') {
        tagName.erase(tagName.begin());
    }
    const auto colon = tagName.find(L':');
    if (colon != std::wstring::npos) {
        tagName.erase(0, colon + 1);
    }
    return LowerCopy(tagName);
}

bool ReadStartTag(const std::wstring& xml, const std::size_t start, std::wstring& tagName, std::size_t& tagEnd) {
    if (start >= xml.size() || xml[start] != L'<') {
        return false;
    }
    std::size_t nameStart = start + 1;
    if (nameStart >= xml.size() || xml[nameStart] == L'/' || xml[nameStart] == L'!' || xml[nameStart] == L'?') {
        return false;
    }
    std::size_t nameEnd = nameStart;
    while (nameEnd < xml.size() &&
           !std::iswspace(xml[nameEnd]) &&
           xml[nameEnd] != L'>' &&
           xml[nameEnd] != L'/') {
        ++nameEnd;
    }
    tagEnd = xml.find(L'>', nameEnd);
    if (tagEnd == std::wstring::npos || nameEnd <= nameStart) {
        return false;
    }
    tagName = xml.substr(nameStart, nameEnd - nameStart);
    return true;
}

std::optional<std::wstring> FirstElementText(const std::wstring& xml, const std::wstring& localName) {
    std::size_t pos = 0;
    const std::wstring wanted = LowerCopy(localName);
    while ((pos = xml.find(L'<', pos)) != std::wstring::npos) {
        std::wstring tagName;
        std::size_t tagEnd = 0;
        if (!ReadStartTag(xml, pos, tagName, tagEnd)) {
            ++pos;
            continue;
        }
        if (ElementLocalName(tagName) == wanted) {
            const auto close = xml.find(L"</" + tagName, tagEnd + 1);
            if (close == std::wstring::npos) {
                return std::nullopt;
            }
            return XmlDecode(xml.substr(tagEnd + 1, close - tagEnd - 1));
        }
        pos = tagEnd + 1;
    }
    return std::nullopt;
}

bool HasElement(const std::wstring& xml, const std::wstring& localName) {
    std::size_t pos = 0;
    const std::wstring wanted = LowerCopy(localName);
    while ((pos = xml.find(L'<', pos)) != std::wstring::npos) {
        std::wstring tagName;
        std::size_t tagEnd = 0;
        if (!ReadStartTag(xml, pos, tagName, tagEnd)) {
            ++pos;
            continue;
        }
        if (ElementLocalName(tagName) == wanted) {
            return true;
        }
        pos = tagEnd + 1;
    }
    return false;
}

std::optional<WebDavUrlParts> CrackWebDavUrl(const std::wstring& url) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        return std::nullopt;
    }
    WebDavUrlParts parts;
    parts.scheme.assign(components.lpszScheme, components.dwSchemeLength);
    parts.host.assign(components.lpszHostName, components.dwHostNameLength);
    parts.port = components.nPort;
    parts.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    if (components.lpszUrlPath && components.dwUrlPathLength > 0) {
        parts.pathAndQuery.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo && components.dwExtraInfoLength > 0) {
        parts.pathAndQuery.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (parts.pathAndQuery.empty()) {
        parts.pathAndQuery = L"/";
    }
    return parts;
}

std::wstring WebDavOrigin(const std::wstring& url) {
    const auto parts = CrackWebDavUrl(url);
    if (!parts) {
        return {};
    }
    std::wostringstream origin;
    origin << parts->scheme << L"://" << parts->host;
    const bool defaultPort = (parts->secure && parts->port == INTERNET_DEFAULT_HTTPS_PORT) ||
                             (!parts->secure && parts->port == INTERNET_DEFAULT_HTTP_PORT);
    if (!defaultPort) {
        origin << L":" << parts->port;
    }
    return origin.str();
}

std::wstring NormalizeWebDavUrlText(std::wstring url, const bool ensureTrailingSlash) {
    url = TrimWhitespace(std::move(url));
    const auto hash = url.find(L'#');
    if (hash != std::wstring::npos) {
        url.erase(hash);
    }
    const auto query = url.find(L'?');
    if (query != std::wstring::npos) {
        url.erase(query);
    }
    if (const auto parts = CrackWebDavUrl(url)) {
        std::wstring normalizedPath;
        normalizedPath.reserve(parts->pathAndQuery.size() + 1);
        bool previousSlash = false;
        for (const wchar_t ch : parts->pathAndQuery) {
            if (ch == L'/') {
                if (!previousSlash) {
                    normalizedPath.push_back(ch);
                }
                previousSlash = true;
                continue;
            }
            normalizedPath.push_back(ch);
            previousSlash = false;
        }
        if (normalizedPath.empty() || normalizedPath.front() != L'/') {
            normalizedPath.insert(normalizedPath.begin(), L'/');
        }
        while (normalizedPath.size() > 1 && normalizedPath.back() == L'/') {
            normalizedPath.pop_back();
        }

        std::wstring encodedPath = L"/";
        std::size_t segmentStart = normalizedPath.front() == L'/' ? 1 : 0;
        while (segmentStart < normalizedPath.size()) {
            const auto segmentEnd = normalizedPath.find(L'/', segmentStart);
            const auto segment = normalizedPath.substr(
                segmentStart,
                segmentEnd == std::wstring::npos ? std::wstring::npos : segmentEnd - segmentStart);
            if (!segment.empty()) {
                if (encodedPath.size() > 1 && encodedPath.back() != L'/') {
                    encodedPath.push_back(L'/');
                }
                encodedPath += UrlEncodePathSegmentPreservingEscapes(segment);
            }
            if (segmentEnd == std::wstring::npos) {
                break;
            }
            segmentStart = segmentEnd + 1;
        }

        std::wostringstream rebuilt;
        rebuilt << parts->scheme << L"://" << parts->host;
        const bool defaultPort = (parts->secure && parts->port == INTERNET_DEFAULT_HTTPS_PORT) ||
                                 (!parts->secure && parts->port == INTERNET_DEFAULT_HTTP_PORT);
        if (!defaultPort) {
            rebuilt << L":" << parts->port;
        }
        rebuilt << encodedPath;
        url = rebuilt.str();
    }
    if (ensureTrailingSlash && !url.empty() && url.back() != L'/') {
        url.push_back(L'/');
    }
    return url;
}

std::wstring AbsoluteWebDavHref(const std::wstring& baseUrl, std::wstring href) {
    href = XmlDecode(std::move(href));
    for (std::size_t index = 0; (index = href.find(L'#', index)) != std::wstring::npos; index += 3) {
        href.replace(index, 1, L"%23");
    }
    if (StartsWithInsensitive(href, L"http://") || StartsWithInsensitive(href, L"https://")) {
        return NormalizeWebDavUrlText(href, false);
    }
    const auto origin = WebDavOrigin(baseUrl);
    if (href.rfind(L"/", 0) == 0) {
        return NormalizeWebDavUrlText(origin + href, false);
    }
    return NormalizeWebDavUrlText(NormalizeWebDavUrlText(baseUrl) + href, false);
}

std::wstring WebDavChildUrlFromHref(const std::wstring& baseUrl, const std::wstring& href, const bool isDirectory) {
    std::wstring childUrl = AbsoluteWebDavHref(baseUrl, href);
    if (childUrl.empty()) {
        return {};
    }
    if (isDirectory && childUrl.back() != L'/') {
        childUrl.push_back(L'/');
    }
    return childUrl;
}

bool SameWebDavResource(const std::wstring& left, const std::wstring& right) {
    auto normalize = [](std::wstring value) {
        value = LowerCopy(value);
        while (!value.empty() && value.back() == L'/') {
            value.pop_back();
        }
        return value;
    };
    return normalize(left) == normalize(right);
}

std::wstring WebDavNameFromUrl(std::wstring url, const std::wstring& fallback) {
    while (!url.empty() && url.back() == L'/') {
        url.pop_back();
    }
    const auto slash = url.find_last_of(L'/');
    if (slash == std::wstring::npos || slash + 1 >= url.size()) {
        return fallback;
    }
    const auto decoded = UrlPercentDecodeUtf8(url.substr(slash + 1));
    return decoded.empty() ? fallback : decoded;
}

std::wstring WebDavExtension(const std::wstring& url) {
    std::wstring path = url;
    const auto query = path.find_first_of(L"?#");
    if (query != std::wstring::npos) {
        path.erase(query);
    }
    const auto slash = path.find_last_of(L'/');
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return {};
    }
    return LowerCopy(path.substr(dot));
}

std::wstring CredentialedWebDavUrl(const std::wstring& url, const std::wstring& username, const std::wstring& password) {
    if (username.empty() && password.empty()) {
        return url;
    }
    const auto parts = CrackWebDavUrl(url);
    if (!parts) {
        return url;
    }
    std::wostringstream output;
    output << parts->scheme << L"://" << UrlEncodeUserInfo(username) << L":" << UrlEncodeUserInfo(password)
           << L"@" << parts->host;
    const bool defaultPort = (parts->secure && parts->port == INTERNET_DEFAULT_HTTPS_PORT) ||
                             (!parts->secure && parts->port == INTERNET_DEFAULT_HTTP_PORT);
    if (!defaultPort) {
        output << L":" << parts->port;
    }
    output << parts->pathAndQuery;
    return output.str();
}

std::vector<WebDavEntry> ParseWebDavEntries(const std::wstring& baseUrl, const std::string& xmlUtf8) {
    const std::wstring xml = Utf8ToWide(xmlUtf8.c_str());
    std::vector<WebDavEntry> entries;
    std::size_t pos = 0;
    while ((pos = xml.find(L'<', pos)) != std::wstring::npos) {
        std::wstring tagName;
        std::size_t tagEnd = 0;
        if (!ReadStartTag(xml, pos, tagName, tagEnd)) {
            ++pos;
            continue;
        }
        if (ElementLocalName(tagName) != L"response") {
            pos = tagEnd + 1;
            continue;
        }
        const auto close = xml.find(L"</" + tagName, tagEnd + 1);
        if (close == std::wstring::npos) {
            break;
        }
        const auto segment = xml.substr(tagEnd + 1, close - tagEnd - 1);
        const auto href = FirstElementText(segment, L"href");
        if (href && !href->empty()) {
            const auto absoluteHref = AbsoluteWebDavHref(baseUrl, *href);
            if (!SameWebDavResource(absoluteHref, baseUrl)) {
                const bool isDirectory = HasElement(segment, L"collection");
                const auto childUrl = WebDavChildUrlFromHref(baseUrl, *href, isDirectory);
                if (childUrl.empty()) {
                    pos = close + tagName.size() + 3;
                    continue;
                }
                const auto sizeText = FirstElementText(segment, L"getcontentlength").value_or(L"0");
                std::uintmax_t sizeBytes = 0;
                try {
                    sizeBytes = static_cast<std::uintmax_t>(std::stoull(sizeText));
                } catch (...) {
                    sizeBytes = 0;
                }
                entries.push_back(WebDavEntry{
                    childUrl,
                    isDirectory,
                    0,
                    sizeBytes,
                });
            }
        }
        pos = close + tagName.size() + 3;
    }
    return entries;
}

std::vector<WebDavEntry> WebDavPropFind(const std::wstring& url,
                                        const std::wstring& username,
                                        const std::wstring& password,
                                        std::wstring& errorMessage) {
    const auto normalizedUrl = NormalizeWebDavUrlText(url);
    const auto parts = CrackWebDavUrl(normalizedUrl);
    if (!parts) {
        errorMessage = L"WebDAV 地址无效。";
        return {};
    }

    HINTERNET session = WinHttpOpen(
        L"AnvilPlayer/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        errorMessage = L"WebDAV 初始化失败：" + WindowsErrorMessage(GetLastError());
        return {};
    }

    HINTERNET connection = WinHttpConnect(session, parts->host.c_str(), parts->port, 0);
    if (!connection) {
        errorMessage = L"WebDAV 连接失败：" + WindowsErrorMessage(GetLastError());
        WinHttpCloseHandle(session);
        return {};
    }

    const DWORD flags = parts->secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"PROPFIND",
        parts->pathAndQuery.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!request) {
        errorMessage = L"WebDAV 请求创建失败：" + WindowsErrorMessage(GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return {};
    }

    std::wstring headers = L"Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n";
    if (!username.empty() || !password.empty()) {
        headers += L"Authorization: Basic " + Utf8ToWide(Base64Encode(WideToUtf8(username + L":" + password)).c_str()) + L"\r\n";
    }
    const std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><propfind xmlns=\"DAV:\"><prop><resourcetype/><getcontentlength/><getlastmodified/></prop></propfind>";
    const BOOL sent = WinHttpSendRequest(
        request,
        headers.c_str(),
        static_cast<DWORD>(headers.size()),
        const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        errorMessage = L"WebDAV 请求失败：" + WindowsErrorMessage(GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return {};
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200 && statusCode != 207) {
        errorMessage = L"WebDAV 请求失败：HTTP " + std::to_wstring(statusCode) + L" · " + normalizedUrl;
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return {};
    }

    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
            break;
        }
        chunk.resize(read);
        response += chunk;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ParseWebDavEntries(normalizedUrl, response);
}

std::vector<SmbDirectoryEntry> ListWebDavDirectories(const std::wstring& url,
                                                     const std::wstring& username,
                                                     const std::wstring& password,
                                                     std::wstring& errorMessage) {
    const auto entries = WebDavPropFind(url, username, password, errorMessage);
    std::vector<SmbDirectoryEntry> directories;
    for (const auto& entry : entries) {
        if (!entry.isDirectory) {
            continue;
        }
        const auto normalized = NormalizeWebDavUrlText(entry.href);
        directories.push_back(SmbDirectoryEntry{
            WebDavNameFromUrl(normalized, L"WebDAV"),
            std::filesystem::path(normalized),
        });
    }
    std::sort(directories.begin(), directories.end(), [](const auto& left, const auto& right) {
        return LowerCopy(left.name) < LowerCopy(right.name);
    });
    return directories;
}

std::vector<LocalFolderScanItem> ScanWebDavMediaFiles(const std::wstring& rootUrl,
                                                      const std::wstring& username,
                                                      const std::wstring& password,
                                                      bool& truncated,
                                                      std::wstring& errorMessage) {
    truncated = false;
    std::vector<LocalFolderScanItem> items;
    std::deque<std::pair<std::wstring, int>> queue;
    std::set<std::wstring> seenDirectories;
    std::set<std::wstring> seenFiles;
    queue.push_back({NormalizeWebDavUrlText(rootUrl), 0});

    while (!queue.empty() && items.size() < kMaxLocalFolderScanItems) {
        const auto [currentUrl, depth] = queue.front();
        queue.pop_front();
        const auto directoryKey = LowerCopy(NormalizeWebDavUrlText(currentUrl));
        if (!seenDirectories.insert(directoryKey).second) {
            continue;
        }

        std::wstring propFindError;
        const auto entries = WebDavPropFind(currentUrl, username, password, propFindError);
        if (!propFindError.empty()) {
            errorMessage = propFindError;
            return items;
        }

        for (const auto& entry : entries) {
            if (entry.isDirectory) {
                if (depth < kMaxLocalFolderScanDepth) {
                    queue.push_back({NormalizeWebDavUrlText(entry.href), depth + 1});
                }
                continue;
            }

            const auto extension = WebDavExtension(entry.href);
            if (extension != L".mp4" &&
                extension != L".mkv" &&
                extension != L".mov" &&
                extension != L".m4v" &&
                extension != L".m2ts" &&
                extension != L".ts" &&
                extension != L".webm" &&
                extension != L".avi" &&
                extension != L".wmv" &&
                extension != L".mpg" &&
                extension != L".mpeg") {
                continue;
            }
            const auto fileKey = LowerCopy(entry.href);
            if (!seenFiles.insert(fileKey).second) {
                continue;
            }
            items.push_back(LocalFolderScanItem{
                std::filesystem::path(entry.href),
                CredentialedWebDavUrl(entry.href, username, password),
                entry.modifiedAtMs,
                entry.sizeBytes,
            });
            if (items.size() >= kMaxLocalFolderScanItems) {
                truncated = true;
                break;
            }
        }
    }

    std::sort(items.begin(), items.end(), [](const auto& left, const auto& right) {
        return LowerCopy(left.path.filename().wstring()) < LowerCopy(right.path.filename().wstring());
    });
    return items;
}

std::wstring MediaPathItemsJson(const std::vector<std::filesystem::path>& paths, const std::size_t maxCount) {
    std::wostringstream json;
    json << L"[";
    const std::size_t count = std::min(paths.size(), maxCount);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& path = paths[index];
        const std::wstring name = path.filename().empty() ? path.wstring() : path.filename().wstring();
        if (index > 0) {
            json << L",";
        }
        json << L"{\"name\":\"" << JsonEscape(name) << L"\",\"path\":\"" << JsonEscape(path.wstring()) << L"\"}";
    }
    json << L"]";
    return json.str();
}

std::wstring LocalFolderScanItemsJson(const std::vector<LocalFolderScanItem>& items) {
    std::wostringstream json;
    json << L"[";
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const std::wstring name = item.path.filename().empty() ? item.path.wstring() : item.path.filename().wstring();
        if (index > 0) {
            json << L",";
        }
        json << L"{\"name\":\"" << JsonEscape(name)
             << L"\",\"path\":\"" << JsonEscape(item.path.wstring())
             << L"\"";
        if (!item.playbackPath.empty()) {
            json << L",\"playbackPath\":\"" << JsonEscape(item.playbackPath) << L"\"";
        }
        json << L",\"modifiedAt\":" << item.modifiedAtMs
             << L",\"sizeBytes\":" << item.sizeBytes
             << L"}";
    }
    json << L"]";
    return json.str();
}

std::wstring LocalFolderJsonPrefix(const wchar_t* type, const std::filesystem::path& folder) {
    const std::wstring folderName = folder.filename().empty() ? folder.wstring() : folder.filename().wstring();
    std::wostringstream json;
    json << L"{\"type\":\"" << type << L"\",";
    json << L"\"folder\":{\"name\":\"" << JsonEscape(folderName)
         << L"\",\"path\":\"" << JsonEscape(folder.wstring()) << L"\"}";
    return json.str();
}

std::wstring LocalFolderPickedJson(const std::filesystem::path& folder,
                                   const std::vector<LocalFolderScanItem>& items,
                                   const bool truncated,
                                   const bool scanPending) {
    std::wostringstream json;
    json << LocalFolderJsonPrefix(L"localFolderPicked", folder) << L",";
    json << L"\"items\":" << LocalFolderScanItemsJson(items) << L",";
    json << L"\"truncated\":" << (truncated ? L"true" : L"false");
    if (scanPending) {
        json << L",\"scanPending\":true";
    }
    json << L"}";
    return json.str();
}

std::wstring LocalFolderScanCompletedJson(const std::filesystem::path& folder,
                                          const std::vector<LocalFolderScanItem>& items,
                                          const bool truncated) {
    std::wostringstream json;
    json << LocalFolderJsonPrefix(L"localFolderScanCompleted", folder) << L",";
    json << L"\"items\":" << LocalFolderScanItemsJson(items) << L",";
    json << L"\"truncated\":" << (truncated ? L"true" : L"false");
    json << L"}";
    return json.str();
}

std::wstring LocalFolderScanFailedJson(const std::filesystem::path& folder, const std::wstring& message) {
    std::wostringstream json;
    json << LocalFolderJsonPrefix(L"localFolderScanFailed", folder) << L",";
    json << L"\"message\":\"" << JsonEscape(message) << L"\"";
    json << L"}";
    return json.str();
}

std::wstring SmbDirectoriesJson(const std::vector<SmbDirectoryEntry>& directories) {
    std::wostringstream json;
    json << L"[";
    for (std::size_t index = 0; index < directories.size(); ++index) {
        const auto& directory = directories[index];
        if (index > 0) {
            json << L",";
        }
        json << L"{\"name\":\"" << JsonEscape(directory.name)
             << L"\",\"path\":\"" << JsonEscape(directory.path.wstring()) << L"\"}";
    }
    json << L"]";
    return json.str();
}

std::wstring SmbDirectoryListedJson(const std::wstring& requestId,
                                    const std::filesystem::path& path,
                                    const std::vector<SmbDirectoryEntry>& directories) {
    std::wostringstream json;
    json << L"{\"type\":\"smbDirectoryListed\",";
    json << L"\"requestId\":\"" << JsonEscape(requestId) << L"\",";
    json << L"\"path\":\"" << JsonEscape(path.wstring()) << L"\",";
    json << L"\"directories\":" << SmbDirectoriesJson(directories);
    json << L"}";
    return json.str();
}

std::wstring SmbDirectoryFailedJson(const std::wstring& requestId,
                                    const std::filesystem::path& path,
                                    const std::wstring& message) {
    std::wostringstream json;
    json << L"{\"type\":\"smbDirectoryFailed\",";
    json << L"\"requestId\":\"" << JsonEscape(requestId) << L"\",";
    json << L"\"path\":\"" << JsonEscape(path.wstring()) << L"\",";
    json << L"\"message\":\"" << JsonEscape(message) << L"\"";
    json << L"}";
    return json.str();
}

std::wstring WebDavDirectoryListedJson(const std::wstring& requestId,
                                       const std::wstring& path,
                                       const std::vector<SmbDirectoryEntry>& directories) {
    std::wostringstream json;
    json << L"{\"type\":\"webDavDirectoryListed\",";
    json << L"\"requestId\":\"" << JsonEscape(requestId) << L"\",";
    json << L"\"path\":\"" << JsonEscape(path) << L"\",";
    json << L"\"directories\":" << SmbDirectoriesJson(directories);
    json << L"}";
    return json.str();
}

std::wstring WebDavDirectoryFailedJson(const std::wstring& requestId,
                                       const std::wstring& path,
                                       const std::wstring& message) {
    std::wostringstream json;
    json << L"{\"type\":\"webDavDirectoryFailed\",";
    json << L"\"requestId\":\"" << JsonEscape(requestId) << L"\",";
    json << L"\"path\":\"" << JsonEscape(path) << L"\",";
    json << L"\"message\":\"" << JsonEscape(message) << L"\"";
    json << L"}";
    return json.str();
}

void StartLocalFolderScanAsync(HWND targetWindow, std::filesystem::path folder, SmbCredentials credentials) {
    std::thread([targetWindow, folder = std::move(folder), credentials = std::move(credentials)]() {
        try {
            if (const auto connectError = ConnectSmbPathForAccess(folder, credentials)) {
                auto* payload = new std::wstring(LocalFolderScanFailedJson(folder, *connectError));
                if (!PostMessageW(targetWindow, kLocalFolderScanResultMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                    delete payload;
                }
                return;
            }
            bool truncated = false;
            const auto items = ScanMediaFilesInLocalFolder(folder, truncated);
            auto* payload = new std::wstring(LocalFolderScanCompletedJson(folder, items, truncated));
            if (!PostMessageW(targetWindow, kLocalFolderScanResultMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        } catch (...) {
            auto* payload = new std::wstring(LocalFolderScanFailedJson(folder, L"扫描本地文件夹失败"));
            if (!PostMessageW(targetWindow, kLocalFolderScanResultMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        }
    }).detach();
}

void StartSmbDirectoryListAsync(HWND targetWindow,
                                std::wstring requestId,
                                std::wstring host,
                                std::wstring path,
                                SmbCredentials credentials) {
    std::thread([
        targetWindow,
        requestId = std::move(requestId),
        host = std::move(host),
        path = std::move(path),
        credentials = std::move(credentials)]() {
        std::filesystem::path currentPath;
        try {
            std::wstring browseText = path.empty() ? host : path;
            browseText = NormalizeSmbPathText(browseText);
            const auto parts = SmbUncParts(browseText);
            if (parts.empty()) {
                currentPath = std::filesystem::path(NormalizeSmbPathText(host));
                auto* payload = new std::wstring(SmbDirectoryFailedJson(requestId, currentPath, L"请填写有效的 SMB 主机或 IP。"));
                if (!PostMessageW(targetWindow, kLocalFolderScanResultMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                    delete payload;
                }
                return;
            }

            std::wstring errorMessage;
            std::vector<SmbDirectoryEntry> directories;
            if (parts.size() <= 1) {
                const std::wstring serverRoot = SmbServerRootFromHost(browseText);
                currentPath = std::filesystem::path(serverRoot);
                directories = ListSmbShares(serverRoot, credentials, errorMessage);
            } else {
                currentPath = std::filesystem::path(browseText);
                directories = ListSmbChildDirectories(currentPath, credentials, errorMessage);
            }

            auto* payload = new std::wstring(errorMessage.empty()
                ? SmbDirectoryListedJson(requestId, currentPath, directories)
                : SmbDirectoryFailedJson(requestId, currentPath, errorMessage));
            if (!PostMessageW(targetWindow, kLocalFolderScanResultMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        } catch (const std::exception& error) {
            auto* payload = new std::wstring(SmbDirectoryFailedJson(
                requestId,
                currentPath,
                L"读取 SMB 文件夹失败：" + Utf8ToWide(error.what())));
            if (!PostMessageW(targetWindow, kLocalFolderScanResultMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        }
    }).detach();
}

void StartWebDavDirectoryListAsync(HWND targetWindow,
                                   std::wstring requestId,
                                   std::wstring url,
                                   std::wstring username,
                                   std::wstring password) {
    std::thread([
        targetWindow,
        requestId = std::move(requestId),
        url = std::move(url),
        username = std::move(username),
        password = std::move(password)]() {
        const auto normalizedUrl = NormalizeWebDavUrlText(url);
        try {
            std::wstring errorMessage;
            const auto directories = ListWebDavDirectories(normalizedUrl, username, password, errorMessage);
            auto* payload = new std::wstring(errorMessage.empty()
                ? WebDavDirectoryListedJson(requestId, normalizedUrl, directories)
                : WebDavDirectoryFailedJson(requestId, normalizedUrl, errorMessage));
            if (!PostMessageW(targetWindow, kLocalFolderScanResultMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        } catch (const std::exception& error) {
            auto* payload = new std::wstring(WebDavDirectoryFailedJson(
                requestId,
                normalizedUrl,
                L"读取 WebDAV 文件夹失败：" + Utf8ToWide(error.what())));
            if (!PostMessageW(targetWindow, kLocalFolderScanResultMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        }
    }).detach();
}

void StartWebDavScanAsync(HWND targetWindow,
                          std::wstring url,
                          std::wstring username,
                          std::wstring password) {
    std::thread([
        targetWindow,
        url = std::move(url),
        username = std::move(username),
        password = std::move(password)]() {
        const auto normalizedUrl = NormalizeWebDavUrlText(url);
        const auto folder = std::filesystem::path(normalizedUrl);
        try {
            bool truncated = false;
            std::wstring errorMessage;
            const auto items = ScanWebDavMediaFiles(normalizedUrl, username, password, truncated, errorMessage);
            auto* payload = new std::wstring(errorMessage.empty()
                ? LocalFolderScanCompletedJson(folder, items, truncated)
                : LocalFolderScanFailedJson(folder, errorMessage));
            if (!PostMessageW(targetWindow, kLocalFolderScanResultMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        } catch (const std::exception& error) {
            auto* payload = new std::wstring(LocalFolderScanFailedJson(
                folder,
                L"扫描 WebDAV 文件夹失败：" + Utf8ToWide(error.what())));
            if (!PostMessageW(targetWindow, kLocalFolderScanResultMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        }
    }).detach();
}

}  // namespace anvil::app
