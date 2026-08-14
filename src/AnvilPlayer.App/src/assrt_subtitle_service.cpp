#include "AnvilPlayer/App/assrt_subtitle_service.h"

#include "AnvilPlayer/App/string_util.h"
#include "AnvilPlayer/App/web_ui_json.h"

#include <shlobj.h>
#include <wincrypt.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <vector>

namespace anvil::app {
namespace {

constexpr wchar_t kAssrtSettingsRegistryPath[] = L"Software\\AnvilPlayer\\Subtitles";
constexpr wchar_t kAssrtTokenRegistryValue[] = L"AssrtToken";
constexpr wchar_t kAssrtApiHost[] = L"api.assrt.net";
constexpr std::size_t kMaximumJsonBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumSubtitleBytes = 16 * 1024 * 1024;

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
    ~WinHttpHandle() {
        if (handle_) {
            WinHttpCloseHandle(handle_);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                WinHttpCloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    explicit operator bool() const { return handle_ != nullptr; }
    HINTERNET get() const { return handle_; }

private:
    HINTERNET handle_ = nullptr;
};

struct HttpResponse {
    DWORD statusCode = 0;
    std::vector<std::uint8_t> body;
    std::wstring error;
};

std::wstring WindowsErrorMessage(const DWORD error) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    std::wstring result = length && message ? std::wstring(message, length) : L"error " + std::to_wstring(error);
    if (message) {
        LocalFree(message);
    }
    while (!result.empty() && std::iswspace(result.back())) {
        result.pop_back();
    }
    return result;
}

std::wstring UrlEncode(const std::wstring& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    const std::string utf8 = WideToUtf8(value);
    std::string encoded;
    encoded.reserve(utf8.size() * 3);
    for (const unsigned char ch : utf8) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[(ch >> 4) & 0x0f]);
            encoded.push_back(kHex[ch & 0x0f]);
        }
    }
    return Utf8ToWide(encoded.c_str());
}

HttpResponse HttpGet(
    const std::wstring& url,
    const std::wstring& bearerToken,
    const std::size_t maximumBytes) {
    HttpResponse response;
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        response.error = L"无法解析 ASSRT 请求地址";
        return response;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path;
    if (components.lpszUrlPath && components.dwUrlPathLength) {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo && components.dwExtraInfoLength) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    WinHttpHandle session(WinHttpOpen(
        L"AnvilPlayer/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) {
        response.error = L"无法初始化网络请求：" + WindowsErrorMessage(GetLastError());
        return response;
    }
    WinHttpSetTimeouts(session.get(), 10'000, 10'000, 20'000, 30'000);

    WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
    if (!connection) {
        response.error = L"无法连接 ASSRT：" + WindowsErrorMessage(GetLastError());
        return response;
    }

    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(
        connection.get(),
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags));
    if (!request) {
        response.error = L"无法创建 ASSRT 请求：" + WindowsErrorMessage(GetLastError());
        return response;
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (!bearerToken.empty()) {
        headers += L"Authorization: Bearer " + bearerToken + L"\r\n";
    }
    if (!WinHttpSendRequest(
            request.get(),
            headers.c_str(),
            static_cast<DWORD>(headers.size()),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        response.error = L"ASSRT 请求失败：" + WindowsErrorMessage(GetLastError());
        return response;
    }

    DWORD statusSize = sizeof(response.statusCode);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &response.statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        response.error = L"无法读取 ASSRT 响应状态";
        return response;
    }

    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            response.error = L"读取 ASSRT 响应失败：" + WindowsErrorMessage(GetLastError());
            return response;
        }
        if (available == 0) {
            break;
        }
        if (response.body.size() + available > maximumBytes) {
            response.error = L"ASSRT 响应超过允许的大小";
            response.body.clear();
            return response;
        }
        const std::size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), response.body.data() + offset, available, &read)) {
            response.error = L"读取 ASSRT 响应失败：" + WindowsErrorMessage(GetLastError());
            response.body.clear();
            return response;
        }
        response.body.resize(offset + read);
    }
    return response;
}

std::wstring BodyAsText(const HttpResponse& response) {
    if (response.body.empty()) {
        return {};
    }
    std::string text(response.body.begin(), response.body.end());
    return Utf8ToWide(text.c_str());
}

std::wstring ApiError(const HttpResponse& response, const std::wstring& json) {
    if (!response.error.empty()) {
        return response.error;
    }
    const auto message = ReadJsonString(json, L"errmsg")
                             .value_or(ReadJsonString(json, L"message").value_or(L""));
    if (response.statusCode < 200 || response.statusCode >= 300) {
        return L"ASSRT 返回 HTTP " + std::to_wstring(response.statusCode) +
               (message.empty() ? L"" : L"：" + message);
    }
    const auto status = ReadJsonNumber(json, L"status");
    if (!status.has_value()) {
        return L"ASSRT 返回了无法识别的数据";
    }
    if (static_cast<int>(*status) != 0) {
        return message.empty()
                   ? L"ASSRT 返回错误 " + std::to_wstring(static_cast<int>(*status))
                   : message;
    }
    return {};
}

std::vector<std::wstring> JsonArrayObjects(const std::wstring_view array) {
    std::vector<std::wstring> objects;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    std::size_t objectStart = std::wstring_view::npos;
    for (std::size_t index = 0; index < array.size(); ++index) {
        const wchar_t ch = array[index];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == L'\\') {
                escaped = true;
            } else if (ch == L'"') {
                inString = false;
            }
            continue;
        }
        if (ch == L'"') {
            inString = true;
            continue;
        }
        if (ch == L'{') {
            if (depth == 0) {
                objectStart = index;
            }
            ++depth;
        } else if (ch == L'}' && depth > 0) {
            --depth;
            if (depth == 0 && objectStart != std::wstring_view::npos) {
                objects.emplace_back(array.substr(objectStart, index - objectStart + 1));
                objectStart = std::wstring_view::npos;
            }
        }
    }
    return objects;
}

std::filesystem::path SubtitleCacheRoot() {
    wchar_t localAppData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(
            nullptr,
            CSIDL_LOCAL_APPDATA,
            nullptr,
            SHGFP_TYPE_CURRENT,
            localAppData))) {
        return std::filesystem::path(localAppData) / L"AnvilPlayer" / L"Subtitles" / L"ASSRT";
    }
    return std::filesystem::temp_directory_path() / L"anvil-player" / L"Subtitles" / L"ASSRT";
}

std::wstring LowerExtension(const std::wstring& name) {
    std::wstring extension = std::filesystem::path(name).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return extension;
}

std::wstring LowerText(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool IsSupportedSubtitleFile(const std::wstring& name) {
    const std::wstring extension = LowerExtension(name);
    return extension == L".srt" || extension == L".ass" ||
           extension == L".ssa" || extension == L".vtt";
}

std::wstring SafeFileName(const std::wstring& value, const int subtitleId) {
    std::wstring name = std::filesystem::path(value).filename().wstring();
    if (name.empty() || !IsSupportedSubtitleFile(name)) {
        name = L"assrt-" + std::to_wstring(subtitleId) + L".srt";
    }
    for (wchar_t& ch : name) {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    if (name.size() > 180) {
        const std::wstring extension = std::filesystem::path(name).extension().wstring();
        name.resize(std::max<std::size_t>(1, 180 - extension.size()));
        name += extension;
    }
    return name;
}

std::optional<std::pair<int, int>> EpisodeNumber(const std::wstring& value) {
    static const std::wregex pattern(LR"((?:^|[^a-z0-9])s(\d{1,2})[ ._-]*e(\d{1,3})(?:[^a-z0-9]|$))", std::regex::icase);
    std::wsmatch match;
    if (!std::regex_search(value, match, pattern) || match.size() < 3) {
        return std::nullopt;
    }
    try {
        return std::pair{std::stoi(match[1].str()), std::stoi(match[2].str())};
    } catch (...) {
        return std::nullopt;
    }
}

std::wstring SearchComparableName(const std::wstring& value) {
    std::wstring output;
    output.reserve(value.size());
    for (const wchar_t ch : std::filesystem::path(value).stem().wstring()) {
        if (std::iswalnum(ch)) {
            output.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }
    }
    return output;
}

int SubtitleFileScore(const std::wstring& subtitleName, const std::wstring& mediaName) {
    if (!IsSupportedSubtitleFile(subtitleName)) {
        return std::numeric_limits<int>::min();
    }
    int score = 0;
    const auto wantedEpisode = EpisodeNumber(mediaName);
    const auto subtitleEpisode = EpisodeNumber(subtitleName);
    if (wantedEpisode && subtitleEpisode) {
        score += *wantedEpisode == *subtitleEpisode ? 2000 : -2000;
    }
    const std::wstring mediaComparable = SearchComparableName(mediaName);
    const std::wstring subtitleComparable = SearchComparableName(subtitleName);
    if (!mediaComparable.empty() && !subtitleComparable.empty()) {
        if (subtitleComparable.find(mediaComparable) != std::wstring::npos ||
            mediaComparable.find(subtitleComparable) != std::wstring::npos) {
            score += 400;
        }
        const std::size_t common = std::min(mediaComparable.size(), subtitleComparable.size());
        std::size_t prefix = 0;
        while (prefix < common && mediaComparable[prefix] == subtitleComparable[prefix]) {
            ++prefix;
        }
        score += static_cast<int>(std::min<std::size_t>(prefix, 100));
    }
    const std::wstring lower = LowerText(subtitleName);
    if (lower.find(L"chs") != std::wstring::npos || lower.find(L"zh-cn") != std::wstring::npos ||
        lower.find(L"简") != std::wstring::npos) {
        score += 30;
    } else if (lower.find(L"cht") != std::wstring::npos || lower.find(L"zh-tw") != std::wstring::npos ||
               lower.find(L"繁") != std::wstring::npos) {
        score += 20;
    }
    if (LowerExtension(subtitleName) == L".ass") {
        score += 5;
    }
    return score;
}

struct DownloadFile {
    std::wstring name;
    std::wstring url;
    int score = std::numeric_limits<int>::min();
};

std::optional<DownloadFile> SelectDownloadFile(
    const std::wstring& detail,
    const std::wstring& mediaName) {
    std::vector<DownloadFile> files;
    if (const auto fileList = ReadJsonObject(detail, L"filelist")) {
        for (const auto& object : JsonArrayObjects(*fileList)) {
            const std::wstring name = ReadJsonString(object, L"f").value_or(L"");
            const std::wstring url = ReadJsonString(object, L"url").value_or(L"");
            if (name.empty() || url.empty() || !IsSupportedSubtitleFile(name)) {
                continue;
            }
            files.push_back({name, url, SubtitleFileScore(name, mediaName)});
        }
    }
    const std::wstring outerName = ReadJsonString(detail, L"filename").value_or(L"");
    const std::wstring outerUrl = ReadJsonString(detail, L"url").value_or(L"");
    if (!outerName.empty() && !outerUrl.empty() && IsSupportedSubtitleFile(outerName)) {
        files.push_back({outerName, outerUrl, SubtitleFileScore(outerName, mediaName)});
    }
    if (files.empty()) {
        return std::nullopt;
    }
    return *std::max_element(files.begin(), files.end(), [](const DownloadFile& left, const DownloadFile& right) {
        return left.score < right.score;
    });
}

bool LooksLikeSubtitlePayload(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return false;
    }
    const std::size_t sampleSize = std::min<std::size_t>(bytes.size(), 512);
    std::string sample(bytes.begin(), bytes.begin() + sampleSize);
    std::transform(sample.begin(), sample.end(), sample.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    const std::size_t first = sample.find_first_not_of(" \t\r\n\xef\xbb\xbf");
    if (first != std::string::npos &&
        (sample.compare(first, 5, "<html") == 0 ||
         sample.compare(first, 9, "<!doctype") == 0 ||
         sample.compare(first, 1, "{") == 0)) {
        return false;
    }
    return true;
}

}  // namespace

bool SaveAssrtToken(const std::wstring& token) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kAssrtSettingsRegistryPath,
            0,
            nullptr,
            0,
            KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }

    bool saved = false;
    if (token.empty()) {
        const LONG result = RegDeleteValueW(key, kAssrtTokenRegistryValue);
        saved = result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    } else {
        DATA_BLOB input{};
        input.cbData = static_cast<DWORD>((token.size() + 1) * sizeof(wchar_t));
        input.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(token.c_str()));
        DATA_BLOB encrypted{};
        if (CryptProtectData(
                &input,
                L"Anvil Player ASSRT token",
                nullptr,
                nullptr,
                nullptr,
                CRYPTPROTECT_UI_FORBIDDEN,
                &encrypted)) {
            saved = RegSetValueExW(
                        key,
                        kAssrtTokenRegistryValue,
                        0,
                        REG_BINARY,
                        encrypted.pbData,
                        encrypted.cbData) == ERROR_SUCCESS;
            LocalFree(encrypted.pbData);
        }
    }
    RegCloseKey(key);
    return saved;
}

std::wstring LoadAssrtToken() {
    DWORD size = 0;
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            kAssrtSettingsRegistryPath,
            kAssrtTokenRegistryValue,
            RRF_RT_REG_BINARY,
            nullptr,
            nullptr,
            &size) != ERROR_SUCCESS ||
        size == 0) {
        return {};
    }
    std::vector<BYTE> encrypted(size);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            kAssrtSettingsRegistryPath,
            kAssrtTokenRegistryValue,
            RRF_RT_REG_BINARY,
            nullptr,
            encrypted.data(),
            &size) != ERROR_SUCCESS) {
        return {};
    }
    DATA_BLOB input{size, encrypted.data()};
    DATA_BLOB decrypted{};
    if (!CryptUnprotectData(
            &input,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &decrypted)) {
        return {};
    }
    std::wstring token;
    if (decrypted.pbData && decrypted.cbData >= sizeof(wchar_t)) {
        const auto* value = reinterpret_cast<const wchar_t*>(decrypted.pbData);
        const std::size_t count = decrypted.cbData / sizeof(wchar_t);
        const auto* terminator = std::find(value, value + count, L'\0');
        if (terminator != value + count) {
            token.assign(value, terminator);
        }
    }
    if (decrypted.pbData) {
        SecureZeroMemory(decrypted.pbData, decrypted.cbData);
        LocalFree(decrypted.pbData);
    }
    return token;
}

AssrtSubtitleSearchResult SearchAssrtSubtitles(
    const std::wstring& token,
    const std::wstring& query) {
    AssrtSubtitleSearchResult result;
    if (token.empty()) {
        result.error = L"请先配置 ASSRT Token";
        return result;
    }
    if (query.size() < 3) {
        result.error = L"搜索关键词至少需要 3 个字符";
        return result;
    }

    const std::wstring url = std::wstring(L"https://") + kAssrtApiHost +
                             L"/v1/sub/search?q=" + UrlEncode(query) +
                             L"&cnt=15&pos=0&no_muxer=1";
    const HttpResponse response = HttpGet(url, token, kMaximumJsonBytes);
    const std::wstring json = BodyAsText(response);
    result.error = ApiError(response, json);
    if (!result.error.empty()) {
        return result;
    }

    const auto sub = ReadJsonObject(json, L"sub");
    const auto subtitles = sub ? ReadJsonObject(*sub, L"subs") : std::nullopt;
    if (!subtitles) {
        return result;
    }
    for (const auto& object : JsonArrayObjects(*subtitles)) {
        const auto id = ReadJsonNumber(object, L"id");
        if (!id || *id <= 0) {
            continue;
        }
        AssrtSubtitleCandidate item;
        item.id = static_cast<int>(*id);
        item.name = ReadJsonString(object, L"native_name").value_or(L"ASSRT " + std::to_wstring(item.id));
        item.videoName = ReadJsonString(object, L"videoname").value_or(L"");
        item.format = ReadJsonString(object, L"subtype").value_or(L"");
        item.releaseSite = ReadJsonString(object, L"release_site").value_or(L"");
        item.uploadTime = ReadJsonString(object, L"upload_time").value_or(L"");
        item.score = ReadJsonNumber(object, L"vote_score").value_or(0.0);
        if (const auto language = ReadJsonObject(object, L"lang")) {
            item.language = ReadJsonString(*language, L"desc").value_or(L"");
        }
        result.items.push_back(std::move(item));
    }
    return result;
}

AssrtSubtitleDownloadResult DownloadAssrtSubtitle(
    const std::wstring& token,
    const int subtitleId,
    const std::wstring& mediaName) {
    AssrtSubtitleDownloadResult result;
    if (token.empty()) {
        result.error = L"请先配置 ASSRT Token";
        return result;
    }
    if (subtitleId <= 0) {
        result.error = L"ASSRT 字幕 ID 无效";
        return result;
    }

    const std::wstring detailUrl = std::wstring(L"https://") + kAssrtApiHost +
                                   L"/v1/sub/detail?id=" + std::to_wstring(subtitleId);
    const HttpResponse detailResponse = HttpGet(detailUrl, token, kMaximumJsonBytes);
    const std::wstring json = BodyAsText(detailResponse);
    result.error = ApiError(detailResponse, json);
    if (!result.error.empty()) {
        return result;
    }

    const auto sub = ReadJsonObject(json, L"sub");
    const auto subtitles = sub ? ReadJsonObject(*sub, L"subs") : std::nullopt;
    const auto details = subtitles ? JsonArrayObjects(*subtitles) : std::vector<std::wstring>{};
    if (details.empty()) {
        result.error = L"ASSRT 没有返回字幕下载信息";
        return result;
    }
    const auto selected = SelectDownloadFile(details.front(), mediaName);
    if (!selected) {
        result.error = L"该字幕包没有可直接使用的 SRT、ASS、SSA 或 VTT 文件";
        return result;
    }

    const HttpResponse fileResponse = HttpGet(selected->url, L"", kMaximumSubtitleBytes);
    if (!fileResponse.error.empty()) {
        result.error = fileResponse.error;
        return result;
    }
    if (fileResponse.statusCode < 200 || fileResponse.statusCode >= 300) {
        result.error = L"字幕下载返回 HTTP " + std::to_wstring(fileResponse.statusCode);
        return result;
    }
    if (!LooksLikeSubtitlePayload(fileResponse.body)) {
        result.error = L"下载结果不是可识别的字幕文件";
        return result;
    }

    result.fileName = SafeFileName(selected->name, subtitleId);
    const std::filesystem::path folder = SubtitleCacheRoot() / std::to_wstring(subtitleId);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        result.error = L"无法创建字幕缓存目录";
        return result;
    }
    result.path = folder / result.fileName;
    std::ofstream output(result.path, std::ios::binary | std::ios::trunc);
    if (!output) {
        result.error = L"无法写入字幕缓存";
        result.path.clear();
        return result;
    }
    output.write(
        reinterpret_cast<const char*>(fileResponse.body.data()),
        static_cast<std::streamsize>(fileResponse.body.size()));
    if (!output) {
        result.error = L"字幕缓存写入失败";
        result.path.clear();
    }
    return result;
}

}  // namespace anvil::app
