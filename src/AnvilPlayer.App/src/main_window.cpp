#include "AnvilPlayer/App/main_window.h"
#include "AnvilPlayer/App/string_util.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <lm.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winnetwk.h>
#include <winhttp.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace anvil::app {

using anvil::playback::CapabilityReport;
using anvil::playback::FormatTimecode;
using anvil::playback::LogLevel;
using anvil::playback::PlaybackSessionSnapshot;
using anvil::playback::PlaybackState;
using anvil::playback::ToDisplayString;

namespace {

constexpr auto kSidebarAnimationDuration = std::chrono::milliseconds{460};
constexpr auto kProgressHoverAnimationDuration = std::chrono::milliseconds{180};
constexpr auto kFullscreenTransportAnimationDuration = std::chrono::milliseconds{260};
constexpr auto kSubtitleMenuAnimationDuration = std::chrono::milliseconds{240};
constexpr auto kWebUiSubtitleMenuOpenAnimationDuration = std::chrono::milliseconds{560};
constexpr auto kWebUiSubtitleMenuCloseAnimationDuration = std::chrono::milliseconds{260};
constexpr auto kButtonHoverAnimationDuration = std::chrono::milliseconds{180};
constexpr auto kButtonPressAnimationDuration = std::chrono::milliseconds{220};
constexpr auto kVolumeHoverAnimationDuration = std::chrono::milliseconds{180};
constexpr auto kWebUiProgressUpdateInterval = std::chrono::milliseconds{100};
constexpr auto kNativeBufferingNoProgressTimeout = std::chrono::seconds{18};
constexpr auto kNativeBufferingHardTimeout = std::chrono::seconds{45};
constexpr auto kFullscreenTransportHideDelay = std::chrono::seconds{5};
constexpr auto kScrollbarVisibleDuration = std::chrono::milliseconds{650};
constexpr auto kScrollbarFadeDuration = std::chrono::milliseconds{300};
constexpr int kFullscreenTransportActivationHeight = 110;
constexpr std::size_t kMaxRecentMedia = 12;
constexpr std::size_t kMaxLocalFolderScanItems = 2000;
constexpr int kMaxLocalFolderScanDepth = 8;

struct LocalFolderScanItem {
    std::filesystem::path path;
    std::wstring playbackPath;
    std::int64_t modifiedAtMs = 0;
    std::uintmax_t sizeBytes = 0;
};

struct SmbCredentials {
    std::wstring username;
    std::wstring password;
};

struct SmbDirectoryEntry {
    std::wstring name;
    std::filesystem::path path;
};

struct WebDavEntry {
    std::wstring href;
    bool isDirectory = false;
    std::int64_t modifiedAtMs = 0;
    std::uintmax_t sizeBytes = 0;
};

bool HasBuiltWebUiRoot(const std::filesystem::path& root) {
    if (root.empty()) {
        return false;
    }

    std::error_code error;
    return std::filesystem::exists(root / L"index.html", error) &&
           std::filesystem::exists(root / L"assets", error);
}

std::filesystem::path FindBuiltWebUiRootNear(std::filesystem::path base) {
    if (base.empty()) {
        return {};
    }

    std::error_code error;
    base = std::filesystem::absolute(base, error).lexically_normal();
    if (error) {
        return {};
    }

    for (auto current = base; !current.empty();) {
        const std::filesystem::path candidates[] = {
            current / L"webui",
            current / L"webui" / L"dist",
            current / L"src" / L"AnvilPlayer.App" / L"webui" / L"dist",
            current / L"AnvilPlayer.App" / L"webui" / L"dist",
            current / L"dist",
        };

        for (const auto& candidate : candidates) {
            if (HasBuiltWebUiRoot(candidate)) {
                return candidate;
            }
        }

        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

enum class MotionCurve {
    Fluid,
    Spring,
    Press,
    Fade,
    EaseIn,
};

double Cubic(const double a,
             const double b,
             const double c,
             const double d,
             const double t) {
    const double inverse = 1.0 - t;
    return inverse * inverse * inverse * a +
           3.0 * inverse * inverse * t * b +
           3.0 * inverse * t * t * c +
           t * t * t * d;
}

double CubicDerivative(const double p1, const double p2, const double t) {
    const double inverse = 1.0 - t;
    return 3.0 * inverse * inverse * p1 +
           6.0 * inverse * t * (p2 - p1) +
           3.0 * t * t * (1.0 - p2);
}

double CubicBezierEase(const double value,
                       const double x1,
                       const double y1,
                       const double x2,
                       const double y2) {
    const double x = std::clamp(value, 0.0, 1.0);
    if (x <= 0.0 || x >= 1.0) {
        return x;
    }

    double t = x;
    for (int i = 0; i < 5; ++i) {
        const double currentX = Cubic(0.0, x1, x2, 1.0, t);
        const double derivative = CubicDerivative(x1, x2, t);
        if (std::abs(derivative) < 0.000001) {
            break;
        }
        const double next = t - (currentX - x) / derivative;
        if (next < 0.0 || next > 1.0) {
            break;
        }
        t = next;
    }

    double lower = 0.0;
    double upper = 1.0;
    for (int i = 0; i < 8; ++i) {
        const double currentX = Cubic(0.0, x1, x2, 1.0, t);
        if (std::abs(currentX - x) < 0.00001) {
            break;
        }
        if (currentX < x) {
            lower = t;
        } else {
            upper = t;
        }
        t = (lower + upper) * 0.5;
    }

    return Cubic(0.0, y1, y2, 1.0, t);
}

double Ease(const double value, const MotionCurve curve) {
    switch (curve) {
    case MotionCurve::Fluid:
        return CubicBezierEase(value, 0.16, 1.0, 0.30, 1.0);
    case MotionCurve::Spring:
        return CubicBezierEase(value, 0.18, 1.35, 0.28, 1.0);
    case MotionCurve::Press:
        return CubicBezierEase(value, 0.18, 1.20, 0.28, 1.0);
    case MotionCurve::Fade:
        return CubicBezierEase(value, 0.22, 1.0, 0.36, 1.0);
    case MotionCurve::EaseIn:
        return CubicBezierEase(value, 0.32, 0.0, 0.67, 0.0);
    }
    return value;
}

double AnimatedValue(const double from,
                     const double to,
                     const std::chrono::steady_clock::time_point startedAt,
                     const std::chrono::milliseconds duration,
                     const std::chrono::steady_clock::time_point now,
                     const MotionCurve curve,
                     bool& complete) {
    if (duration.count() <= 0 || startedAt.time_since_epoch().count() == 0) {
        complete = true;
        return to;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startedAt);
    const double t = std::clamp(static_cast<double>(elapsed.count()) / static_cast<double>(duration.count()), 0.0, 1.0);
    complete = t >= 1.0;
    return from + (to - from) * Ease(t, curve);
}

bool UpdateMotionValue(UiMotionValue& value,
                       const std::chrono::milliseconds duration,
                       const std::chrono::steady_clock::time_point now,
                       const MotionCurve curve,
                       bool& complete) {
    const double previous = value.amount;
    value.amount = AnimatedValue(value.startAmount,
                                 value.target,
                                 value.startedAt,
                                 duration,
                                 now,
                                 curve,
                                 complete);
    if (complete) {
        value.amount = value.target;
    }
    return std::abs(value.amount - previous) > 0.0001;
}

bool HasArea(const RECT& rect) {
    return RectWidth(rect) > 0 && RectHeight(rect) > 0;
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool RectsIntersect(const RECT& a, const RECT& b) {
    RECT intersection{};
    return HasArea(a) && HasArea(b) && IntersectRect(&intersection, &a, &b) != FALSE;
}

RECT InflateRectCopy(RECT rect, const int dx, const int dy) {
    InflateRect(&rect, dx, dy);
    return rect;
}

void InvalidateIfVisible(HWND hwnd, RECT rect, const int padding) {
    if (!hwnd || !HasArea(rect)) {
        return;
    }
    rect = InflateRectCopy(rect, padding, padding);
    InvalidateRect(hwnd, &rect, FALSE);
}

RECT SidebarEdgeRect(const RECT& before, const RECT& after) {
    if (!HasArea(before) || !HasArea(after)) {
        return HasArea(before) ? before : after;
    }
    const int left = std::min(before.right, after.right);
    const int right = std::max(before.right, after.right);
    return MakeRect(left, std::min(before.top, after.top), right, std::max(before.bottom, after.bottom));
}

std::wstring FormatMillisecondsFromMicroseconds(const uint64_t microseconds) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(2) << (static_cast<double>(microseconds) / 1000.0);
    return stream.str();
}

std::wstring FormatFixed2(const double value) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

std::wstring FormatAverageMilliseconds(const uint64_t totalMicroseconds, const uint64_t count) {
    if (count == 0) {
        return L"0.00";
    }
    return FormatMillisecondsFromMicroseconds(totalMicroseconds / count);
}

bool WantsDolbyVisionHdrOutput(const anvil::playback::VideoSettings& settings,
                               const anvil::playback::DisplayCapabilities& display) {
    (void)display;
    return settings.dolbyVisionHdrOutput;
}

bool ExperimentalDoviTrimEnabled() {
    wchar_t value[16]{};
    constexpr DWORD kValueCount = static_cast<DWORD>(sizeof(value) / sizeof(value[0]));
    const DWORD length = GetEnvironmentVariableW(L"ANVIL_EXPERIMENTAL_DOVI_TRIM", value, kValueCount);
    if (length == 0 || length >= kValueCount) {
        return false;
    }
    return value[0] == L'1' || value[0] == L't' || value[0] == L'T' ||
           value[0] == L'y' || value[0] == L'Y' || value[0] == L'o' ||
           value[0] == L'O';
}

bool WantsCmv4Approx(const anvil::playback::VideoSettings& settings) {
    return settings.dolbyVisionCmv4Approx || ExperimentalDoviTrimEnabled();
}

std::filesystem::path AppDataStorageFolder() {
    wchar_t localAppData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData))) {
        return std::filesystem::path(localAppData) / L"AnvilPlayer";
    }
    return std::filesystem::temp_directory_path() / L"anvil-player";
}

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

struct WebDavUrlParts {
    std::wstring scheme;
    std::wstring host;
    std::wstring pathAndQuery;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
    bool secure = false;
};

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

std::wstring NormalizeWebDavUrlText(std::wstring url) {
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

        std::wostringstream rebuilt;
        rebuilt << parts->scheme << L"://" << parts->host;
        const bool defaultPort = (parts->secure && parts->port == INTERNET_DEFAULT_HTTPS_PORT) ||
                                 (!parts->secure && parts->port == INTERNET_DEFAULT_HTTP_PORT);
        if (!defaultPort) {
            rebuilt << L":" << parts->port;
        }
        rebuilt << normalizedPath;
        url = rebuilt.str();
    }
    if (!url.empty() && url.back() != L'/') {
        url.push_back(L'/');
    }
    return url;
}

std::wstring AbsoluteWebDavHref(const std::wstring& baseUrl, std::wstring href) {
    href = XmlDecode(std::move(href));
    if (StartsWithInsensitive(href, L"http://") || StartsWithInsensitive(href, L"https://")) {
        return href;
    }
    const auto origin = WebDavOrigin(baseUrl);
    if (href.rfind(L"/", 0) == 0) {
        return origin + href;
    }
    return NormalizeWebDavUrlText(baseUrl) + href;
}

std::wstring WebDavHrefLastSegment(std::wstring href) {
    href = XmlDecode(std::move(href));
    const auto query = href.find_first_of(L"?#");
    if (query != std::wstring::npos) {
        href.erase(query);
    }
    while (!href.empty() && (href.back() == L'/' || href.back() == L'\\')) {
        href.pop_back();
    }
    const auto slash = href.find_last_of(L"/\\");
    const auto segment = slash == std::wstring::npos ? href : href.substr(slash + 1);
    return segment;
}

std::wstring WebDavChildUrlFromHref(const std::wstring& baseUrl, const std::wstring& href, const bool isDirectory) {
    const auto segment = WebDavHrefLastSegment(href);
    if (segment.empty()) {
        return {};
    }
    std::wstring childUrl = NormalizeWebDavUrlText(baseUrl) + UrlEncodePathSegmentPreservingEscapes(segment);
    if (isDirectory) {
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

bool HdrFormatLooksHdr(const std::wstring& value) {
    return ContainsInsensitive(value, L"HDR") ||
           ContainsInsensitive(value, L"HLG") ||
           ContainsInsensitive(value, L"PQ") ||
           ContainsInsensitive(value, L"Dolby Vision");
}

bool MediaIsDolbyVision(const std::optional<anvil::playback::MediaDescriptor>& media) {
    return media.has_value() && media->hasVideo && media->dolbyVisionDetected;
}

bool MediaHasDolbyVisionEnhancementStream(const std::optional<anvil::playback::MediaDescriptor>& media) {
    if (!MediaIsDolbyVision(media)) {
        return false;
    }
    for (const auto& stream : media->streams) {
        if (stream.kind == L"Video" && ContainsInsensitive(stream.details, L"EL-only")) {
            return true;
        }
    }
    return false;
}

bool MediaHasHdrSignal(const std::optional<anvil::playback::MediaDescriptor>& media) {
    if (!media.has_value() || !media->hasVideo) {
        return false;
    }
    return media->dolbyVisionDetected ||
           media->videoColor.IsHdr() ||
           HdrFormatLooksHdr(media->hdrFormat);
}

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

bool MessageContains(const std::wstring_view message, const wchar_t* needle) {
    return message.find(needle) != std::wstring_view::npos;
}

std::optional<double> ReadJsonNumber(const std::wstring_view message, const wchar_t* field) {
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

std::optional<std::wstring> ReadJsonString(const std::wstring_view message, const wchar_t* field) {
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

std::wstring InspectorTabName(const InspectorTab tab) {
    switch (tab) {
    case InspectorTab::Recent: return L"recent";
    case InspectorTab::Folder: return L"folder";
    case InspectorTab::Media: return L"media";
    case InspectorTab::System: return L"system";
    case InspectorTab::Log: return L"log";
    case InspectorTab::Settings: return L"settings";
    }
    return L"recent";
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

void StartLocalFolderScanAsync(HWND targetWindow, std::filesystem::path folder, SmbCredentials credentials = {}) {
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

std::wstring RecentLogLinesJson(const std::shared_ptr<anvil::playback::InMemoryLogSink>& sink, const std::size_t maxCount) {
    std::wostringstream json;
    json << L"[";
    if (sink) {
        const auto entries = sink->Entries();
        const std::size_t start = entries.size() > maxCount ? entries.size() - maxCount : 0;
        for (std::size_t index = start; index < entries.size(); ++index) {
            if (index > start) {
                json << L",";
            }
            json << L"\"" << JsonEscape(ToDisplayString(entries[index].level) + L" " +
                                        entries[index].category + L": " +
                                        entries[index].message)
                 << L"\"";
        }
    }
    json << L"]";
    return json.str();
}

std::wstring DisplayLanguageName(const std::wstring& language) {
    if (language.empty() || language == L"-") {
        return {};
    }

    const std::wstring normalized = LowerCopy(language);
    if (normalized == L"chi" || normalized == L"zho" || normalized == L"zh" ||
        normalized == L"chs" || normalized == L"cht" || normalized == L"cmn" ||
        normalized == L"cn" || normalized.rfind(L"zh-", 0) == 0 ||
        normalized.find(L"chinese") != std::wstring::npos) {
        if (normalized == L"cht" ||
            normalized.find(L"trad") != std::wstring::npos ||
            normalized.find(L"hant") != std::wstring::npos ||
            normalized.find(L"tw") != std::wstring::npos) {
            return L"Chinese Traditional";
        }
        if (normalized == L"chs" ||
            normalized.find(L"simp") != std::wstring::npos ||
            normalized.find(L"hans") != std::wstring::npos ||
            normalized.find(L"cn") != std::wstring::npos) {
            return L"Chinese Simplified";
        }
        return L"Chinese";
    }
    if (normalized == L"eng" || normalized == L"en") {
        return L"English";
    }
    if (normalized == L"jpn" || normalized == L"ja") {
        return L"Japanese";
    }
    if (normalized == L"kor" || normalized == L"ko") {
        return L"Korean";
    }
    return language;
}

std::wstring UpperCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towupper(ch));
    });
    return value;
}

std::vector<int> TrackMenuItems(const std::optional<anvil::playback::MediaDescriptor>& media,
                                const std::wstring& kind,
                                const int offTrack,
                                const int autoTrack) {
    std::vector<int> tracks;
    tracks.push_back(offTrack);
    tracks.push_back(autoTrack);
    if (media.has_value()) {
        for (const auto& stream : media->streams) {
            if (stream.kind == kind) {
                tracks.push_back(stream.index);
            }
        }
    }
    return tracks;
}

const anvil::playback::MediaStreamSummary* FindStream(const std::optional<anvil::playback::MediaDescriptor>& media,
                                                      const std::wstring& kind,
                                                      const int index) {
    if (!media.has_value() || index < 0) {
        return nullptr;
    }
    for (const auto& stream : media->streams) {
        if (stream.kind == kind && stream.index == index) {
            return &stream;
        }
    }
    return nullptr;
}

std::wstring TrackLabel(const std::optional<anvil::playback::MediaDescriptor>& media,
                        const std::wstring& kind,
                        const int index,
                        const int offTrack,
                        const int autoTrack) {
    if (index == offTrack) {
        return L"Off";
    }
    if (index == autoTrack) {
        return L"Auto";
    }

    std::wstring label = L"Stream " + std::to_wstring(index);
    if (const auto* stream = FindStream(media, kind, index)) {
        const std::wstring language = DisplayLanguageName(stream->language);
        if (!language.empty()) {
            label = language;
        }
    }
    return label;
}

std::wstring TrackDetail(const std::optional<anvil::playback::MediaDescriptor>& media,
                         const std::wstring& kind,
                         const int index) {
    if (const auto* stream = FindStream(media, kind, index)) {
        const std::wstring codec = UpperCopy(stream->codec);
        if (!codec.empty() && !stream->details.empty()) {
            return codec + L" / " + stream->details;
        }
        return !codec.empty() ? codec : stream->details;
    }
    return {};
}

std::wstring TrackItemsJson(const std::optional<anvil::playback::MediaDescriptor>& media,
                            const std::wstring& kind,
                            const int offTrack,
                            const int autoTrack) {
    const auto tracks = TrackMenuItems(media, kind, offTrack, autoTrack);
    std::wostringstream json;
    json << L"[";
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (index > 0) {
            json << L",";
        }
        const int track = tracks[index];
        json << L"{\"index\":" << track
             << L",\"label\":\"" << JsonEscape(TrackLabel(media, kind, track, offTrack, autoTrack)) << L"\""
             << L",\"detail\":\"" << JsonEscape(TrackDetail(media, kind, track)) << L"\"}";
    }
    json << L"]";
    return json.str();
}

std::wstring HdrToneCurveJson(const anvil::playback::VideoSettings& settings) {
    std::wostringstream json;
    json << L"[";
    for (std::size_t index = 0; index < settings.hdrToneCurve.size(); ++index) {
        if (index > 0) {
            json << L",";
        }
        const double inputNits = index < anvil::playback::kDefaultHdrToneCurve.size()
                                     ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                     : settings.hdrToneCurve[index].inputNits;
        json << L"{\"index\":" << index
             << L",\"inputNits\":" << std::fixed << std::setprecision(0) << inputNits
             << L",\"outputNits\":" << std::fixed << std::setprecision(0) << settings.hdrToneCurve[index].outputNits
             << L"}";
    }
    json << L"]";
    return json.str();
}

}  // namespace

void MainWindow::ConfigureLogging(const LogLevel minimumLevel) {
    auto sink = controller_.LogSink();
    sink->SetMinimumLevel(minimumLevel);
    sink->SetFilePath(DefaultLogPath());
#if defined(_DEBUG)
    sink->EnableDebuggerOutput(true);
#else
    sink->EnableDebuggerOutput(false);
#endif
    nativeVideoDecoder_.emplace(sink);
    d3dRenderer_.emplace(sink);
    d3dRenderer_->SetDiagnosticsEnabled(minimumLevel == LogLevel::Debug);
    audioPlayer_.SetLogSink(sink);
    LogApp(LogLevel::Info, L"logging configured level=" + ToDisplayString(minimumLevel) + L" file=" + sink->FilePath().wstring());
    LogApp(LogLevel::Debug, L"debug logging enabled");
}

void MainWindow::SetBackend(const PlaybackBackend backend) {
    backend_ = backend;
    std::wstring name;
    switch (backend_) {
    case PlaybackBackend::NativeFfmpegD3D11: name = L"native_ffmpeg_d3d11"; break;
    case PlaybackBackend::EmbeddedFfplay: name = L"embedded_ffplay"; break;
    case PlaybackBackend::RawFrameBridge: name = L"raw_frame_bridge"; break;
    }
    LogApp(LogLevel::Info, L"playback backend=" + name);
}

void MainWindow::SetInitialVideoTrackSelection(const int selectedTrackIndex) {
    auto settings = controller_.Settings();
    settings.video.selectedTrackIndex = selectedTrackIndex;
    controller_.ApplySettings(settings);

    std::wstring label = L"auto";
    if (selectedTrackIndex == anvil::playback::kVideoTrackDolbyVisionEnhancement) {
        label = L"dolby_vision_enhancement";
    } else if (selectedTrackIndex >= 0) {
        label = L"stream=" + std::to_wstring(selectedTrackIndex);
    }
    LogApp(LogLevel::Info, L"initial video track=" + label);
}

void MainWindow::SetWebUiEnabled(const bool enabled) {
    webUiRequested_ = enabled;
}

bool MainWindow::Create(HINSTANCE instance) {
    instance_ = instance;
    dpi_ = GetDpiForSystem();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = &MainWindow::WindowProc;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    constexpr DWORD windowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    constexpr DWORD windowExStyle = 0;
    const auto adjustedWindowRect = [this, windowStyle, windowExStyle](const int clientWidth, const int clientHeight) {
        RECT rect = MakeRect(0, 0, clientWidth, clientHeight);
        if (!AdjustWindowRectExForDpi(&rect, windowStyle, FALSE, windowExStyle, dpi_)) {
            AdjustWindowRectEx(&rect, windowStyle, FALSE, windowExStyle);
        }
        return rect;
    };

    const int targetClientWidth = Scale(1260);
    const int targetClientHeight = Scale(760);
    RECT initialRect = adjustedWindowRect(targetClientWidth, targetClientHeight);
    RECT workArea{};
    int initialX = CW_USEDEFAULT;
    int initialY = CW_USEDEFAULT;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) &&
        RectWidth(workArea) > 0 &&
        RectHeight(workArea) > 0) {
        const int maxWindowWidth = std::max(Scale(720), RectWidth(workArea) * 92 / 100);
        const int maxWindowHeight = std::max(Scale(500), RectHeight(workArea) * 92 / 100);
        const int windowWidth = RectWidth(initialRect);
        const int windowHeight = RectHeight(initialRect);
        if (windowWidth > maxWindowWidth || windowHeight > maxWindowHeight) {
            const double fit = std::min(static_cast<double>(maxWindowWidth) / std::max(1, windowWidth),
                                        static_cast<double>(maxWindowHeight) / std::max(1, windowHeight));
            const int fittedClientWidth = std::max(Scale(720), static_cast<int>(std::round(targetClientWidth * fit)));
            const int fittedClientHeight = std::max(Scale(500), static_cast<int>(std::round(targetClientHeight * fit)));
            initialRect = adjustedWindowRect(fittedClientWidth, fittedClientHeight);
        }
        initialX = workArea.left + std::max(0, RectWidth(workArea) - RectWidth(initialRect)) / 2;
        initialY = workArea.top + std::max(0, RectHeight(workArea) - RectHeight(initialRect)) / 2;
    }

    hwnd_ = CreateWindowExW(
        windowExStyle,
        kWindowClassName,
        L"Anvil Player",
        windowStyle,
        initialX,
        initialY,
        RectWidth(initialRect),
        RectHeight(initialRect),
        nullptr,
        nullptr,
        instance_,
        this);

    if (!hwnd_) {
        return false;
    }

    LoadRecentMedia();
    TryCreateWebUi();
    return true;
}

void MainWindow::Show(const int commandShow) const {
    ShowWindow(hwnd_, commandShow);
    UpdateWindow(hwnd_);
}

std::filesystem::path MainWindow::WebUiRoot() const {
    wchar_t modulePath[MAX_PATH]{};
    constexpr DWORD modulePathCount = static_cast<DWORD>(sizeof(modulePath) / sizeof(modulePath[0]));
    const DWORD length = GetModuleFileNameW(instance_, modulePath, modulePathCount);
    if (length > 0 && length < modulePathCount) {
        const auto root = FindBuiltWebUiRootNear(std::filesystem::path(modulePath).parent_path());
        if (!root.empty()) {
            return root;
        }
    }

    std::error_code error;
    const auto currentRoot = FindBuiltWebUiRootNear(std::filesystem::current_path(error));
    if (!error && !currentRoot.empty()) {
        return currentRoot;
    }

    if (length > 0 && length < modulePathCount) {
        return std::filesystem::path(modulePath).parent_path() / L"webui";
    }
    return std::filesystem::current_path(error) / L"webui";
}

bool MainWindow::TryCreateWebUi() {
    if (!webUiRequested_ || webUiActive_ || !hwnd_) {
        return false;
    }

    auto host = std::make_unique<WebUiHost>();
    const auto root = WebUiRoot();
    const bool started = host->Create(hwnd_, root, [this](const std::wstring& message) {
        HandleWebUiMessage(message);
    });
    if (!started) {
        LogApp(LogLevel::Warning, L"web ui unavailable root=" + root.wstring() + L" hr=0x" + HexHr(host->LastCreateResult()));
        return false;
    }

    webUiHost_ = std::move(host);
    webUiActive_ = true;
    LogApp(LogLevel::Info,
           L"web ui enabled root=" + root.wstring() +
               (webUiHost_->UsedDefaultOptionsFallback() ? L" options=default_after_retry" : L" options=custom"));
    return true;
}

std::wstring MainWindow::BuildWebUiStateJson() const {
    const auto snapshot = controller_.Snapshot();
    const auto settings = controller_.Settings();

    std::wstring playbackState = ToDisplayString(snapshot.state);
    if (!snapshot.media.has_value()) {
        playbackState = L"Empty";
    }

    const std::wstring mediaName = snapshot.media.has_value() ? snapshot.media->displayName : L"No media loaded";
    long long positionMs = std::max<long long>(0, snapshot.position.count());
    const long long durationMs = snapshot.media.has_value() ? std::max<long long>(0, snapshot.media->duration.count()) : 0;
    long long bufferedEndMs = positionMs;
    bool buffering = false;
    uint64_t networkBytesPerSecond = 0;
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        nativeVideoDecoder_ &&
        snapshot.media.has_value() &&
        snapshot.media->duration.count() > 0) {
        const auto stats = nativeVideoDecoder_->Stats();
        buffering = stats.buffering;
        networkBytesPerSecond = stats.networkBytesPerSecond;
        if (buffering) {
            positionMs = std::clamp<long long>(stats.clockPosition.count(), 0, durationMs);
            bufferedEndMs = positionMs;
        }
        if ((stats.queueDepth > 0 || stats.packetQueueDepth > 0) && stats.bufferedEnd.count() > positionMs) {
            bufferedEndMs = std::clamp<long long>(stats.bufferedEnd.count(), positionMs, durationMs);
        }
    }
    const bool hdrAvailable = CurrentMediaHasHdrControls();
    const bool cmv4Available = CurrentMediaHasCmv4Control();
    const bool cmv4Enabled = CurrentCmv4ControlEnabled(settings) && settings.video.dolbyVisionCmv4Approx;
    const bool hdrToneCurveAvailable = HdrToneCurveAvailable(settings);
    const bool hasVideo = snapshot.media.has_value() && snapshot.media->hasVideo;
    const bool hasAudio = snapshot.media.has_value() && snapshot.media->hasAudio;
    const std::wstring mediaPath = snapshot.media.has_value() ? snapshot.media->path.wstring() : L"";
    const std::wstring container = snapshot.media.has_value() ? snapshot.media->container : L"";
    const std::wstring videoCodec = snapshot.media.has_value() ? snapshot.media->videoCodec : L"";
    const std::wstring audioCodec = snapshot.media.has_value() ? snapshot.media->audioCodec : L"";
    const std::wstring hdrFormat = snapshot.media.has_value() ? snapshot.media->hdrFormat : L"";
    std::wstring resolution;
    std::wstring frameRate;
    int streamCount = 0;
    if (snapshot.media.has_value()) {
        streamCount = static_cast<int>(snapshot.media->streams.size());
        if (snapshot.media->videoWidth > 0 && snapshot.media->videoHeight > 0) {
            resolution = std::to_wstring(snapshot.media->videoWidth) + L" x " + std::to_wstring(snapshot.media->videoHeight);
        }
        if (snapshot.media->videoFrameRate > 0.0) {
            std::wostringstream fps;
            fps << std::fixed << std::setprecision(2) << snapshot.media->videoFrameRate << L" fps";
            frameRate = fps.str();
        }
    }

    std::wostringstream json;
    json << L"{\"type\":\"state\",\"state\":{";
    json << L"\"playbackState\":\"" << JsonEscape(playbackState) << L"\",";
    json << L"\"lastError\":\"" << JsonEscape(snapshot.lastError) << L"\",";
    json << L"\"mediaName\":\"" << JsonEscape(mediaName) << L"\",";
    json << L"\"mediaPath\":\"" << JsonEscape(mediaPath) << L"\",";
    json << L"\"hasMedia\":" << (snapshot.media.has_value() ? L"true" : L"false") << L",";
    json << L"\"hasVideo\":" << (hasVideo ? L"true" : L"false") << L",";
    json << L"\"hasAudio\":" << (hasAudio ? L"true" : L"false") << L",";
    json << L"\"positionMs\":" << positionMs << L",";
    json << L"\"durationMs\":" << durationMs << L",";
    json << L"\"bufferedEndMs\":" << bufferedEndMs << L",";
    json << L"\"buffering\":" << (buffering ? L"true" : L"false") << L",";
    json << L"\"networkKbps\":" << (networkBytesPerSecond / 1024) << L",";
    json << L"\"volume\":" << std::fixed << std::setprecision(3) << std::clamp(snapshot.volume, 0.0, 1.0) << L",";
    json << L"\"runtimeLabel\":\"" << JsonEscape(RuntimeShortLabel()) << L"\",";
    json << L"\"backendLabel\":\"" << JsonEscape(RuntimeLabel()) << L"\",";
    json << L"\"sidebarCollapsed\":" << (inspectorCollapsed_ ? L"true" : L"false") << L",";
    json << L"\"fullscreen\":" << (fullscreen_ ? L"true" : L"false") << L",";
    json << L"\"fullscreenTransportVisible\":"
         << ((!fullscreen_ || fullscreenTransportTarget_ > 0.0 || draggingProgress_ || draggingVolume_) ? L"true" : L"false")
         << L",";
    json << L"\"subtitleMenuOpen\":" << ((subtitleMenuTarget_ > 0.0 || subtitleMenuAmount_ > 0.01) ? L"true" : L"false") << L",";
    json << L"\"inspectorTab\":\"" << InspectorTabName(inspectorTab_) << L"\",";
    json << L"\"hdrAvailable\":" << (hdrAvailable ? L"true" : L"false") << L",";
    json << L"\"hdrOutput\":" << (settings.video.dolbyVisionHdrOutput ? L"true" : L"false") << L",";
    json << L"\"cmv4Available\":" << (cmv4Available ? L"true" : L"false") << L",";
    json << L"\"cmv4Enabled\":" << (cmv4Enabled ? L"true" : L"false") << L",";
    json << L"\"audioSelectedTrack\":" << settings.audio.selectedTrackIndex << L",";
    json << L"\"subtitleSelectedTrack\":" << settings.subtitles.selectedTrackIndex << L",";
    json << L"\"subtitleDelayMs\":" << settings.subtitles.subtitleDelayMs << L",";
    json << L"\"subtitleFontScale\":" << std::fixed << std::setprecision(2) << settings.subtitles.fontScale << L",";
    json << L"\"subtitleOffsetX\":" << settings.subtitles.offsetXPx << L",";
    json << L"\"subtitleOffsetY\":" << settings.subtitles.offsetYPx << L",";
    json << L"\"danmakuEnabled\":" << (settings.danmaku.enabled ? L"true" : L"false") << L",";
    json << L"\"danmakuMode\":" << settings.danmaku.mode << L",";
    json << L"\"danmakuOpacityPercent\":" << settings.danmaku.opacityPercent << L",";
    json << L"\"danmakuSpeedPercent\":" << settings.danmaku.speedPercent << L",";
    json << L"\"danmakuPath\":\"" << JsonEscape(settings.danmaku.externalDanmakuPath.wstring()) << L"\",";
    json << L"\"audioTracks\":" << TrackItemsJson(snapshot.media,
                                                   L"Audio",
                                                   anvil::playback::kAudioTrackOff,
                                                  anvil::playback::kAudioTrackAuto) << L",";
    json << L"\"subtitleTracks\":" << TrackItemsJson(snapshot.media,
                                                     L"Subtitle",
                                                     anvil::playback::kSubtitleTrackOff,
                                                     anvil::playback::kSubtitleTrackAuto) << L",";
    json << L"\"hdrToneCurveAvailable\":" << (hdrToneCurveAvailable ? L"true" : L"false") << L",";
    json << L"\"hdrToneCurvePeakNits\":" << settings.video.peakBrightnessNits << L",";
    json << L"\"hdrToneCurve\":" << HdrToneCurveJson(settings.video) << L",";
    json << L"\"container\":\"" << JsonEscape(container) << L"\",";
    json << L"\"videoCodec\":\"" << JsonEscape(videoCodec) << L"\",";
    json << L"\"audioCodec\":\"" << JsonEscape(audioCodec) << L"\",";
    json << L"\"hdrFormat\":\"" << JsonEscape(hdrFormat) << L"\",";
    json << L"\"resolution\":\"" << JsonEscape(resolution) << L"\",";
    json << L"\"frameRate\":\"" << JsonEscape(frameRate) << L"\",";
    json << L"\"streamCount\":" << streamCount << L",";
    json << L"\"recentMedia\":" << MediaPathItemsJson(recentMedia_, 8) << L",";
    json << L"\"folderMedia\":" << MediaPathItemsJson(currentFolderEntries_, 16) << L",";
    json << L"\"logLines\":" << RecentLogLinesJson(controller_.LogSink(), 14);
    json << L"}}";
    return json.str();
}

void MainWindow::PostWebUiState(const bool force) const {
    if (webUiActive_ && webUiHost_ && webUiHost_->Ready()) {
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            lastWebUiStatePostedAt_.time_since_epoch().count() != 0 &&
            now - lastWebUiStatePostedAt_ < kWebUiProgressUpdateInterval) {
            return;
        }
        lastWebUiStatePostedAt_ = now;
        webUiHost_->PostJson(BuildWebUiStateJson());
    }
}

void MainWindow::HandleWebUiMessage(const std::wstring_view message) {
    if (MessageContains(message, L"\"type\":\"requestState\"")) {
        PostWebUiState();
        return;
    }

    if (!MessageContains(message, L"\"type\":\"command\"")) {
        return;
    }

    if (MessageContains(message, L"\"command\":\"setAllowInsecureCertificates\"")) {
        const bool enabled = MessageContains(message, L"\"enabled\":true");
        if (webUiHost_) {
            webUiHost_->SetAllowInsecureCertificates(enabled);
        }
        LogApp(LogLevel::Info,
               std::wstring(L"web ui insecure certificates ") + (enabled ? L"enabled" : L"disabled"));
    } else if (MessageContains(message, L"\"command\":\"setWebUiRoute\"")) {
        const bool playerRoute = MessageContains(message, L"\"route\":\"player\"");
        if (webUiPlayerRouteActive_ != playerRoute) {
            webUiPlayerRouteActive_ = playerRoute;
            if (!playerRoute) {
                HideSubtitleMenu();
                webUiSubtitleGeometryValid_ = false;
                webUiTransportGeometryValid_ = false;
                webUiSubtitleAnchor_ = RECT{};
                webUiSubtitlePopover_ = RECT{};
                webUiTransportBounds_ = RECT{};
                controller_.Stop();
                SetPlaybackTimer(false);
                StopRuntimeAsync(true);
            }
            LogApp(LogLevel::Debug,
                   std::wstring(L"web ui route=") + (playerRoute ? L"player" : L"library"));
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    } else if (MessageContains(message, L"\"command\":\"open\"")) {
        OpenFileDialog();
    } else if (MessageContains(message, L"\"command\":\"pickLocalFolder\"")) {
        OpenLocalFolderDialog();
    } else if (MessageContains(message, L"\"command\":\"scanLocalFolder\"")) {
        if (const auto path = ReadJsonString(message, L"path")) {
            const SmbCredentials credentials{
                ReadJsonString(message, L"username").value_or(L""),
                ReadJsonString(message, L"password").value_or(L""),
            };
            std::wstring pathText = *path;
            if (StartsWithInsensitive(pathText, L"smb://")) {
                pathText = NormalizeSmbPathText(pathText);
            }
            const auto folder = NormalizeListPath(pathText);
            if (const auto connectError = ConnectSmbPathForAccess(folder, credentials)) {
                if (webUiActive_ && webUiHost_ && webUiHost_->Ready()) {
                    webUiHost_->PostJson(LocalFolderScanFailedJson(folder, *connectError));
                }
                return;
            }
            std::error_code error;
            if (!std::filesystem::exists(folder, error) || !std::filesystem::is_directory(folder, error)) {
                if (webUiActive_ && webUiHost_ && webUiHost_->Ready()) {
                    webUiHost_->PostJson(LocalFolderScanFailedJson(folder, L"所选路径不是可读取的文件夹"));
                }
                return;
            }
            StartLocalFolderScanAsync(hwnd_, folder, credentials);
        }
    } else if (MessageContains(message, L"\"command\":\"listSmbDirectory\"")) {
        const auto requestId = ReadJsonString(message, L"requestId").value_or(L"");
        const auto host = ReadJsonString(message, L"host").value_or(L"");
        const auto path = ReadJsonString(message, L"path").value_or(L"");
        const SmbCredentials credentials{
            ReadJsonString(message, L"username").value_or(L""),
            ReadJsonString(message, L"password").value_or(L""),
        };
        StartSmbDirectoryListAsync(hwnd_, requestId, host, path, credentials);
    } else if (MessageContains(message, L"\"command\":\"connectSmbShare\"")) {
        if (const auto path = ReadJsonString(message, L"path")) {
            const SmbCredentials credentials{
                ReadJsonString(message, L"username").value_or(L""),
                ReadJsonString(message, L"password").value_or(L""),
            };
            const auto normalizedPath = std::filesystem::path(StartsWithInsensitive(*path, L"smb://")
                ? NormalizeSmbPathText(*path)
                : *path);
            if (const auto connectError = ConnectSmbPathForAccess(normalizedPath, credentials)) {
                LogApp(LogLevel::Warning, L"smb connect failed path=" + normalizedPath.wstring() + L" error=" + *connectError);
            }
        }
    } else if (MessageContains(message, L"\"command\":\"listWebDavDirectory\"")) {
        StartWebDavDirectoryListAsync(
            hwnd_,
            ReadJsonString(message, L"requestId").value_or(L""),
            ReadJsonString(message, L"url").value_or(L""),
            ReadJsonString(message, L"username").value_or(L""),
            ReadJsonString(message, L"password").value_or(L""));
    } else if (MessageContains(message, L"\"command\":\"scanWebDavFolder\"")) {
        StartWebDavScanAsync(
            hwnd_,
            ReadJsonString(message, L"url").value_or(L""),
            ReadJsonString(message, L"username").value_or(L""),
            ReadJsonString(message, L"password").value_or(L""));
    } else if (MessageContains(message, L"\"command\":\"debugLog\"")) {
        if (const auto text = ReadJsonString(message, L"message")) {
            LogApp(LogLevel::Debug, L"web ui debug: " + *text);
        }
    } else if (MessageContains(message, L"\"command\":\"openPath\"")) {
        if (const auto path = ReadJsonString(message, L"path")) {
            auto* postedPath = new std::filesystem::path(*path);
            if (!PostMessageW(hwnd_, kOpenPathMessage, 1, reinterpret_cast<LPARAM>(postedPath))) {
                delete postedPath;
                LogApp(LogLevel::Error, L"web ui openPath post failed path=" + *path);
            }
        }
    } else if (MessageContains(message, L"\"command\":\"playPause\"")) {
        TogglePlayback();
    } else if (MessageContains(message, L"\"command\":\"stop\"")) {
        StopPlayback();
    } else if (MessageContains(message, L"\"command\":\"back\"")) {
        SeekRelative(-std::chrono::seconds{10});
    } else if (MessageContains(message, L"\"command\":\"forward\"")) {
        SeekRelative(std::chrono::seconds{10});
    } else if (MessageContains(message, L"\"command\":\"toggleSidebar\"")) {
        ToggleSidebar();
    } else if (MessageContains(message, L"\"command\":\"toggleFullscreen\"")) {
        ToggleFullscreen();
    } else if (MessageContains(message, L"\"command\":\"showFullscreenTransport\"")) {
        ShowFullscreenTransport(L"web_ui_activation");
    } else if (MessageContains(message, L"\"command\":\"subtitleGeometry\"")) {
        const double scale = std::clamp(ReadJsonNumber(message, L"scale").value_or(1.0), 0.25, 4.0);
        const auto scaled = [scale](const std::optional<double>& value) {
            return static_cast<int>(std::round(value.value_or(0.0) * scale));
        };
        const auto anchorLeft = ReadJsonNumber(message, L"anchorLeft");
        const auto anchorTop = ReadJsonNumber(message, L"anchorTop");
        const auto anchorWidth = ReadJsonNumber(message, L"anchorWidth");
        const auto anchorHeight = ReadJsonNumber(message, L"anchorHeight");
        const auto popoverLeft = ReadJsonNumber(message, L"popoverLeft");
        const auto popoverTop = ReadJsonNumber(message, L"popoverTop");
        const auto popoverWidth = ReadJsonNumber(message, L"popoverWidth");
        const auto popoverHeight = ReadJsonNumber(message, L"popoverHeight");
        if (anchorLeft && anchorTop && anchorWidth && anchorHeight &&
            popoverLeft && popoverTop && popoverWidth && popoverHeight) {
            const int anchorX = scaled(anchorLeft);
            const int anchorY = scaled(anchorTop);
            const int anchorW = std::max(1, scaled(anchorWidth));
            const int anchorH = std::max(1, scaled(anchorHeight));
            const int popoverX = scaled(popoverLeft);
            const int popoverY = scaled(popoverTop);
            const int popoverW = std::max(1, scaled(popoverWidth));
            const int popoverH = std::max(1, scaled(popoverHeight));
            webUiSubtitleAnchor_ = MakeRect(anchorX, anchorY, anchorX + anchorW, anchorY + anchorH);
            webUiSubtitlePopover_ = MakeRect(popoverX, popoverY, popoverX + popoverW, popoverY + popoverH);
            webUiSubtitleGeometryValid_ = true;
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    } else if (MessageContains(message, L"\"command\":\"transportGeometry\"")) {
        const double scale = std::clamp(ReadJsonNumber(message, L"scale").value_or(1.0), 0.25, 4.0);
        const auto scaled = [scale](const std::optional<double>& value) {
            return static_cast<int>(std::round(value.value_or(0.0) * scale));
        };
        const auto left = ReadJsonNumber(message, L"left");
        const auto top = ReadJsonNumber(message, L"top");
        const auto width = ReadJsonNumber(message, L"width");
        const auto height = ReadJsonNumber(message, L"height");
        if (left && top && width && height) {
            const int x = scaled(left);
            const int y = scaled(top);
            const int w = std::max(1, scaled(width));
            const int h = std::max(1, scaled(height));
            webUiTransportBounds_ = MakeRect(x, y, x + w, y + h);
            webUiTransportGeometryValid_ = true;
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    } else if (MessageContains(message, L"\"command\":\"subtitleMenu\"")) {
        ShowSubtitleMenu();
    } else if (MessageContains(message, L"\"command\":\"hideSubtitleMenu\"")) {
        HideSubtitleMenu();
    } else if (MessageContains(message, L"\"command\":\"settings\"")) {
        Execute(Command::Settings);
    } else if (MessageContains(message, L"\"command\":\"inspectorRecent\"")) {
        Execute(Command::InspectorRecent);
    } else if (MessageContains(message, L"\"command\":\"inspectorFolder\"")) {
        Execute(Command::InspectorFolder);
    } else if (MessageContains(message, L"\"command\":\"inspectorMedia\"")) {
        Execute(Command::InspectorMedia);
    } else if (MessageContains(message, L"\"command\":\"inspectorSystem\"")) {
        Execute(Command::InspectorSystem);
    } else if (MessageContains(message, L"\"command\":\"inspectorLog\"")) {
        Execute(Command::InspectorLog);
    } else if (MessageContains(message, L"\"command\":\"toggleHdr\"")) {
        Execute(Command::ToggleDolbyVisionHdr);
    } else if (MessageContains(message, L"\"command\":\"toggleCmv4\"")) {
        Execute(Command::ToggleDolbyVisionCmv4Approx);
    } else if (MessageContains(message, L"\"command\":\"setAudioTrack\"")) {
        if (const auto index = ReadJsonNumber(message, L"index")) {
            ApplyAudioSelection(static_cast<int>(std::round(*index)));
        }
    } else if (MessageContains(message, L"\"command\":\"setSubtitleTrack\"")) {
        if (const auto index = ReadJsonNumber(message, L"index")) {
            ApplySubtitleSelection(static_cast<int>(std::round(*index)));
        }
    } else if (MessageContains(message, L"\"command\":\"setSubtitleDelay\"")) {
        if (const auto delayMs = ReadJsonNumber(message, L"delayMs")) {
            const auto settings = controller_.Settings();
            ApplySubtitleDelayDelta(static_cast<int>(std::round(*delayMs)) - settings.subtitles.subtitleDelayMs);
        }
    } else if (MessageContains(message, L"\"command\":\"setSubtitleFontScale\"")) {
        if (const auto scale = ReadJsonNumber(message, L"scale")) {
            const auto settings = controller_.Settings();
            ApplySubtitleFontScaleDelta(std::clamp(*scale, 0.5, 2.0) - settings.subtitles.fontScale);
        }
    } else if (MessageContains(message, L"\"command\":\"setSubtitleOffset\"")) {
        const auto x = ReadJsonNumber(message, L"x");
        const auto y = ReadJsonNumber(message, L"y");
        if (x && y) {
            const auto settings = controller_.Settings();
            ApplySubtitleOffsetDelta(static_cast<int>(std::round(*x)) - settings.subtitles.offsetXPx,
                                     static_cast<int>(std::round(*y)) - settings.subtitles.offsetYPx);
        }
    } else if (MessageContains(message, L"\"command\":\"openSubtitleFile\"")) {
        OpenSubtitleFileDialog();
    } else if (MessageContains(message, L"\"command\":\"toggleDanmakuEnabled\"")) {
        ToggleDanmakuEnabled();
    } else if (MessageContains(message, L"\"command\":\"cycleDanmakuMode\"")) {
        CycleDanmakuMode();
    } else if (MessageContains(message, L"\"command\":\"setDanmakuOpacity\"")) {
        if (const auto opacityPercent = ReadJsonNumber(message, L"opacityPercent")) {
            const auto settings = controller_.Settings();
            ApplyDanmakuOpacityDelta(static_cast<int>(std::round(*opacityPercent)) - settings.danmaku.opacityPercent);
        }
    } else if (MessageContains(message, L"\"command\":\"setDanmakuSpeed\"")) {
        if (const auto speedPercent = ReadJsonNumber(message, L"speedPercent")) {
            const auto settings = controller_.Settings();
            ApplyDanmakuSpeedDelta(static_cast<int>(std::round(*speedPercent)) - settings.danmaku.speedPercent);
        }
    } else if (MessageContains(message, L"\"command\":\"openDanmakuFile\"")) {
        OpenDanmakuFileDialog();
    } else if (MessageContains(message, L"\"command\":\"resetHdrToneCurve\"")) {
        ResetHdrToneCurve();
    } else if (MessageContains(message, L"\"command\":\"toggleHdrToneCurveExpanded\"")) {
        Execute(Command::ToggleHdrToneCurveExpanded);
    } else if (MessageContains(message, L"\"command\":\"setHdrToneCurvePoints\"")) {
        std::array<double, anvil::playback::kHdrToneCurvePointCount> outputNits{};
        std::array<bool, anvil::playback::kHdrToneCurvePointCount> hasOutput{};
        for (std::size_t index = 1; index < outputNits.size(); ++index) {
            const std::wstring field = L"output" + std::to_wstring(index);
            if (const auto value = ReadJsonNumber(message, field.c_str())) {
                outputNits[index] = *value;
                hasOutput[index] = true;
            }
        }
        ApplyHdrToneCurvePoints(outputNits, hasOutput);
    } else if (MessageContains(message, L"\"command\":\"setHdrToneCurvePoint\"")) {
        const auto index = ReadJsonNumber(message, L"index");
        const auto outputNits = ReadJsonNumber(message, L"outputNits");
        if (index && outputNits) {
            ApplyHdrToneCurvePoint(static_cast<int>(std::round(*index)), *outputNits);
        }
    } else if (MessageContains(message, L"\"command\":\"setVolume\"")) {
        if (const auto volume = ReadJsonNumber(message, L"volume")) {
            ApplyVolume(std::clamp(*volume, 0.0, 1.0), true);
        }
    } else if (MessageContains(message, L"\"command\":\"seekToRatio\"")) {
        const auto snapshot = controller_.Snapshot();
        if (snapshot.media.has_value() && snapshot.media->duration.count() > 0) {
            if (const auto ratio = ReadJsonNumber(message, L"ratio")) {
                const auto duration = snapshot.media->duration;
                SeekToPosition(std::chrono::milliseconds{
                    static_cast<long long>(static_cast<double>(duration.count()) * std::clamp(*ratio, 0.0, 1.0))});
            }
        }
    }

    PostWebUiState();
}

void MainWindow::OpenInitialPath(const std::filesystem::path& path, const bool autoplay) {
    if (!path.empty()) {
        LogApp(LogLevel::Info, L"initial path=" + path.wstring() + L" autoplay=" + (autoplay ? L"true" : L"false"));
        OpenPath(path, autoplay);
    }
    PostWebUiState();
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window) {
        try {
            return window->HandleMessage(message, wParam, lParam);
        } catch (const std::exception& error) {
            window->LogApp(LogLevel::Error,
                           L"window message exception message=0x" + std::to_wstring(message) +
                               L" error=" + Utf8ToWide(error.what()));
            return 0;
        } catch (...) {
            window->LogApp(LogLevel::Error,
                           L"window message exception message=0x" + std::to_wstring(message) +
                               L" error=unknown");
            return 0;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::VideoHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(hwnd, window->hwnd_, &point, 1);
        return SendMessageW(window->hwnd_, message, wParam, MAKELPARAM(point.x, point.y));
    }
    if (window && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
        return SendMessageW(window->hwnd_, message, wParam, lParam);
    }

    if (message == WM_ERASEBKGND) {
        return 1;
    }

    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::FullscreenOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(hwnd, window->hwnd_, &point, 1);
        return SendMessageW(window->hwnd_, message, wParam, MAKELPARAM(point.x, point.y));
    }
    if (window && message == WM_MOUSEWHEEL) {
        return SendMessageW(window->hwnd_, message, wParam, lParam);
    }
    if (window && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
        return SendMessageW(window->hwnd_, message, wParam, lParam);
    }

    if (window && message == WM_PAINT) {
        window->PaintFullscreenOverlay(hwnd);
        return 0;
    }

    if (window && message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window->hwnd_, &point);
        SetCursor(LoadCursorW(nullptr, window->IsPointInteractive(point) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    if (message == WM_ERASEBKGND) {
        return 1;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::TransportOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(hwnd, window->hwnd_, &point, 1);
        return SendMessageW(window->hwnd_, message, wParam, MAKELPARAM(point.x, point.y));
    }
    if (window && message == WM_MOUSEWHEEL) {
        return SendMessageW(window->hwnd_, message, wParam, lParam);
    }
    if (window && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
        return SendMessageW(window->hwnd_, message, wParam, lParam);
    }

    if (window && message == WM_PAINT) {
        window->PaintTransportOverlay(hwnd);
        return 0;
    }

    if (window && message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window->hwnd_, &point);
        SetCursor(LoadCursorW(nullptr, window->IsPointInteractive(point) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    if (message == WM_ERASEBKGND) {
        return 1;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::BufferingOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::BufferingHudOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (window && message == WM_TIMER) {
        window->RenderBufferingHudOverlay(window->bufferingOverlayStats_);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::SubtitleMenuOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(hwnd, window->hwnd_, &point, 1);
        return SendMessageW(window->hwnd_, message, wParam, MAKELPARAM(point.x, point.y));
    }
    if (window && message == WM_MOUSEWHEEL) {
        return SendMessageW(window->hwnd_, message, wParam, lParam);
    }

    if (window && message == WM_PAINT) {
        window->PaintSubtitleMenuOverlay(hwnd);
        return 0;
    }

    if (window && message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window->hwnd_, &point);
        SetCursor(LoadCursorW(nullptr, window->IsPointInteractive(point) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    if (message == WM_ERASEBKGND) {
        return 1;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::HdrToneCurveWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hdrToneCurveWindow_ = hwnd;
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window) {
        return window->HandleHdrToneCurveWindowMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void CALLBACK MainWindow::PlaybackTimerQueueProc(PVOID context, BOOLEAN) {
    auto* window = static_cast<MainWindow*>(context);
    if (window && window->hwnd_) {
        PostMessageW(window->hwnd_, kPlaybackTimerTickMessage, 0, 0);
    }
}

LRESULT MainWindow::HandleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        ApplyWindowChrome();
        DragAcceptFiles(hwnd_, TRUE);
        SetPlaybackTimer(false);
        MarkLayoutDirty();
        EnsureLayout();
        return 0;
    case kVideoFrameReadyMessage:
        if (runtimeStopAsyncInProgress_.load()) {
            return 0;
        }
        videoDecoder_.AcknowledgeFrameNotification();
        if (backend_ == PlaybackBackend::RawFrameBridge) {
            controller_.UpdateClock();
            const auto snapshot = controller_.Snapshot();
            RenderPlaybackTick(snapshot, false);
            InvalidateVideoSurface();
        }
        return 0;
    case kNativeVideoFrameReadyMessage: {
        if (runtimeStopAsyncInProgress_.load()) {
            return 0;
        }
        if (nativeVideoDecoder_) {
            nativeVideoDecoder_->AcknowledgeFrameNotification();
            if (SidebarAnimationActive() && !webUiActive_) {
                return 0;
            }
            NativeVideoFrame frame;
            const auto snapshot = controller_.Snapshot();
            const auto stats = nativeVideoDecoder_->Stats();
            const bool nativeBuffering = snapshot.state == PlaybackState::Playing && stats.buffering;
            if (!nativeBuffering) {
                if (nativeSeekPrerollHoldingAudio_ && !stats.seekRecoveryAudioHandoffReady) {
                    controller_.SyncClock(stats.clockPosition);
                } else {
                    ResumeNativeSeekPrerollAudio(stats.clockPosition);
                }
            }
            const bool shouldRenderFrame =
                (snapshot.state == PlaybackState::Playing || pendingPausedFrameRefresh_) &&
                !nativeBuffering;
            if (shouldRenderFrame && nativeVideoDecoder_->LatestFrame(frame) && d3dRenderer_) {
                d3dRenderer_->Render(frame);
                heldNativeFrame_ = frame;
                heldNativeFrameNeedsPresent_ = false;
                nativeFrameHoldVisible_ = false;
                MaybeLogNativeSchedulerStats(stats);
                if (pendingPausedFrameRefresh_) {
                    pendingPausedFrameRefresh_ = false;
                    nativeFrameHoldVisible_ = true;
                    MarkLayoutDirty();
                    EnsureLayout();
                    RenderHeldNativeFrame();
                    LogApp(LogLevel::Debug, L"paused native frame refresh completed");
                }
                if (webUiActive_ && webUiPlayerRouteActive_) {
                    UpdateVideoHost();
                }
            } else if (nativeBuffering) {
                MaybeLogNativeSchedulerStats(stats);
                if (webUiActive_ && webUiPlayerRouteActive_) {
                    UpdateVideoHost();
                }
                UpdateBufferingOverlay(&stats);
            }
            if (snapshot.state == PlaybackState::Playing) {
                RenderPlaybackTick(snapshot, false);
            }
        }
        return 0;
    }
    case kNativeVideoDecodeFailedMessage: {
        std::unique_ptr<NativeDecodeFailure> failure(reinterpret_cast<NativeDecodeFailure*>(lParam));
        const std::wstring message = failure && !failure->message.empty()
                                         ? failure->message
                                         : L"native video decoder failed";
        const auto snapshot = controller_.Snapshot();
        if (!snapshot.media.has_value() || (failure && snapshot.media->path != failure->path)) {
            LogApp(LogLevel::Debug, L"ignored stale native decode failure message=" + message);
            return 0;
        }

        LogApp(LogLevel::Error, L"native decode failed message=" + message);
        FailPlaybackRuntime(message);
        return 0;
    }
    case kRuntimeStopCompleteMessage:
        WaitForAsyncRuntimeStop();
        ContinueRuntimeAfterAsyncStop();
        return 0;
    case kOpenPathMessage: {
        std::unique_ptr<std::filesystem::path> path(reinterpret_cast<std::filesystem::path*>(lParam));
        if (path && !path->empty()) {
            OpenPath(*path, wParam != 0);
        }
        return 0;
    }
    case kLocalFolderScanResultMessage: {
        std::unique_ptr<std::wstring> json(reinterpret_cast<std::wstring*>(lParam));
        if (json && webUiActive_ && webUiHost_ && webUiHost_->Ready()) {
            webUiHost_->PostJson(*json);
        }
        return 0;
    }
    case WM_DPICHANGED:
        dpi_ = HIWORD(wParam);
        if (auto* suggested = reinterpret_cast<RECT*>(lParam)) {
            SetWindowPos(
                hwnd_,
                nullptr,
                suggested->left,
                suggested->top,
                RectWidth(*suggested),
                RectHeight(*suggested),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        MarkLayoutDirty();
        EnsureLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(720);
        info->ptMinTrackSize.y = Scale(500);
        return 0;
    }
    case WM_SIZE:
        MarkLayoutDirty();
        EnsureLayout();
        if (webUiActive_ && webUiHost_) {
            RECT client{};
            GetClientRect(hwnd_, &client);
            webUiHost_->Resize(client);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSELEAVE:
        trackingMouseLeave_ = false;
        ClearButtonHoverTargets();
        hoveredButton_ = -1;
        hoveredInspectorPathItem_ = -1;
        hoveredHdrToneCurvePoint_ = -1;
        SetVolumeSliderHover(false);
        SetProgressHover(false);
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd_, &point);
            SetCursor(LoadCursorW(nullptr, IsPointInteractive(point) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        OnLeftButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_LBUTTONUP:
        OnLeftButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), point);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != hwnd_) {
            CancelProgressDrag();
            CancelVolumeDrag();
            CancelHdrToneCurveInteraction();
            CancelSettingsScrollDrag();
            CancelVideoPress();
        }
        return 0;
    case WM_CANCELMODE:
        CancelProgressDrag();
        CancelVolumeDrag();
        CancelHdrToneCurveInteraction();
        CancelSettingsScrollDrag();
        CancelVideoPress();
        return 0;
    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == VK_ESCAPE) {
            OnKeyDown(wParam);
            return 0;
        }
        break;
    case WM_DROPFILES:
        OnDropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case kPlaybackTimerTickMessage:
        OnPlaybackTimerTick();
        return 0;
    case kNativeColorSettingsRefreshMessage:
        if (static_cast<UINT_PTR>(wParam) == nativeColorSettingsRefreshSerial_) {
            const bool requiresDecoderRefresh = nativeColorSettingsRefreshRequiresDecoderRefresh_;
            nativeColorSettingsRefreshRequiresDecoderRefresh_ = false;
            ApplyNativeColorSettingsRefresh(requiresDecoderRefresh);
        }
        return 0;
    case WM_TIMER:
        if (wParam == kUiAnimationTimer) {
            UpdateUiAnimations();
            return 0;
        }
        if (wParam == kFullscreenChromeHideTimer) {
            HideFullscreenTransportIfIdle();
            return 0;
        }
        if (wParam == kVideoPressTimer) {
            CompleteVideoLongPress();
            return 0;
        }
        if (wParam == kScrollbarAutoHideTimer) {
            KillTimer(hwnd_, kScrollbarAutoHideTimer);
            InvalidateTransportArea();
            InvalidateFullscreenOverlay();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wParam == kPlaybackTimer) {
            OnPlaybackTimerTick();
            return 0;
        }
        break;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_DESTROY:
        SetPlaybackTimer(false);
        KillTimer(hwnd_, kPlaybackTimer);
        KillTimer(hwnd_, kUiAnimationTimer);
        KillTimer(hwnd_, kFullscreenChromeHideTimer);
        KillTimer(hwnd_, kVideoPressTimer);
        KillTimer(hwnd_, kScrollbarAutoHideTimer);
        DragAcceptFiles(hwnd_, FALSE);
        SetTemporaryPlaybackRate(1.0);
        if (hdrToneCurveWindow_) {
            DestroyWindow(hdrToneCurveWindow_);
            hdrToneCurveWindow_ = nullptr;
        }
        StopRuntime();
        ClearPreviewBitmap();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT MainWindow::HandleHdrToneCurveWindowMessage(HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(460);
        info->ptMinTrackSize.y = Scale(340);
        return 0;
    }
    case WM_SIZE:
        UpdateHdrToneCurveFloatingLayout();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (selectingHdrToneCurveRange_) {
            UpdateHdrToneCurveRangeSelection(point);
            return 0;
        }
        if (draggingHdrToneCurve_) {
            hdrToneCurveDragMoved_ = true;
            UpdateHdrToneCurveDrag(point);
            return 0;
        }
        if (!hdrToneCurveWindowTrackingMouseLeave_) {
            TRACKMOUSEEVENT event{};
            event.cbSize = sizeof(event);
            event.dwFlags = TME_LEAVE;
            event.hwndTrack = window;
            TrackMouseEvent(&event);
            hdrToneCurveWindowTrackingMouseLeave_ = true;
        }
        UpdateHdrToneCurveHover(point);
        return 0;
    }
    case WM_MOUSELEAVE:
        hdrToneCurveWindowTrackingMouseLeave_ = false;
        hoveredHdrToneCurvePoint_ = -1;
        InvalidateHdrToneCurveEditor();
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (ContainsPoint(hdrToneCurveFloatingReset_, point)) {
            ResetHdrToneCurve();
            return 0;
        }
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0 && BeginHdrToneCurveRangeSelection(point)) {
            return 0;
        }
        if (BeginHdrToneCurveDrag(point)) {
            return 0;
        }
        if (HasHdrToneCurveSelection()) {
            ClearHdrToneCurveSelection();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (selectingHdrToneCurveRange_) {
            UpdateHdrToneCurveRangeSelection(point);
            EndHdrToneCurveRangeSelection();
            return 0;
        }
        if (draggingHdrToneCurve_) {
            if (hdrToneCurveDragMoved_) {
                UpdateHdrToneCurveDrag(point);
            }
            EndHdrToneCurveDrag();
            return 0;
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != window) {
            CancelHdrToneCurveInteraction();
        }
        return 0;
    case WM_CANCELMODE:
        CancelHdrToneCurveInteraction();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_UP) {
            NudgeHdrToneCurveSelection(1.0);
            return 0;
        }
        if (wParam == VK_DOWN) {
            NudgeHdrToneCurveSelection(-1.0);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            HideHdrToneCurveWindow();
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            RECT hitRect = hdrToneCurvePlot_;
            InflateRect(&hitRect, Scale(18), Scale(18));
            SetCursor(LoadCursorW(nullptr,
                                  ContainsPoint(hitRect, point) ||
                                          ContainsPoint(hdrToneCurveFloatingReset_, point)
                                      ? IDC_HAND
                                      : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_PAINT:
        PaintHdrToneCurveWindow(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        HideHdrToneCurveWindow();
        return 0;
    case WM_DESTROY:
        if (window == hdrToneCurveWindow_) {
            hdrToneCurveWindow_ = nullptr;
            hdrToneCurveExpanded_ = false;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::filesystem::path MainWindow::DefaultLogPath() {
    return std::filesystem::temp_directory_path() / L"anvil-player" / L"anvil-player.log";
}

std::filesystem::path MainWindow::RecentMediaPath() {
    return AppDataStorageFolder() / L"recent-media.txt";
}

void MainWindow::LogApp(const LogLevel level, const std::wstring& message) const {
    if (const auto sink = controller_.LogSink()) {
        sink->Write(level, L"app", message);
    }
}

void MainWindow::LogRuntime(const LogLevel level, const std::wstring& category, const std::wstring& message) const {
    if (const auto sink = controller_.LogSink()) {
        sink->Write(level, category, message);
    }
}

void MainWindow::MaybeLogNativeSchedulerStats(const NativeVideoQueueStats& stats) {
    const auto now = std::chrono::steady_clock::now();
    if (lastNativeStatsLog_.time_since_epoch().count() != 0 &&
        now - lastNativeStatsLog_ < std::chrono::seconds{2}) {
        return;
    }
    lastNativeStatsLog_ = now;

    const std::wstring master = stats.usingAudioClock ? L"audio" : L"wall";
    const std::wstring scheduler = stats.usingAudioClock ? L"audio_clock" : L"wall_clock";
    LogRuntime(LogLevel::Debug,
               L"clock",
               L"master=" + master +
                   L" position=" + FormatTimecode(stats.clockPosition) +
                   L" drift_ms=" + std::to_wstring(stats.driftMs));
    LogRuntime(LogLevel::Debug,
               L"video",
                L"decoder=" + stats.decoder +
                    L" scheduler=" + scheduler +
                    L" timeline_serial=" + std::to_wstring(stats.timelineSerial) +
                    L" queue_depth=" + std::to_wstring(stats.queueDepth) +
                    L" packet_depth=" + std::to_wstring(stats.packetQueueDepth) +
                    L" packet_mb=" + std::to_wstring(stats.packetQueueBytes / (1024 * 1024)) +
                    L" buffered_end=" + FormatTimecode(stats.bufferedEnd) +
                    L" buffered_ms=" + std::to_wstring(stats.bufferedDuration.count()) +
                    L" read_ahead_ms=" + std::to_wstring(stats.readAheadDuration.count()) +
                    L" buffering=" + std::wstring(stats.buffering ? L"true" : L"false") +
                    L" net_kbps=" + std::to_wstring(stats.networkBytesPerSecond / 1024) +
                    L" rendered=" + std::to_wstring(stats.rendered) +
                   L" hardware_frames=" + std::to_wstring(stats.hardwareFrames) +
                   L" zero_copy_frames=" + std::to_wstring(stats.zeroCopyFrames) +
                   L" cpu_transfer_frames=" + std::to_wstring(stats.cpuTransferFrames) +
                   L" dropped_late=" + std::to_wstring(stats.droppedLate) +
                   L" dropped_stale=" + std::to_wstring(stats.droppedStale) +
                   L" dropped_superseded=" + std::to_wstring(stats.droppedSuperseded) +
                   L" dropped_queue_full=" + std::to_wstring(stats.droppedQueueFull) +
                   L" cadence_ms=" + std::to_wstring(stats.frameCadenceMs) +
                   L" early_tolerance_ms=" + std::to_wstring(stats.earlyToleranceMs) +
                   (stats.fallbackReason.empty() ? L"" : L" fallback_reason=" + stats.fallbackReason));

    if (!d3dRenderer_) {
        return;
    }

    const D3D11RenderStats renderStats = d3dRenderer_->TakeRenderStats();
    if (renderStats.frames == 0) {
        return;
    }

    std::wstring renderMessage =
        L"frames=" + std::to_wstring(renderStats.frames) +
        L" hw_frames=" + std::to_wstring(renderStats.hardwareFrames) +
        L" bgra_frames=" + std::to_wstring(renderStats.bgraFrames) +
        L" avg_ms=" + FormatAverageMilliseconds(renderStats.totalRenderUs, renderStats.frames) +
        L" max_ms=" + FormatMillisecondsFromMicroseconds(renderStats.maxRenderUs) +
        L" present_avg_ms=" + FormatAverageMilliseconds(renderStats.presentUs, renderStats.frames) +
        L" present_max_ms=" + FormatMillisecondsFromMicroseconds(renderStats.maxPresentUs) +
        L" slow_frames=" + std::to_wstring(renderStats.slowFrames);

    if (renderStats.hardwareFrames > 0) {
        renderMessage +=
            L" hw_prepare_avg_ms=" + FormatAverageMilliseconds(renderStats.hardwarePrepareUs, renderStats.hardwareFrames) +
            L" srv_cache_hits=" + std::to_wstring(renderStats.hardwareSrvCacheHits) +
            L" srv_cache_misses=" + std::to_wstring(renderStats.hardwareSrvCacheMisses);
    }
    if (renderStats.bgraFrames > 0) {
        renderMessage +=
            L" bgra_upload_avg_ms=" + FormatAverageMilliseconds(renderStats.bgraUploadUs, renderStats.bgraFrames);
    }
    if (renderStats.subtitleFrames > 0 || renderStats.subtitleSurfaceRebuilds > 0) {
        const uint64_t subtitleCount = std::max<uint64_t>(1, renderStats.subtitleFrames);
        renderMessage +=
            L" subtitle_avg_ms=" + FormatAverageMilliseconds(renderStats.subtitleUs, subtitleCount) +
            L" subtitle_frames=" + std::to_wstring(renderStats.subtitleFrames) +
            L" subtitle_rebuilds=" + std::to_wstring(renderStats.subtitleSurfaceRebuilds) +
            L" subtitle_bitmap_rects=" + std::to_wstring(renderStats.subtitleBitmapRects) +
            L" subtitle_bitmap_mpixels=" +
                FormatFixed2(static_cast<double>(renderStats.subtitleBitmapPixels) / 1000000.0);
    }
    if (renderStats.presentSyncFrames > 0 ||
        renderStats.frameLatencyWaits > 0 ||
        renderStats.frameStatsSamples > 0 ||
        renderStats.frameStatsDisjoint > 0) {
        renderMessage +=
            L" present_sync_frames=" + std::to_wstring(renderStats.presentSyncFrames) +
            L" frame_latency_waits=" + std::to_wstring(renderStats.frameLatencyWaits) +
            L" frame_latency_wait_avg_ms=" +
                FormatAverageMilliseconds(renderStats.frameLatencyWaitUs, renderStats.frameLatencyWaits) +
            L" frame_latency_wait_max_ms=" + FormatMillisecondsFromMicroseconds(renderStats.maxFrameLatencyWaitUs) +
            L" frame_latency_wait_timeouts=" + std::to_wstring(renderStats.frameLatencyWaitTimeouts) +
            L" frame_stats_samples=" + std::to_wstring(renderStats.frameStatsSamples) +
            L" frame_stats_disjoint=" + std::to_wstring(renderStats.frameStatsDisjoint);
    }

    LogRuntime(LogLevel::Debug, L"renderer", renderMessage);
}

void MainWindow::ResetNativeBufferingWatchdog() {
    nativeBufferingStartedAt_ = {};
    nativeBufferingLastProgressAt_ = {};
    nativeBufferingLastRendered_ = 0;
    nativeBufferingLastQueueDepth_ = 0;
    nativeBufferingLastPacketDepth_ = 0;
    nativeBufferingLastPacketBytes_ = 0;
    nativeBufferingLastReadAhead_ = std::chrono::milliseconds{0};
    nativeBufferingLastBufferedEnd_ = std::chrono::milliseconds{0};
    nativeBufferingLastClockPosition_ = std::chrono::milliseconds{0};
}

bool MainWindow::CheckNativeBufferingWatchdog(const PlaybackSessionSnapshot& snapshot,
                                              const NativeVideoQueueStats& stats) {
    if (snapshot.state != PlaybackState::Playing || !stats.buffering) {
        ResetNativeBufferingWatchdog();
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool firstSample = nativeBufferingStartedAt_.time_since_epoch().count() == 0;
    const bool progressed =
        firstSample ||
        stats.rendered != nativeBufferingLastRendered_ ||
        stats.queueDepth != nativeBufferingLastQueueDepth_ ||
        stats.packetQueueDepth != nativeBufferingLastPacketDepth_ ||
        stats.packetQueueBytes != nativeBufferingLastPacketBytes_ ||
        stats.readAheadDuration != nativeBufferingLastReadAhead_ ||
        stats.bufferedEnd != nativeBufferingLastBufferedEnd_ ||
        stats.clockPosition != nativeBufferingLastClockPosition_;

    if (firstSample) {
        nativeBufferingStartedAt_ = now;
    }
    if (progressed) {
        nativeBufferingLastProgressAt_ = now;
        nativeBufferingLastRendered_ = stats.rendered;
        nativeBufferingLastQueueDepth_ = stats.queueDepth;
        nativeBufferingLastPacketDepth_ = stats.packetQueueDepth;
        nativeBufferingLastPacketBytes_ = stats.packetQueueBytes;
        nativeBufferingLastReadAhead_ = stats.readAheadDuration;
        nativeBufferingLastBufferedEnd_ = stats.bufferedEnd;
        nativeBufferingLastClockPosition_ = stats.clockPosition;
        return false;
    }

    const auto waitingFor = now - nativeBufferingStartedAt_;
    const auto noProgressFor = nativeBufferingLastProgressAt_.time_since_epoch().count() == 0
                                   ? waitingFor
                                   : now - nativeBufferingLastProgressAt_;
    if (waitingFor < kNativeBufferingHardTimeout &&
        noProgressFor < kNativeBufferingNoProgressTimeout) {
        return false;
    }

    std::wstring message =
        L"Playback stalled while waiting for native frames at " +
        FormatTimecode(stats.clockPosition) +
        L" decoder=" + stats.decoder +
        L" rendered=" + std::to_wstring(stats.rendered) +
        L" queue=" + std::to_wstring(stats.queueDepth) +
        L" packets=" + std::to_wstring(stats.packetQueueDepth) +
        L" read_ahead_ms=" + std::to_wstring(stats.readAheadDuration.count()) +
        L" wait_ms=" + std::to_wstring(
            std::chrono::duration_cast<std::chrono::milliseconds>(waitingFor).count()) +
        L" no_progress_ms=" + std::to_wstring(
            std::chrono::duration_cast<std::chrono::milliseconds>(noProgressFor).count());
    FailPlaybackRuntime(message);
    return true;
}

void MainWindow::FailPlaybackRuntime(const std::wstring& message) {
    LogApp(LogLevel::Error, L"playback runtime failed message=" + message);
    ClearDeferredRuntimeStart();
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    if (RuntimeStopInProgress()) {
        controller_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
    } else {
        SetTemporaryPlaybackRate(1.0);
    }
    StopRuntimeAsync(true);
    controller_.SetError(message);
    SetPlaybackTimer(false);
    ResetNativeBufferingWatchdog();
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::StartUiAnimationTimer() const {
    if (hwnd_) {
        SetTimer(hwnd_, kUiAnimationTimer, kUiAnimationTimerMs, nullptr);
    }
}

bool MainWindow::SidebarAnimationActive() const {
    return !fullscreen_ && std::abs(inspectorCollapseAmount_ - inspectorCollapseTarget_) > 0.001;
}

void MainWindow::SetButtonHoverTarget(const int buttonIndex, const bool hovered) {
    if (buttonIndex < 0 || buttonIndex >= static_cast<int>(buttons_.size())) {
        return;
    }

    const std::size_t slot = static_cast<std::size_t>(buttons_[static_cast<std::size_t>(buttonIndex)].command);
    if (slot >= buttonHoverAnimations_.size()) {
        return;
    }

    UiMotionValue& animation = buttonHoverAnimations_[slot];
    const double target = hovered ? 1.0 : 0.0;
    if (std::abs(animation.target - target) < 0.001) {
        return;
    }

    animation.startAmount = animation.amount;
    animation.target = target;
    animation.startedAt = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ClearButtonHoverTargets() {
    bool changed = false;
    const auto now = std::chrono::steady_clock::now();
    for (auto& animation : buttonHoverAnimations_) {
        if (animation.target <= 0.001 && animation.amount <= 0.001) {
            continue;
        }
        animation.startAmount = animation.amount;
        animation.target = 0.0;
        animation.startedAt = now;
        changed = true;
    }
    if (!changed) {
        return;
    }

    StartUiAnimationTimer();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::TriggerButtonPress(const int buttonIndex) {
    if (buttonIndex < 0 || buttonIndex >= static_cast<int>(buttons_.size())) {
        return;
    }

    const std::size_t slot = static_cast<std::size_t>(buttons_[static_cast<std::size_t>(buttonIndex)].command);
    if (slot >= buttonPressAnimations_.size()) {
        return;
    }

    UiMotionValue& animation = buttonPressAnimations_[slot];
    animation.amount = 1.0;
    animation.startAmount = 1.0;
    animation.target = 0.0;
    animation.startedAt = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

double MainWindow::ButtonHoverAmount(const Command command) const {
    const std::size_t slot = static_cast<std::size_t>(command);
    if (slot >= buttonHoverAnimations_.size()) {
        return 0.0;
    }
    return std::clamp(buttonHoverAnimations_[slot].amount, 0.0, 1.0);
}

double MainWindow::ButtonPressAmount(const Command command) const {
    const std::size_t slot = static_cast<std::size_t>(command);
    if (slot >= buttonPressAnimations_.size()) {
        return 0.0;
    }
    return std::clamp(buttonPressAnimations_[slot].amount, 0.0, 1.0);
}

void MainWindow::SetVolumeSliderHover(const bool hovered) {
    if (volumeSliderHovered_ == hovered && volumeHoverTarget_ == (hovered ? 1.0 : 0.0)) {
        return;
    }

    volumeSliderHovered_ = hovered;
    volumeHoverStartAmount_ = volumeHoverAmount_;
    volumeHoverTarget_ = hovered ? 1.0 : 0.0;
    volumeHoverAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
}

void MainWindow::UpdateUiAnimations() {
    const auto now = std::chrono::steady_clock::now();
    const RECT previousVideoSurface = videoSurface_;
    const RECT previousInspector = inspector_;
    const double previousInspectorCollapseAmount = inspectorCollapseAmount_;
    const double previousProgressHoverAmount = progressHoverAmount_;
    const double previousFullscreenTransportAmount = fullscreenTransportAmount_;
    const double previousSubtitleMenuAmount = subtitleMenuAmount_;
    const double previousVolumeHoverAmount = volumeHoverAmount_;
    bool sidebarComplete = true;
    bool hoverComplete = true;
    bool fullscreenTransportComplete = true;
    bool subtitleMenuComplete = true;
    bool volumeHoverComplete = true;
    bool commandAnimationsComplete = true;
    const bool sidebarWasActive = SidebarAnimationActive();

    inspectorCollapseAmount_ = AnimatedValue(inspectorCollapseStartAmount_,
                                             inspectorCollapseTarget_,
                                             inspectorAnimationStartedAt_,
                                             kSidebarAnimationDuration,
                                             now,
                                             MotionCurve::Fluid,
                                             sidebarComplete);
    progressHoverAmount_ = AnimatedValue(progressHoverStartAmount_,
                                         progressHoverTarget_,
                                         progressHoverAnimationStartedAt_,
                                         kProgressHoverAnimationDuration,
                                         now,
                                         MotionCurve::Press,
                                         hoverComplete);
    fullscreenTransportAmount_ = AnimatedValue(fullscreenTransportStartAmount_,
                                               fullscreenTransportTarget_,
                                               fullscreenTransportAnimationStartedAt_,
                                               kFullscreenTransportAnimationDuration,
                                               now,
                                               MotionCurve::Fluid,
                                               fullscreenTransportComplete);
    const auto subtitleMenuDuration = webUiActive_
                                          ? (subtitleMenuTarget_ > subtitleMenuStartAmount_
                                                 ? kWebUiSubtitleMenuOpenAnimationDuration
                                                 : kWebUiSubtitleMenuCloseAnimationDuration)
                                          : kSubtitleMenuAnimationDuration;
    const MotionCurve subtitleMenuCurve = webUiActive_ &&
                                                  subtitleMenuTarget_ <= subtitleMenuStartAmount_
                                              ? MotionCurve::EaseIn
                                              : MotionCurve::Fluid;
    subtitleMenuAmount_ = AnimatedValue(subtitleMenuStartAmount_,
                                        subtitleMenuTarget_,
                                        subtitleMenuAnimationStartedAt_,
                                        subtitleMenuDuration,
                                        now,
                                        subtitleMenuCurve,
                                        subtitleMenuComplete);
    volumeHoverAmount_ = AnimatedValue(volumeHoverStartAmount_,
                                       volumeHoverTarget_,
                                       volumeHoverAnimationStartedAt_,
                                       kVolumeHoverAnimationDuration,
                                       now,
                                       MotionCurve::Press,
                                       volumeHoverComplete);
    if (sidebarComplete) {
        inspectorCollapseAmount_ = inspectorCollapseTarget_;
    }
    if (hoverComplete) {
        progressHoverAmount_ = progressHoverTarget_;
    }
    if (fullscreenTransportComplete) {
        fullscreenTransportAmount_ = fullscreenTransportTarget_;
    }
    if (subtitleMenuComplete) {
        subtitleMenuAmount_ = subtitleMenuTarget_;
        if (subtitleMenuTarget_ <= 0.001) {
            subtitleMenuOpen_ = false;
        }
    }
    if (volumeHoverComplete) {
        volumeHoverAmount_ = volumeHoverTarget_;
    }

    bool commandAnimationValueChanged = false;
    for (auto& animation : buttonHoverAnimations_) {
        bool complete = true;
        commandAnimationValueChanged =
            UpdateMotionValue(animation, kButtonHoverAnimationDuration, now, MotionCurve::Press, complete) ||
            commandAnimationValueChanged;
        commandAnimationsComplete = commandAnimationsComplete && complete;
    }
    for (auto& animation : buttonPressAnimations_) {
        bool complete = true;
        commandAnimationValueChanged =
            UpdateMotionValue(animation, kButtonPressAnimationDuration, now, MotionCurve::Press, complete) ||
            commandAnimationValueChanged;
        commandAnimationsComplete = commandAnimationsComplete && complete;
    }

    const bool sidebarValueChanged = std::abs(inspectorCollapseAmount_ - previousInspectorCollapseAmount) > 0.0001;
    const bool hoverValueChanged = std::abs(progressHoverAmount_ - previousProgressHoverAmount) > 0.0001;
    const bool fullscreenTransportValueChanged =
        std::abs(fullscreenTransportAmount_ - previousFullscreenTransportAmount) > 0.0001;
    const bool subtitleMenuValueChanged = std::abs(subtitleMenuAmount_ - previousSubtitleMenuAmount) > 0.0001;
    const bool volumeHoverValueChanged = std::abs(volumeHoverAmount_ - previousVolumeHoverAmount) > 0.0001;
    const bool scrollbarFadeActive = ScrollbarFadeActive(now);

    if (sidebarValueChanged || fullscreenTransportValueChanged || subtitleMenuValueChanged) {
        MarkLayoutDirty();
        EnsureLayout();
    }

    const int invalidationPadding = Scale(4);
    const auto noMediaPlaceholderRect = [this](const RECT& surface) {
        if (!HasArea(surface)) {
            return RECT{};
        }
        const bool compactSurface = RectWidth(surface) < Scale(720) || RectHeight(surface) < Scale(280);
        const RECT inner = DeflateRectCopy(surface,
                                           compactSurface ? Scale(18) : Scale(24),
                                           compactSurface ? Scale(16) : Scale(22));
        const int centerX = inner.left + RectWidth(inner) / 2;
        const int centerY = inner.top + RectHeight(inner) / 2;
        const int width = std::min(RectWidth(inner), compactSurface ? Scale(280) : Scale(340));
        const int topOffset = compactSurface ? Scale(88) : Scale(104);
        const int bottomOffset = compactSurface ? Scale(56) : Scale(64);
        return MakeRect(centerX - width / 2, centerY - topOffset, centerX + width / 2, centerY + bottomOffset);
    };

    if (hoverValueChanged) {
        InvalidateTransportArea();
    }
    if (volumeHoverValueChanged || commandAnimationValueChanged) {
        InvalidateTransportArea();
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (fullscreenTransportValueChanged) {
        UpdateVideoHost();
        InvalidateFullscreenOverlay();
    }
    if (subtitleMenuValueChanged) {
        UpdateVideoHost();
        UpdateSubtitleMenuOverlay();
        InvalidateTransportArea();
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (scrollbarFadeActive) {
        InvalidateTransportArea();
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    if (!fullscreen_) {
        const bool inspectorGeometryChanged = !SameRect(previousInspector, inspector_);
        const bool videoGeometryChanged = !SameRect(previousVideoSurface, videoSurface_);
        if (inspectorGeometryChanged || videoGeometryChanged) {
            InvalidateIfVisible(hwnd_, previousInspector, invalidationPadding);
            InvalidateIfVisible(hwnd_, inspector_, invalidationPadding);
            if (videoGeometryChanged) {
                const auto snapshot = controller_.Snapshot();
                InvalidateIfVisible(hwnd_, SidebarEdgeRect(previousVideoSurface, videoSurface_), invalidationPadding);
                if (!snapshot.media.has_value()) {
                    InvalidateIfVisible(hwnd_, noMediaPlaceholderRect(previousVideoSurface), invalidationPadding);
                    InvalidateIfVisible(hwnd_, noMediaPlaceholderRect(videoSurface_), invalidationPadding);
                }
            }
        }
    }

    if (sidebarWasActive && sidebarComplete) {
        UpdateVideoHost();
        if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
            nativeVideoDecoder_ &&
            nativeVideoDecoder_->IsRunning() &&
            d3dRenderer_) {
            NativeVideoFrame frame;
            if (nativeVideoDecoder_->LatestFrame(frame)) {
                d3dRenderer_->Render(frame);
                heldNativeFrame_ = frame;
                heldNativeFrameNeedsPresent_ = false;
                nativeFrameHoldVisible_ = false;
            }
        }
        InvalidateVideoSurface();
    }

    if (sidebarComplete &&
        hoverComplete &&
        fullscreenTransportComplete &&
        subtitleMenuComplete &&
        volumeHoverComplete &&
        commandAnimationsComplete &&
        !scrollbarFadeActive) {
        KillTimer(hwnd_, kUiAnimationTimer);
    } else {
        StartUiAnimationTimer();
    }
}

void MainWindow::SetSubtitleMenuTarget(const bool visible) {
    const RECT oldMenu = subtitleMenu_;
    const double target = visible ? 1.0 : 0.0;
    if (std::abs(subtitleMenuTarget_ - target) < 0.001 &&
        std::abs(subtitleMenuAmount_ - target) < 0.001) {
        return;
    }

    subtitleMenuOpen_ = visible || subtitleMenuAmount_ > 0.001;
    subtitleMenuStartAmount_ = subtitleMenuAmount_;
    subtitleMenuTarget_ = target;
    subtitleMenuAnimationStartedAt_ = std::chrono::steady_clock::now();
    if (!visible) {
        hoveredSubtitleMenuItem_ = -1;
    }
    StartUiAnimationTimer();
    MarkLayoutDirty();
    EnsureLayout();
    UpdateVideoHost();
    UpdateSubtitleMenuOverlay();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    if (!visible && RectWidth(oldMenu) > 0 && RectHeight(oldMenu) > 0) {
        InvalidateRect(hwnd_, &oldMenu, FALSE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::HideSubtitleMenu() {
    if (subtitleMenuTarget_ <= 0.0 && subtitleMenuAmount_ <= 0.0) {
        return;
    }
    SetSubtitleMenuTarget(false);
}

bool MainWindow::IsPointInteractive(const POINT point) const {
    if (HitButton(point) >= 0) {
        return true;
    }
    if (HitInspectorPathItem(point) >= 0) {
        return true;
    }
    if (ContainsPoint(VolumeSliderHitRect(), point)) {
        return true;
    }
    if (IsPointInSubtitleMenu(point)) {
        return true;
    }
    if (settingsScrollMax_ > 0 && ContainsPoint(settingsScrollThumb_, point)) {
        return true;
    }
    if (IsHdrToneCurveVisible()) {
        RECT hitRect = hdrToneCurvePlot_;
        InflateRect(&hitRect, Scale(18), Scale(18));
        if (ContainsPoint(hitRect, point)) {
            return true;
        }
    }
    const auto snapshot = controller_.Snapshot();
    return snapshot.media.has_value() &&
           snapshot.media->duration.count() > 0 &&
           ContainsPoint(ProgressHitRect(), point);
}

void MainWindow::SetProgressHover(const bool hovered) {
    if (progressHovered_ == hovered && progressHoverTarget_ == (hovered ? 1.0 : 0.0)) {
        return;
    }

    progressHovered_ = hovered;
    progressHoverStartAmount_ = progressHoverAmount_;
    progressHoverTarget_ = hovered ? 1.0 : 0.0;
    progressHoverAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    InvalidateTransportArea();
}

void MainWindow::ToggleSidebar() {
    const RECT previousVideoSurface = videoSurface_;
    const RECT previousInspector = inspector_;
    const RECT previousTopBar = topBar_;
    const auto snapshot = controller_.Snapshot();
    inspectorCollapsed_ = !inspectorCollapsed_;
    inspectorCollapseStartAmount_ = inspectorCollapseAmount_;
    inspectorCollapseTarget_ = inspectorCollapsed_ ? 1.0 : 0.0;
    inspectorAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    MarkLayoutDirty();
    EnsureLayout();
    const int invalidationPadding = Scale(4);
    InvalidateIfVisible(hwnd_, previousTopBar, invalidationPadding);
    InvalidateIfVisible(hwnd_, topBar_, invalidationPadding);
    InvalidateIfVisible(hwnd_, previousInspector, invalidationPadding);
    InvalidateIfVisible(hwnd_, inspector_, invalidationPadding);
    InvalidateIfVisible(hwnd_, SidebarEdgeRect(previousVideoSurface, videoSurface_), invalidationPadding);
    if (!snapshot.media.has_value()) {
        const auto noMediaPlaceholderRect = [this](const RECT& surface) {
            if (!HasArea(surface)) {
                return RECT{};
            }
            const bool compactSurface = RectWidth(surface) < Scale(720) || RectHeight(surface) < Scale(280);
            const RECT inner = DeflateRectCopy(surface,
                                               compactSurface ? Scale(18) : Scale(24),
                                               compactSurface ? Scale(16) : Scale(22));
            const int centerX = inner.left + RectWidth(inner) / 2;
            const int centerY = inner.top + RectHeight(inner) / 2;
            const int width = std::min(RectWidth(inner), compactSurface ? Scale(280) : Scale(340));
            const int topOffset = compactSurface ? Scale(88) : Scale(104);
            const int bottomOffset = compactSurface ? Scale(56) : Scale(64);
            return MakeRect(centerX - width / 2, centerY - topOffset, centerX + width / 2, centerY + bottomOffset);
        };
        InvalidateIfVisible(hwnd_, noMediaPlaceholderRect(previousVideoSurface), invalidationPadding);
        InvalidateIfVisible(hwnd_, noMediaPlaceholderRect(videoSurface_), invalidationPadding);
    }
    PostWebUiState();
}

bool MainWindow::ShouldShowFullscreenTransport(const PlaybackSessionSnapshot&) const {
    if (!fullscreen_) {
        return true;
    }
    return draggingProgress_ ||
           draggingVolume_ ||
           fullscreenTransportTarget_ > 0.0 ||
           fullscreenTransportAmount_ > 0.01;
}

bool MainWindow::IsFullscreenTransportActivationPoint(const POINT point) const {
    if (!fullscreen_) {
        return false;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!ContainsPoint(client, point)) {
        return false;
    }

    return point.y >= client.bottom - Scale(kFullscreenTransportActivationHeight);
}

void MainWindow::UpdateFullscreenTransportCursorPolling() {
    if (!fullscreen_) {
        hasLastFullscreenCursorClient_ = false;
        return;
    }

    POINT point{};
    if (!GetCursorPos(&point) || !ScreenToClient(hwnd_, &point)) {
        return;
    }

    const bool moved = !hasLastFullscreenCursorClient_ ||
                       point.x != lastFullscreenCursorClient_.x ||
                       point.y != lastFullscreenCursorClient_.y;
    lastFullscreenCursorClient_ = point;
    hasLastFullscreenCursorClient_ = true;

    if (moved &&
        !ShouldShowFullscreenTransport(controller_.Snapshot()) &&
        IsFullscreenTransportActivationPoint(point)) {
        ShowFullscreenTransport(L"cursor_poll_activation");
    }
}

std::wstring MainWindow::FullscreenTransportDebugState(const wchar_t* reason) const {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(3)
           << L" reason=" << (reason ? reason : L"unspecified")
           << L" target=" << fullscreenTransportTarget_
           << L" amount=" << fullscreenTransportAmount_;
    if (fullscreenTransportLastShownAt_.time_since_epoch().count() != 0) {
        const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - fullscreenTransportLastShownAt_).count();
        stream << L" idle_ms=" << idleMs;
    } else {
        stream << L" idle_ms=none";
    }
    if (hasLastFullscreenCursorClient_) {
        const bool activation = IsFullscreenTransportActivationPoint(lastFullscreenCursorClient_);
        const bool transportHit = ContainsPoint(transportBar_, lastFullscreenCursorClient_);
        const bool menuHit = IsPointInSubtitleMenu(lastFullscreenCursorClient_);
        stream << L" cursor=" << lastFullscreenCursorClient_.x << L"," << lastFullscreenCursorClient_.y
               << L" activation=" << (activation ? L"true" : L"false")
               << L" transport_hit=" << (transportHit ? L"true" : L"false")
               << L" menu_hit=" << (menuHit ? L"true" : L"false");
    } else {
        stream << L" cursor=none";
    }
    stream << L" dragging_progress=" << (draggingProgress_ ? L"true" : L"false")
           << L" dragging_volume=" << (draggingVolume_ ? L"true" : L"false")
           << L" subtitle_menu=" << ((subtitleMenuTarget_ > 0.0 || subtitleMenuAmount_ > 0.01) ? L"true" : L"false");
    return stream.str();
}

void MainWindow::ShowFullscreenTransport(const wchar_t* reason) {
    if (!fullscreen_) {
        return;
    }

    if (fullscreenTransportTarget_ <= 0.001 && fullscreenTransportAmount_ <= 0.001) {
        LogApp(LogLevel::Debug, L"fullscreen_transport show" + FullscreenTransportDebugState(reason));
    }
    SetFullscreenTransportTarget(true);
    fullscreenTransportLastShownAt_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kFullscreenChromeHideTimer, 120, nullptr);
}

void MainWindow::HideFullscreenTransportIfIdle() {
    if (!fullscreen_) {
        KillTimer(hwnd_, kFullscreenChromeHideTimer);
        return;
    }

    UpdateFullscreenTransportCursorPolling();

    if (draggingProgress_ || draggingVolume_) {
        if (fullscreenTransportTarget_ <= 0.001) {
            LogApp(LogLevel::Debug,
                   L"fullscreen_transport keep" +
                       FullscreenTransportDebugState(draggingProgress_ ? L"dragging_progress" : L"dragging_volume"));
        }
        SetFullscreenTransportTarget(true);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (subtitleMenuTarget_ > 0.0 || subtitleMenuAmount_ > 0.01) {
        if (fullscreenTransportTarget_ <= 0.001) {
            LogApp(LogLevel::Debug, L"fullscreen_transport keep" + FullscreenTransportDebugState(L"subtitle_menu"));
        }
        SetFullscreenTransportTarget(true);
        fullscreenTransportLastShownAt_ = now;
        return;
    }

    if (fullscreenTransportLastShownAt_.time_since_epoch().count() != 0 &&
        now - fullscreenTransportLastShownAt_ < kFullscreenTransportHideDelay) {
        return;
    }

    if (fullscreenTransportTarget_ > 0.001 || fullscreenTransportAmount_ > 0.001) {
        LogApp(LogLevel::Debug, L"fullscreen_transport hide" + FullscreenTransportDebugState(L"idle_timeout"));
    }
    SetFullscreenTransportTarget(false);
    ClearButtonHoverTargets();
    hoveredButton_ = -1;
    SetVolumeSliderHover(false);
    SetProgressHover(false);
}

void MainWindow::SetFullscreenTransportTarget(const bool visible) {
    fullscreenTransportVisible_ = visible;
    const double target = visible ? 1.0 : 0.0;
    if (std::abs(fullscreenTransportTarget_ - target) < 0.001) {
        return;
    }

    fullscreenTransportStartAmount_ = fullscreenTransportAmount_;
    fullscreenTransportTarget_ = target;
    fullscreenTransportAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    MarkLayoutDirty();
    EnsureLayout();
    UpdateVideoHost();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::MarkSettingsScrollbarActive() {
    settingsScrollLastActiveAt_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kScrollbarAutoHideTimer, kScrollbarAutoHideTimerMs, nullptr);
    StartUiAnimationTimer();
}

void MainWindow::MarkSubtitleMenuScrollbarActive() {
    subtitleMenuScrollLastActiveAt_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kScrollbarAutoHideTimer, kScrollbarAutoHideTimerMs, nullptr);
    StartUiAnimationTimer();
}

bool MainWindow::ScrollbarFadeActive(const std::chrono::steady_clock::time_point now) const {
    const auto active = [now](const std::chrono::steady_clock::time_point lastActiveAt) {
        return lastActiveAt.time_since_epoch().count() != 0 &&
               now - lastActiveAt < kScrollbarVisibleDuration + kScrollbarFadeDuration;
    };
    return draggingSettingsScrollThumb_ ||
           active(settingsScrollLastActiveAt_) ||
           active(subtitleMenuScrollLastActiveAt_);
}

void MainWindow::SetTemporaryPlaybackRate(const double rate) {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value()) {
        audioPlayer_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
        return;
    }

    const double clamped = std::clamp(rate, 0.25, 4.0);
    if (std::abs(snapshot.playbackRate - clamped) < 0.001) {
        temporaryRateActive_ = clamped > 1.01;
        audioPlayer_.SetPlaybackRate(clamped);
        return;
    }

    controller_.SetPlaybackRate(clamped);
    audioPlayer_.SetPlaybackRate(clamped);
    temporaryRateActive_ = clamped > 1.01;
    InvalidateTransportArea();
}

std::wstring MainWindow::RuntimeLabel() const {
    switch (backend_) {
    case PlaybackBackend::NativeFfmpegD3D11: return kNativeFfmpegD3D11RuntimeLabel;
    case PlaybackBackend::EmbeddedFfplay: return kExternalPlaybackRuntimeLabel;
    case PlaybackBackend::RawFrameBridge: return kInternalPlaybackRuntimeLabel;
    }
    return kNativeFfmpegD3D11RuntimeLabel;
}

std::wstring MainWindow::RuntimeShortLabel() const {
    switch (backend_) {
    case PlaybackBackend::NativeFfmpegD3D11: return L"Native FFmpeg";
    case PlaybackBackend::EmbeddedFfplay: return L"External FFplay";
    case PlaybackBackend::RawFrameBridge: return L"Internal FFmpeg";
    }
    return L"Native FFmpeg";
}

void MainWindow::SetPlaybackTimer(const bool playing) {
    if (!hwnd_) {
        return;
    }

    if (playbackTimerQueueTimer_) {
        DeleteTimerQueueTimer(nullptr, playbackTimerQueueTimer_, INVALID_HANDLE_VALUE);
        playbackTimerQueueTimer_ = nullptr;
    }
    KillTimer(hwnd_, kPlaybackTimer);

    if (playing) {
        if (!CreateTimerQueueTimer(&playbackTimerQueueTimer_,
                                   nullptr,
                                   &MainWindow::PlaybackTimerQueueProc,
                                   this,
                                   kPlayingPlaybackTimerMs,
                                   kPlayingPlaybackTimerMs,
                                   WT_EXECUTEDEFAULT)) {
            playbackTimerQueueTimer_ = nullptr;
            SetTimer(hwnd_, kPlaybackTimer, kPlayingPlaybackTimerMs, nullptr);
        }
    } else {
        SetTimer(hwnd_, kPlaybackTimer, kIdlePlaybackTimerMs, nullptr);
    }
}

void MainWindow::OnPlaybackTimerTick() {
    bool nativeBuffering = false;
    bool nativeDecoderClockActive = false;
    const bool runtimeTransition = RuntimeStopInProgress() || deferredRuntimeStart_;
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        nativeVideoDecoder_) {
        const auto current = controller_.Snapshot();
        if (current.state == PlaybackState::Playing &&
            current.media.has_value() &&
            current.media->hasVideo) {
            if (!nativeVideoDecoder_->IsRunning()) {
                const auto remaining =
                    current.media->duration.count() > 0
                        ? current.media->duration - current.position
                        : std::chrono::milliseconds{0};
                if (runtimeTransition) {
                    nativeBuffering = true;
                } else if (remaining > std::chrono::seconds{2} || current.media->duration.count() <= 0) {
                    FailPlaybackRuntime(L"Native video decoder stopped before playback completed at " +
                                        FormatTimecode(current.position));
                    return;
                }
            } else {
                const auto stats = nativeVideoDecoder_->Stats();
                nativeBuffering = stats.buffering;
                if (nativeBuffering) {
                    controller_.SyncClock(stats.clockPosition);
                    nativeDecoderClockActive = true;
                    if (CheckNativeBufferingWatchdog(current, stats)) {
                        return;
                    }
                } else {
                    ResetNativeBufferingWatchdog();
                    if (nativeSeekPrerollHoldingAudio_ && !stats.seekRecoveryAudioHandoffReady) {
                        controller_.SyncClock(stats.clockPosition);
                        nativeDecoderClockActive = true;
                    } else {
                        ResumeNativeSeekPrerollAudio(stats.clockPosition);
                    }
                }
            }
        }
    }
    if (!nativeBuffering && !runtimeTransition && !nativeDecoderClockActive) {
        controller_.UpdateClock();
    }
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state != PlaybackState::Playing) {
        if (runtimeStopAsyncInProgress_.load()) {
            PostWebUiState(false);
            return;
        }
        SetTemporaryPlaybackRate(1.0);
        if (snapshot.state == PlaybackState::Paused &&
            backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
            nativeVideoDecoder_ &&
            nativeVideoDecoder_->IsRunning()) {
            MaybeLogNativeSchedulerStats(nativeVideoDecoder_->Stats());
            InvalidateRect(hwnd_, &transportBar_, FALSE);
            InvalidateFullscreenOverlay();
            UpdateBufferingOverlay();
            PostWebUiState(false);
            return;
        }
        // Paused keeps the freeze frame captured by PausePlayback; other
        // stopped states clear the runtime frame as before.
        const bool keepFrame = (snapshot.state == PlaybackState::Paused);
        if (RuntimeBackendsRunning()) {
            StopRuntimeAsync(keepFrame ? false : true);
        } else {
            FinishRuntimeStopVisuals(keepFrame ? false : true, true);
        }
        SetPlaybackTimer(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        PostWebUiState();
    } else {
        if (snapshot.media.has_value()) {
            const auto remaining =
                snapshot.media->duration.count() > 0
                    ? snapshot.media->duration - snapshot.position
                    : std::chrono::milliseconds{0};
            const bool beforeEnd = remaining > std::chrono::seconds{2} ||
                                   snapshot.media->duration.count() <= 0;
            if (beforeEnd && !runtimeTransition) {
                if (backend_ == PlaybackBackend::EmbeddedFfplay &&
                    !playbackPlayer_.IsRunning()) {
                    FailPlaybackRuntime(L"External FFplay process exited before playback completed at " +
                                        FormatTimecode(snapshot.position));
                    return;
                }
                if (backend_ == PlaybackBackend::RawFrameBridge &&
                    snapshot.media->hasVideo &&
                    !videoDecoder_.IsRunning()) {
                    FailPlaybackRuntime(L"Internal video decoder stopped before playback completed at " +
                                        FormatTimecode(snapshot.position));
                    return;
                }

                const auto runtimeSettings = controller_.Settings();
                const bool audioRequired =
                    snapshot.media->hasAudio &&
                    runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff;
                const bool audioOwnedByNativeVideoDemuxer =
                    backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                    snapshot.media->hasVideo &&
                    snapshot.media->hasAudio &&
                    IsNetworkMediaPath(snapshot.media->path);
                if (audioRequired &&
                    backend_ != PlaybackBackend::EmbeddedFfplay &&
                    !audioOwnedByNativeVideoDemuxer &&
                    !audioPlayer_.IsRunning()) {
                    FailPlaybackRuntime(L"Audio renderer stopped before playback completed at " +
                                        FormatTimecode(snapshot.position));
                    return;
                }
            }
        }
        if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
            nativeVideoDecoder_ &&
            nativeVideoDecoder_->IsRunning() &&
            snapshot.media.has_value() &&
            snapshot.media->hasVideo) {
            const auto stats = nativeVideoDecoder_->Stats();
            MaybeLogNativeSchedulerStats(stats);
            if (webUiActive_ && webUiPlayerRouteActive_) {
                UpdateVideoHost();
            }
            UpdateBufferingOverlay(&stats);
        }
        RenderPlaybackTick(snapshot);
        PostWebUiState(false);
    }
}

void MainWindow::InvalidatePlaybackAreas() const {
    InvalidateRect(hwnd_, &videoSurface_, FALSE);
    InvalidateRect(hwnd_, &transportBar_, FALSE);
}

void MainWindow::InvalidateVideoSurface() const {
    InvalidateRect(hwnd_, &videoSurface_, FALSE);
}

void MainWindow::InvalidateTransportArea() const {
    if (subtitleMenuOverlay_ && IsWindowVisible(subtitleMenuOverlay_)) {
        InvalidateRect(subtitleMenuOverlay_, nullptr, FALSE);
    }
    if (transportOverlay_ && IsWindowVisible(transportOverlay_)) {
        InvalidateRect(transportOverlay_, nullptr, FALSE);
        return;
    }
    if (fullscreenOverlay_ && IsWindowVisible(fullscreenOverlay_)) {
        InvalidateFullscreenOverlay();
        return;
    }
    if (RectWidth(transportBar_) > 0 && RectHeight(transportBar_) > 0) {
        InvalidateRect(hwnd_, &transportBar_, FALSE);
    }
}

void MainWindow::InvalidateFullscreenOverlay() const {
    if (subtitleMenuOverlay_ && IsWindowVisible(subtitleMenuOverlay_)) {
        InvalidateRect(subtitleMenuOverlay_, nullptr, FALSE);
    }
    if (fullscreenOverlay_ && IsWindowVisible(fullscreenOverlay_)) {
        InvalidateRect(fullscreenOverlay_, nullptr, FALSE);
    }
}

void MainWindow::InvalidateHdrToneCurveEditor() const {
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (hdrToneCurveWindow_ && IsWindowVisible(hdrToneCurveWindow_)) {
        InvalidateRect(hdrToneCurveWindow_, nullptr, FALSE);
    }
}

const CapabilityReport& MainWindow::CachedCapabilities() {
    if (!capabilitiesCached_) {
        cachedCapabilities_ = controller_.CollectCapabilityReport();
        capabilitiesCached_ = true;
        LogApp(LogLevel::Debug, L"capability report cached");
    }
    return cachedCapabilities_;
}

void MainWindow::RefreshCapabilityCache() {
    cachedCapabilities_ = controller_.CollectCapabilityReport();
    capabilitiesCached_ = true;
    LogApp(LogLevel::Debug, L"capability report refreshed");
}

bool MainWindow::CurrentMediaHasHdrControls() const {
    return MediaHasHdrSignal(controller_.Snapshot().media);
}

bool MainWindow::CurrentMediaHasCmv4Control() const {
    return MediaIsDolbyVision(controller_.Snapshot().media);
}

bool MainWindow::CurrentCmv4ControlEnabled(const anvil::playback::PlayerSettings& settings) const {
    (void)settings;
    return CurrentMediaHasCmv4Control();
}

bool MainWindow::HdrToneCurveAvailable(const anvil::playback::PlayerSettings& settings) const {
    if (!CurrentMediaHasHdrControls() || !settings.video.dolbyVisionHdrOutput) {
        return false;
    }
    return !CurrentMediaHasCmv4Control() || !WantsCmv4Approx(settings.video);
}

bool MainWindow::Cmv4ApproxActiveForPlayback(const PlaybackSessionSnapshot& snapshot,
                                             const anvil::playback::VideoSettings& settings) const {
    return MediaIsDolbyVision(snapshot.media) &&
           WantsCmv4Approx(settings);
}

bool MainWindow::ApplyNativeColorSettingsLive(const PlaybackSessionSnapshot& snapshot) {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        !snapshot.media.has_value() ||
        !snapshot.media->hasVideo ||
        !d3dRenderer_) {
        return false;
    }

    const auto settings = controller_.Settings();
    d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
    d3dRenderer_->ResetRenderStats();
    lastNativeStatsLog_ = {};

    if (snapshot.state == PlaybackState::Playing) {
        LogApp(LogLevel::Debug, L"native color settings queued live");
        return true;
    }

    NativeVideoFrame frame;
    const bool haveLatestFrame = nativeVideoDecoder_ &&
                                 nativeVideoDecoder_->LatestFrame(frame) &&
                                 frame.HasContent();
    if (haveLatestFrame) {
        d3dRenderer_->Render(frame);
        heldNativeFrame_ = frame;
        heldNativeFrameNeedsPresent_ = false;
        if (snapshot.state == PlaybackState::Paused) {
            nativeFrameHoldVisible_ = true;
            pendingPausedFrameRefresh_ = false;
        } else {
            nativeFrameHoldVisible_ = false;
        }
    } else if (heldNativeFrame_.has_value() && heldNativeFrame_->HasContent()) {
        RenderHeldNativeFrame();
        if (snapshot.state == PlaybackState::Paused) {
            nativeFrameHoldVisible_ = true;
            pendingPausedFrameRefresh_ = false;
        }
    } else {
        LogApp(LogLevel::Debug, L"native color settings live apply has no frame to repaint");
        return false;
    }

    LogApp(LogLevel::Debug, L"native color settings applied live");
    return true;
}

void MainWindow::ScheduleNativeColorSettingsRefresh(const bool requiresDecoderRefresh) {
    if (!hwnd_) {
        return;
    }
    ++nativeColorSettingsRefreshSerial_;
    nativeColorSettingsRefreshRequiresDecoderRefresh_ = requiresDecoderRefresh;
    PostMessageW(hwnd_, kNativeColorSettingsRefreshMessage, nativeColorSettingsRefreshSerial_, 0);
}

void MainWindow::ApplyNativeColorSettingsRefresh(const bool requiresDecoderRefresh) {
    const auto snapshot = controller_.Snapshot();
    const bool nativeVideo = backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                             snapshot.media.has_value() &&
                             snapshot.media->hasVideo;
    bool handledLive = false;
    if (nativeVideo && !requiresDecoderRefresh) {
        handledLive = ApplyNativeColorSettingsLive(snapshot);
    }
    if (!handledLive) {
        if (snapshot.state == PlaybackState::Paused && nativeVideo) {
            RefreshPausedNativeFrame(snapshot, requiresDecoderRefresh);
        } else {
            RestartPlaybackIfPlaying(false);
        }
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateHdrToneCurveEditor();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::NativeHdrOutputToggleRequiresDecoderRestart(const PlaybackSessionSnapshot& snapshot) const {
    (void)snapshot;
    // The native DV path keeps decoder-side libplacebo output stable as
    // BT.2020/PQ; the button only changes the renderer's final SDR/HDR output.
    return false;
}

bool MainWindow::NativeCmv4ToggleRequiresDecoderRestart(const PlaybackSessionSnapshot& snapshot) const {
    return backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
           snapshot.media.has_value() &&
           snapshot.media->hasVideo &&
           MediaHasDolbyVisionEnhancementStream(snapshot.media);
}

bool MainWindow::NativeCmv4ToggleNeedsDecoderRefresh(const PlaybackSessionSnapshot& snapshot,
                                                     const anvil::playback::VideoSettings& settings) const {
    if (!NativeCmv4ToggleRequiresDecoderRestart(snapshot) || !WantsCmv4Approx(settings)) {
        return false;
    }
    if (heldNativeFrame_.has_value() && heldNativeFrame_->HasEnhancementYuv()) {
        return false;
    }
    return !nativeVideoDecoder_ ||
           !nativeVideoDecoder_->IsRunning() ||
           !nativeVideoDecoder_->DolbyVisionEnhancementDecodeEnabled();
}

int MainWindow::SettingsVideoFieldCount() const {
    int count = 6;
    if (CurrentMediaHasHdrControls()) {
        ++count;
    }
    if (CurrentMediaHasCmv4Control()) {
        ++count;
    }
    return count;
}

void MainWindow::ApplyDefaultHdrControlsForCurrentMedia() {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value()) {
        return;
    }

    auto settings = controller_.Settings();
    const bool windowsHdrEnabled = CachedCapabilities().display.hdrEnabled;
    const bool hdrControlVisible = MediaHasHdrSignal(snapshot.media);
    const bool defaultHdrOutput = hdrControlVisible && windowsHdrEnabled;
    const bool defaultEnhanced = MediaIsDolbyVision(snapshot.media);
    bool changed = false;

    if (settings.video.dolbyVisionHdrOutput != defaultHdrOutput) {
        settings.video.dolbyVisionHdrOutput = defaultHdrOutput;
        changed = true;
    }
    if (settings.video.dolbyVisionCmv4Approx != defaultEnhanced) {
        settings.video.dolbyVisionCmv4Approx = defaultEnhanced;
        changed = true;
    }

    if (changed) {
        controller_.ApplySettings(settings);
    }
    if (!HdrToneCurveAvailable(settings)) {
        HideHdrToneCurveWindow();
    }

    LogApp(LogLevel::Info,
           L"media hdr controls hdr=" +
               std::wstring(hdrControlVisible ? (settings.video.dolbyVisionHdrOutput ? L"on" : L"off") : L"hidden") +
               L" enhanced=" +
               std::wstring(MediaIsDolbyVision(snapshot.media) ? (settings.video.dolbyVisionCmv4Approx ? L"on" : L"off") : L"hidden") +
               L" curve=" +
               std::wstring(HdrToneCurveAvailable(settings) ? L"visible" : L"hidden") +
               L" windows_hdr=" +
               std::wstring(windowsHdrEnabled ? L"on" : L"off"));
}

void MainWindow::RenderPlaybackTick(const PlaybackSessionSnapshot& snapshot, const bool forceRefresh) {
    if (!hwnd_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!forceRefresh &&
        lastPlaybackUiRefreshAt_.time_since_epoch().count() != 0 &&
        now - lastPlaybackUiRefreshAt_ < std::chrono::milliseconds{kPlayingPlaybackTimerMs}) {
        return;
    }
    lastPlaybackUiRefreshAt_ = now;
    InvalidateTransportArea();
    if (!snapshot.media.has_value() || !snapshot.media->hasVideo) {
        InvalidateVideoSurface();
    }
}

int MainWindow::Scale(const int value) const {
    return MulDiv(value, static_cast<int>(dpi_), 96);
}

RECT MainWindow::PlaybackSurfaceBounds() const {
    const RECT surface = (RectWidth(playbackSurface_) > 0 && RectHeight(playbackSurface_) > 0)
                             ? playbackSurface_
                             : videoSurface_;
    if (RectWidth(surface) <= 0 || RectHeight(surface) <= 0) {
        return RECT{};
    }
    if (fullscreen_) {
        return surface;
    }
    return DeflateRectCopy(surface, Scale(2), Scale(2));
}

void MainWindow::ApplyWindowChrome() const {
    const BOOL dark = TRUE;
    const int corner = kDwmCornerRound;
    const COLORREF border = palette_.border;
    const COLORREF caption = palette_.background;
    const COLORREF text = palette_.text;
    DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd_, kDwmWindowCornerPreference, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd_, kDwmBorderColor, &border, sizeof(border));
    DwmSetWindowAttribute(hwnd_, kDwmCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd_, kDwmTextColor, &text, sizeof(text));
}

void MainWindow::OpenFileDialog() {
    std::vector<wchar_t> filePath(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = filePath.data();
    dialog.nMaxFile = static_cast<DWORD>(filePath.size());
    dialog.lpstrFilter = L"Media Files\0*.mp4;*.mkv;*.mov;*.m2ts;*.ts;*.webm;*.avi\0All Files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&dialog)) {
        OpenPath(filePath.data());
    }
}

void MainWindow::OpenLocalFolderDialog() {
    const auto postMessage = [this](const std::wstring& json) {
        if (webUiActive_ && webUiHost_ && webUiHost_->Ready()) {
            webUiHost_->PostJson(json);
        }
    };

    BROWSEINFOW dialog{};
    dialog.hwndOwner = hwnd_;
    dialog.lpszTitle = L"选择本地媒体文件夹";
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&dialog);
    if (!pidl) {
        postMessage(L"{\"type\":\"localFolderPickCancelled\"}");
        return;
    }

    wchar_t folderPath[MAX_PATH]{};
    const BOOL hasPath = SHGetPathFromIDListW(pidl, folderPath);
    CoTaskMemFree(pidl);

    if (!hasPath || folderPath[0] == L'\0') {
        postMessage(L"{\"type\":\"localFolderPickFailed\",\"message\":\"无法读取所选文件夹路径\"}");
        return;
    }

    const auto folder = NormalizeListPath(folderPath);
    std::error_code error;
    if (!std::filesystem::exists(folder, error) || !std::filesystem::is_directory(folder, error)) {
        postMessage(L"{\"type\":\"localFolderPickFailed\",\"message\":\"所选路径不是可读取的文件夹\"}");
        return;
    }

    postMessage(LocalFolderPickedJson(folder, {}, false, true));

    StartLocalFolderScanAsync(hwnd_, folder);
}

void MainWindow::OpenSubtitleFileDialog() {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value()) {
        return;
    }

    std::vector<wchar_t> filePath(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = filePath.data();
    dialog.nMaxFile = static_cast<DWORD>(filePath.size());
    dialog.lpstrFilter = L"Subtitle Files\0*.srt;*.ass;*.ssa;*.vtt\0All Files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    auto settings = controller_.Settings();
    settings.subtitles.externalSubtitlePath = filePath.data();
    settings.subtitles.selectedTrackIndex = anvil::playback::kSubtitleTrackAuto;
    controller_.ApplySettings(settings);
    LogApp(LogLevel::Info, L"subtitle external=" + settings.subtitles.externalSubtitlePath.filename().wstring());

    const auto updated = controller_.Snapshot();
    if (updated.state == PlaybackState::Paused &&
        updated.media.has_value() &&
        updated.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(updated, true);
    } else {
        RestartPlaybackIfPlaying();
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OpenDanmakuFileDialog() {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value()) {
        return;
    }

    std::vector<wchar_t> filePath(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = filePath.data();
    dialog.nMaxFile = static_cast<DWORD>(filePath.size());
    dialog.lpstrFilter = L"Danmaku Files\0*.xml;*.json;*.ass\0All Files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    auto settings = controller_.Settings();
    settings.danmaku.externalDanmakuPath = filePath.data();
    settings.danmaku.enabled = true;
    controller_.ApplySettings(settings);
    LogApp(LogLevel::Info, L"danmaku external=" + settings.danmaku.externalDanmakuPath.filename().wstring());

    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StopRuntime(const bool clearVideoFrame) {
    WaitForAsyncRuntimeStop();
    StopRuntimeBackends();
    FinishRuntimeStopVisuals(clearVideoFrame, true);
}

void MainWindow::StopRuntimeAsync(const bool clearVideoFrame) {
    if (runtimeStopAsyncInProgress_.load()) {
        runtimeStopAsyncClearFrame_.store(runtimeStopAsyncClearFrame_.load() || clearVideoFrame);
        FinishRuntimeStopVisuals(clearVideoFrame, false);
        LogApp(LogLevel::Debug, L"runtime async stop already in progress");
        return;
    }

    WaitForAsyncRuntimeStop();
    runtimeStopAsyncClearFrame_.store(clearVideoFrame);
    runtimeStopAsyncInProgress_.store(true);
    FinishRuntimeStopVisuals(clearVideoFrame, false);
    LogApp(LogLevel::Debug, L"runtime async stop begin");
    runtimeStopThread_ = std::thread([this]() {
        StopRuntimeBackends();
        if (hwnd_) {
            PostMessageW(hwnd_, kRuntimeStopCompleteMessage, 0, 0);
        }
    });
}

void MainWindow::StopRuntimeBackends() {
    nativeSeekPrerollHoldingAudio_ = false;
    ResetNativeBufferingWatchdog();
    playbackPlayer_.Stop();
    videoDecoder_.Stop();
    if (nativeVideoDecoder_) {
        nativeVideoDecoder_->Stop();
    }
    audioPlayer_.Stop();
}

void MainWindow::ResumeNativeSeekPrerollAudio(const std::chrono::milliseconds position) {
    if (!nativeSeekPrerollHoldingAudio_) {
        return;
    }
    nativeSeekPrerollHoldingAudio_ = false;

    const auto snapshot = controller_.Snapshot();
    if (snapshot.state != PlaybackState::Playing ||
        !snapshot.media.has_value() ||
        !snapshot.media->hasAudio ||
        !audioPlayer_.IsRunning()) {
        return;
    }

    const auto settings = controller_.Settings();
    if (settings.audio.selectedTrackIndex == anvil::playback::kAudioTrackOff) {
        return;
    }

    const auto resumePosition = position.count() >= 0 ? position : snapshot.position;
    audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
    const bool resumed = audioPlayer_.ResumePacketStream();
    LogApp(resumed ? LogLevel::Debug : LogLevel::Warning,
           L"native seek preroll resume audio=" + std::wstring(resumed ? L"true" : L"false") +
               L" position=" + FormatTimecode(resumePosition));
}

void MainWindow::FinishRuntimeStopVisuals(const bool clearVideoFrame, const bool clearDecoderFrames) {
    if (clearVideoFrame) {
        if (clearDecoderFrames) {
            videoDecoder_.ClearFrame();
            if (nativeVideoDecoder_) nativeVideoDecoder_->ClearFrame();
        }
        heldNativeFrame_.reset();
        nativeFrameHoldVisible_ = false;
        pendingPausedFrameRefresh_ = false;
        heldNativeFrameNeedsPresent_ = false;
    } else if (backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
        heldNativeFrameNeedsPresent_ = heldNativeFrame_.has_value() &&
                                       heldNativeFrame_->HasPixels();
    }
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 && clearVideoFrame) {
        if (d3dRenderer_) d3dRenderer_->Clear();
        if (videoHost_) {
            ShowWindow(videoHost_, SW_HIDE);
        }
    }
    UpdateBufferingOverlay();
}

void MainWindow::WaitForAsyncRuntimeStop() {
    if (runtimeStopThread_.joinable()) {
        LogApp(LogLevel::Debug, L"runtime async stop wait");
        runtimeStopThread_.join();
    }
    if (runtimeStopAsyncInProgress_.exchange(false)) {
        LogApp(LogLevel::Debug, L"runtime async stop complete");
        FinishRuntimeStopVisuals(runtimeStopAsyncClearFrame_.load(), true);
    }
}

bool MainWindow::RuntimeStopInProgress() const {
    return runtimeStopAsyncInProgress_.load();
}

bool MainWindow::RuntimeBackendsRunning() const {
    return playbackPlayer_.IsRunning() ||
           videoDecoder_.IsRunning() ||
           audioPlayer_.IsRunning() ||
           (nativeVideoDecoder_ && nativeVideoDecoder_->IsRunning());
}

void MainWindow::ClearDeferredRuntimeStart() {
    deferredRuntimeStart_ = false;
    deferredRuntimeRestart_ = false;
    deferredRuntimeWaitForPreroll_ = false;
}

void MainWindow::QueueDeferredRuntimeStart(const bool restart, const bool waitForPreroll) {
    if (!deferredRuntimeStart_) {
        deferredRuntimeWaitForPreroll_ = waitForPreroll;
    } else {
        deferredRuntimeWaitForPreroll_ = deferredRuntimeWaitForPreroll_ && waitForPreroll;
    }
    deferredRuntimeStart_ = true;
    deferredRuntimeRestart_ = deferredRuntimeRestart_ || restart;
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
}

void MainWindow::QueuePausedNativeFrameRefresh(const bool forceDecoderRestart) {
    deferredPausedFrameRefresh_ = true;
    deferredPausedFrameRefreshForceRestart_ =
        deferredPausedFrameRefreshForceRestart_ || forceDecoderRestart;
}

void MainWindow::RequestRuntimeStart(const bool restart, const bool waitForPreroll) {
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state != PlaybackState::Playing ||
        !snapshot.media.has_value() ||
        (!snapshot.media->hasVideo && !snapshot.media->hasAudio)) {
        ClearDeferredRuntimeStart();
        return;
    }

    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    if (RuntimeStopInProgress()) {
        QueueDeferredRuntimeStart(restart, waitForPreroll);
        LogApp(LogLevel::Debug,
               L"runtime start deferred while stop is pending restart=" +
                   std::wstring(restart ? L"true" : L"false"));
        SetPlaybackTimer(true);
        return;
    }

    if (RuntimeBackendsRunning()) {
        QueueDeferredRuntimeStart(restart, waitForPreroll);
        LogApp(LogLevel::Debug,
               L"runtime restart queued restart=" +
                   std::wstring(restart ? L"true" : L"false"));
        StopRuntimeAsync(false);
        SetPlaybackTimer(true);
        return;
    }

    if (StartRuntime(snapshot, restart, waitForPreroll)) {
        SetPlaybackTimer(true);
    }
}

void MainWindow::ContinueRuntimeAfterAsyncStop() {
    const bool startRequested = deferredRuntimeStart_;
    bool restart = deferredRuntimeRestart_;
    const bool waitForPreroll = deferredRuntimeWaitForPreroll_;
    const bool refreshPausedFrame = deferredPausedFrameRefresh_;
    const bool forcePausedFrameRefresh = deferredPausedFrameRefreshForceRestart_;
    ClearDeferredRuntimeStart();
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;

    const auto snapshot = controller_.Snapshot();
    if (snapshot.state == PlaybackState::Playing &&
        snapshot.media.has_value() &&
        (snapshot.media->hasVideo || snapshot.media->hasAudio)) {
        if (!startRequested) {
            restart = true;
        }
        if (StartRuntime(snapshot, restart, waitForPreroll)) {
            SetPlaybackTimer(true);
        }
    } else {
        SetPlaybackTimer(false);
        if (snapshot.state == PlaybackState::Paused &&
            refreshPausedFrame &&
            snapshot.media.has_value() &&
            snapshot.media->hasVideo &&
            backend_ == PlaybackBackend::NativeFfmpegD3D11) {
            RefreshPausedNativeFrame(snapshot, forcePausedFrameRefresh);
        }
    }

    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

bool MainWindow::CaptureLatestNativeFrame() {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 || !nativeVideoDecoder_) {
        return heldNativeFrame_.has_value();
    }

    NativeVideoFrame frame;
    if (nativeVideoDecoder_->LatestFrame(frame)) {
        heldNativeFrame_ = frame;
        return true;
    }
    return heldNativeFrame_.has_value();
}

void MainWindow::RenderHeldNativeFrame(const bool logRepaint) {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 || !d3dRenderer_) {
        return;
    }

    if (heldNativeFrame_.has_value() && heldNativeFrame_->HasContent()) {
        if (logRepaint) {
            LogApp(LogLevel::Debug,
                   L"repainting cached native frame pixels=" +
                       std::wstring(heldNativeFrame_->HasPixels() ? L"true" : L"false") +
                       L" texture=" +
                       std::wstring(heldNativeFrame_->HasD3DTexture() ? L"true" : L"false"));
        }
        d3dRenderer_->Render(*heldNativeFrame_);
    } else if (logRepaint) {
        LogApp(LogLevel::Debug, L"no cached native frame available to repaint");
    }
    heldNativeFrameNeedsPresent_ = false;
}

bool MainWindow::StartRuntime(const PlaybackSessionSnapshot& snapshot,
                              const bool restart,
                              const bool waitForPreroll) {
    if (!snapshot.media.has_value()) {
        return false;
    }

    pendingPausedFrameRefresh_ = false;
    ResetNativeBufferingWatchdog();
    if (RuntimeStopInProgress()) {
        QueueDeferredRuntimeStart(restart, waitForPreroll);
        return false;
    }
    if (RuntimeBackendsRunning()) {
        QueueDeferredRuntimeStart(restart, waitForPreroll);
        StopRuntimeAsync(false);
        return false;
    }

    if (backend_ == PlaybackBackend::EmbeddedFfplay) {
        const bool started = playbackPlayer_.Start(hwnd_,
                                                   PlaybackSurfaceBounds(),
                                                   snapshot.media->path,
                                                   snapshot.position,
                                                   snapshot.volume,
                                                   snapshot.media->dolbyVisionDetected);
        LogApp(started ? (restart ? LogLevel::Debug : LogLevel::Info) : LogLevel::Error,
               std::wstring(L"external ffplay playback ") + (restart ? L"restart=" : L"start=") + (started ? L"true" : L"false"));
        LogApp(LogLevel::Debug, L"ffplay command=" + playbackPlayer_.LastCommandLine());
        if (!started) {
            FailPlaybackRuntime(L"External FFplay failed to start");
            return false;
        }
        return true;
    }

    if (backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        return StartNativeRuntime(snapshot, restart, waitForPreroll);
    }

    // RawFrameBridge (--internal-playback): legacy ffmpeg raw-pipe path.
    bool videoStarted = true;
    bool audioStarted = true;
    if (snapshot.media->hasVideo) {
        videoStarted = videoDecoder_.Start(snapshot.media->path,
                                          snapshot.position,
                                          hwnd_,
                                          kVideoFrameReadyMessage);
    }
    const auto runtimeSettings = controller_.Settings();
    if (snapshot.media->hasAudio &&
        runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff) {
        audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
        audioStarted = audioPlayer_.Start(snapshot.media->path,
                                          snapshot.position,
                                          snapshot.volume,
                                          runtimeSettings.audio.selectedTrackIndex);
    }

    const LogLevel level = videoStarted && audioStarted ? (restart ? LogLevel::Debug : LogLevel::Info) : LogLevel::Error;
    LogApp(level,
           std::wstring(L"internal viewport playback ") +
               (restart ? L"restart" : L"start") +
               L" video=" + (videoStarted ? L"true" : L"false") +
               L" audio=" + (audioStarted ? L"true" : L"false"));
    if (snapshot.media->hasVideo) {
        LogApp(LogLevel::Debug, L"ffmpeg video command=" + videoDecoder_.LastCommandLine());
    }
    if (snapshot.media->hasAudio) {
        LogApp(LogLevel::Debug, L"wasapi audio=" + audioPlayer_.LastStatus());
    }
    if (!videoStarted || !audioStarted) {
        FailPlaybackRuntime(L"Internal playback runtime failed to start video=" +
                            std::wstring(videoStarted ? L"true" : L"false") +
                            L" audio=" + std::wstring(audioStarted ? L"true" : L"false"));
        return false;
    }
    return true;
}

void MainWindow::EnsureVideoHost() {
    if (videoHostReady_) {
        return;
    }
    if (!videoHost_) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance_;
        wc.lpfnWndProc = &MainWindow::VideoHostProc;
        wc.lpszClassName = kVideoHostClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);

        const RECT hostBounds = PlaybackSurfaceBounds();
        const int hostWidth = std::max(1, RectWidth(hostBounds));
        const int hostHeight = std::max(1, RectHeight(hostBounds));
        videoHost_ = CreateWindowExW(
            0,
            kVideoHostClassName,
            L"",
            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            hostBounds.left,
            hostBounds.top,
            hostWidth,
            hostHeight,
            hwnd_,
            nullptr,
            instance_,
            this);

        if (!videoHost_) {
            LogApp(LogLevel::Error, L"failed to create native video host window");
            return;
        }
    }

    if (!d3dRenderer_) {
        LogApp(LogLevel::Error, L"native d3d renderer unavailable");
        return;
    }

    if (!d3dRenderer_->Initialize(videoHost_)) {
        LogApp(LogLevel::Error, L"native d3d renderer initialization failed");
        ShowWindow(videoHost_, SW_HIDE);
        return;
    }

    videoHostReady_ = true;
    UpdateVideoHost();
    LogApp(LogLevel::Info, L"native video host window created");
}

bool MainWindow::StartNativeRuntime(const PlaybackSessionSnapshot& snapshot,
                                    const bool restart,
                                    const bool waitForPreroll) {
    nativeSeekPrerollHoldingAudio_ = false;
    bool videoStarted = true;
    bool audioStarted = true;
    bool preferDolbyVisionHdrOutput = false;
    bool enableDolbyVisionEnhancementDecode = false;
    const auto runtimeSettings = controller_.Settings();
    const bool useSharedNetworkDemuxer =
        snapshot.media->hasVideo &&
        snapshot.media->hasAudio &&
        IsNetworkMediaPath(snapshot.media->path) &&
        runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff;
    NativeAudioPacketSink audioPacketSink;
    if (useSharedNetworkDemuxer) {
        audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
        audioPacketSink.selectedTrackIndex = runtimeSettings.audio.selectedTrackIndex;
        audioPacketSink.start =
            [this, path = snapshot.media->path, volume = snapshot.volume](
                const AVCodecParameters* codecParameters,
                const AVRational timeBase,
                const std::chrono::milliseconds startPosition,
                const int streamIndex) {
                return audioPlayer_.StartPacketStream(path,
                                                      codecParameters,
                                                      timeBase,
                                                      startPosition,
                                                      volume,
                                                      streamIndex);
            };
        audioPacketSink.pushPacket = [this](const AVPacket* packet) {
            return audioPlayer_.QueuePacket(packet);
        };
        audioPacketSink.reset = [this](const std::chrono::milliseconds position) {
            audioPlayer_.ResetPacketStream(position);
        };
        audioPacketSink.endOfStream = [this]() {
            audioPlayer_.MarkPacketStreamEof();
        };
    }
    lastNativeStatsLog_ = {};
    if (snapshot.media->hasVideo) {
        if (d3dRenderer_) {
            const auto& settings = runtimeSettings;
            d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
            d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
            d3dRenderer_->ResetRenderStats();
        }
        MarkLayoutDirty();
        EnsureLayout();
        EnsureVideoHost();
        const auto& settings = runtimeSettings;
        preferDolbyVisionHdrOutput = snapshot.media->dolbyVisionDetected;
        const bool preferHardwareDecode = snapshot.media->selectedDecodePath == L"ffmpeg_d3d11va";
        enableDolbyVisionEnhancementDecode =
            MediaHasDolbyVisionEnhancementStream(snapshot.media) &&
            settings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off &&
            Cmv4ApproxActiveForPlayback(snapshot, settings.video);
        videoStarted = videoHostReady_ && nativeVideoDecoder_ &&
                       nativeVideoDecoder_->Start(snapshot.media->path,
                                                  snapshot.position,
                                                  hwnd_,
                                                  kNativeVideoFrameReadyMessage,
                                                  kNativeVideoDecodeFailedMessage,
                                                  [this]() {
                                                      return audioPlayer_.PlaybackClock();
                                                   },
                                                  preferHardwareDecode,
                                                  d3dRenderer_ ? d3dRenderer_->Device() : nullptr,
                                                  settings.video.selectedTrackIndex,
                                                  settings.subtitles.preferredLanguage,
                                                  settings.subtitles.selectedTrackIndex,
                                                  std::chrono::milliseconds{settings.subtitles.subtitleDelayMs},
                                                  settings.subtitles.externalSubtitleAutoLoad,
                                                  settings.subtitles.externalSubtitlePath,
                                                  false,
                                                  preferDolbyVisionHdrOutput,
                                                  enableDolbyVisionEnhancementDecode,
                                                  std::move(audioPacketSink));
        if (videoStarted) {
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
        }
    }
    const bool waitForEnhancedPreroll =
        waitForPreroll &&
        restart &&
        videoStarted &&
        snapshot.media->hasVideo &&
        nativeVideoDecoder_ &&
        enableDolbyVisionEnhancementDecode &&
        Cmv4ApproxActiveForPlayback(snapshot, runtimeSettings.video);
    if (waitForEnhancedPreroll) {
        const bool enhancedReady = nativeVideoDecoder_->WaitForEnhancementPreroll(
            snapshot.position,
            std::chrono::milliseconds{5000});
        LogApp(enhancedReady ? LogLevel::Debug : LogLevel::Warning,
               L"native enhanced preroll start ready=" + std::wstring(enhancedReady ? L"true" : L"false"));
    } else if (waitForPreroll && videoStarted && snapshot.media->hasVideo && snapshot.media->hasAudio && nativeVideoDecoder_) {
        nativeVideoDecoder_->WaitForPreroll(std::chrono::milliseconds{250},
                                            restart ? std::chrono::milliseconds{140}
                                                    : std::chrono::milliseconds{260});
    }
    if (snapshot.media->hasAudio &&
        runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff) {
        if (useSharedNetworkDemuxer) {
            audioStarted = videoStarted;
        } else {
            audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
            audioStarted = audioPlayer_.Start(snapshot.media->path,
                                              snapshot.position,
                                              snapshot.volume,
                                              runtimeSettings.audio.selectedTrackIndex);
        }
    }

    const LogLevel level = videoStarted && audioStarted ? (restart ? LogLevel::Debug : LogLevel::Info) : LogLevel::Error;
    LogApp(level,
           std::wstring(L"native ffmpeg/d3d11 playback ") +
               (restart ? L"restart" : L"start") +
               L" video=" + (videoStarted ? L"true" : L"false") +
               L" audio=" + (audioStarted ? L"true" : L"false") +
               L" shared_demux=" + (useSharedNetworkDemuxer ? L"true" : L"false"));
    if (snapshot.media->hasVideo && videoStarted) {
        LogApp(LogLevel::Debug, L"native decode path=" + nativeVideoDecoder_->Path().wstring());
        if (snapshot.media->dolbyVisionDetected) {
            const auto runtimeSettings = controller_.Settings();
            const bool cmv4Intermediate = Cmv4ApproxActiveForPlayback(snapshot, runtimeSettings.video);
            const bool finalHdrOutput = WantsDolbyVisionHdrOutput(runtimeSettings.video, CachedCapabilities().display);
            LogApp(LogLevel::Info,
                   L"dolby vision libplacebo intermediate=hdr_bt2020_pq final=" +
                       std::wstring(finalHdrOutput ? L"hdr10" : L"sdr") +
                       (cmv4Intermediate ? L" cmv4=on" : L" cmv4=off") +
                       (enableDolbyVisionEnhancementDecode ? L" el_decode=on" : L" el_decode=off"));
        }
    }
    if (snapshot.media->hasAudio) {
        LogApp(LogLevel::Debug, L"wasapi audio=" + audioPlayer_.LastStatus());
    }
    if (!videoStarted || !audioStarted) {
        FailPlaybackRuntime(L"Native playback runtime failed to start video=" +
                            std::wstring(videoStarted ? L"true" : L"false") +
                            L" audio=" + std::wstring(audioStarted ? L"true" : L"false"));
        return false;
    }
    return true;
}

bool MainWindow::SeekNativeRuntime(const PlaybackSessionSnapshot& snapshot) {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        snapshot.state != PlaybackState::Playing ||
        !snapshot.media.has_value() ||
        (!snapshot.media->hasVideo && !snapshot.media->hasAudio)) {
        return false;
    }
    ResetNativeBufferingWatchdog();
    const auto runtimeSettings = controller_.Settings();
    const bool useSharedNetworkDemuxer =
        snapshot.media->hasVideo &&
        snapshot.media->hasAudio &&
        IsNetworkMediaPath(snapshot.media->path) &&
        runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff;
    bool videoSeeked = true;
    if (snapshot.media->hasVideo) {
        videoSeeked = nativeVideoDecoder_ &&
                      nativeVideoDecoder_->IsRunning() &&
                      nativeVideoDecoder_->Seek(snapshot.position);
    }
    if (!videoSeeked) {
        nativeSeekPrerollHoldingAudio_ = false;
        return false;
    }

    bool audioSeeked = true;
    if (snapshot.media->hasAudio &&
        runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff) {
        if (useSharedNetworkDemuxer) {
            audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
            if (snapshot.media->hasVideo && audioPlayer_.IsRunning()) {
                audioPlayer_.HoldPacketStream(snapshot.position);
                nativeSeekPrerollHoldingAudio_ = true;
                LogApp(LogLevel::Debug,
                       L"native seek preroll hold audio position=" + FormatTimecode(snapshot.position));
            } else {
                nativeSeekPrerollHoldingAudio_ = false;
            }
            audioSeeked = true;
        } else if (audioPlayer_.IsRunning()) {
            nativeSeekPrerollHoldingAudio_ = false;
            audioSeeked = audioPlayer_.Seek(snapshot.position);
        } else {
            nativeSeekPrerollHoldingAudio_ = false;
            audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
            audioSeeked = audioPlayer_.Start(snapshot.media->path,
                                             snapshot.position,
                                             snapshot.volume,
                                             runtimeSettings.audio.selectedTrackIndex);
        }
    } else {
        nativeSeekPrerollHoldingAudio_ = false;
        audioPlayer_.Stop();
    }
    if (!audioSeeked) {
        nativeSeekPrerollHoldingAudio_ = false;
        return false;
    }

    pendingPausedFrameRefresh_ = false;
    nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
    heldNativeFrameNeedsPresent_ = heldNativeFrame_.has_value() && heldNativeFrame_->HasContent();
    lastNativeStatsLog_ = {};
    if (d3dRenderer_) {
        d3dRenderer_->ResetRenderStats();
    }
    if (webUiActive_ && webUiPlayerRouteActive_) {
        UpdateVideoHost();
    }
    UpdateBufferingOverlay();
    LogApp(LogLevel::Debug, L"native runtime seek position=" + FormatTimecode(snapshot.position));
    SetPlaybackTimer(true);
    return true;
}

bool MainWindow::PrepareNativeEnhancedPlaybackBeforePlay(const PlaybackSessionSnapshot& snapshot) {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        !snapshot.media.has_value() ||
        !snapshot.media->hasVideo ||
        !nativeVideoDecoder_) {
        return true;
    }

    const auto settings = controller_.Settings();
    const bool wantsEnhancedPlayback =
        NativeCmv4ToggleRequiresDecoderRestart(snapshot) &&
        Cmv4ApproxActiveForPlayback(snapshot, settings.video) &&
        settings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off;
    if (!wantsEnhancedPlayback) {
        return true;
    }

    const bool enableDolbyVisionEnhancementDecode =
        MediaHasDolbyVisionEnhancementStream(snapshot.media) &&
        settings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off &&
        Cmv4ApproxActiveForPlayback(snapshot, settings.video);
    if (!enableDolbyVisionEnhancementDecode) {
        return true;
    }

    audioPlayer_.Stop();
    pendingPausedFrameRefresh_ = false;
    nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
    heldNativeFrameNeedsPresent_ = false;
    lastNativeStatsLog_ = {};

    if (d3dRenderer_) {
        d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
        d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
        d3dRenderer_->ResetRenderStats();
    }
    MarkLayoutDirty();
    EnsureLayout();
    EnsureVideoHost();

    bool videoReadyToPreroll =
        nativeVideoDecoder_->IsRunning() &&
        !nativeVideoDecoder_->OneShotFrame() &&
        nativeVideoDecoder_->DolbyVisionEnhancementDecodeEnabled();

    if (videoReadyToPreroll) {
        nativeVideoDecoder_->SetPaused(true, snapshot.position);
        LogApp(LogLevel::Debug,
               L"native enhanced preroll gate reuse decoder position=" + FormatTimecode(snapshot.position));
    } else {
        StopRuntime(false);
        pendingPausedFrameRefresh_ = false;
        nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
        heldNativeFrameNeedsPresent_ = false;
        MarkLayoutDirty();
        EnsureLayout();
        EnsureVideoHost();

        const bool preferDolbyVisionHdrOutput = snapshot.media->dolbyVisionDetected;
        const bool preferHardwareDecode = snapshot.media->selectedDecodePath == L"ffmpeg_d3d11va";
        videoReadyToPreroll =
            videoHostReady_ &&
            nativeVideoDecoder_->Start(snapshot.media->path,
                                       snapshot.position,
                                       hwnd_,
                                       kNativeVideoFrameReadyMessage,
                                       0,
                                       [this]() {
                                           return audioPlayer_.PlaybackClock();
                                       },
                                       preferHardwareDecode,
                                       d3dRenderer_ ? d3dRenderer_->Device() : nullptr,
                                       settings.video.selectedTrackIndex,
                                       settings.subtitles.preferredLanguage,
                                       settings.subtitles.selectedTrackIndex,
                                       std::chrono::milliseconds{settings.subtitles.subtitleDelayMs},
                                       settings.subtitles.externalSubtitleAutoLoad,
                                       settings.subtitles.externalSubtitlePath,
                                       false,
                                       preferDolbyVisionHdrOutput,
                                       enableDolbyVisionEnhancementDecode);
        if (videoReadyToPreroll) {
            nativeVideoDecoder_->SetPaused(true, snapshot.position);
            UpdateVideoHost();
            LogApp(LogLevel::Debug,
                   L"native enhanced preroll gate decoder start position=" + FormatTimecode(snapshot.position));
        }
    }

    if (!videoReadyToPreroll) {
        LogApp(LogLevel::Warning, L"native enhanced preroll gate decoder unavailable");
        return false;
    }

    const bool enhancedReady = nativeVideoDecoder_->WaitForEnhancementPreroll(
        snapshot.position,
        std::chrono::milliseconds{5000},
        true);
    LogApp(enhancedReady ? LogLevel::Debug : LogLevel::Warning,
           L"native enhanced preroll gate ready=" + std::wstring(enhancedReady ? L"true" : L"false"));
    if (!enhancedReady) {
        nativeVideoDecoder_->SetPaused(true, snapshot.position);
        return false;
    }

    NativeVideoFrame frame;
    if (nativeVideoDecoder_->LatestFrame(frame) && frame.HasEnhancementYuv()) {
        heldNativeFrame_ = frame;
    }
    return true;
}

void MainWindow::RefreshPausedNativeFrame(const PlaybackSessionSnapshot& snapshot,
                                          const bool forceDecoderRestart) {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        !snapshot.media.has_value() ||
        !snapshot.media->hasVideo ||
        !nativeVideoDecoder_) {
        return;
    }
    if (RuntimeStopInProgress()) {
        QueuePausedNativeFrameRefresh(forceDecoderRestart);
        return;
    }

    if (!forceDecoderRestart && d3dRenderer_) {
        const auto settings = controller_.Settings();
        d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
        d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
        d3dRenderer_->ResetRenderStats();
    }

    if (!forceDecoderRestart && nativeVideoDecoder_->IsRunning()) {
        nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
        pendingPausedFrameRefresh_ = true;
        lastNativeStatsLog_ = {};
        nativeVideoDecoder_->SetPaused(true, snapshot.position);
        if (nativeVideoDecoder_->Seek(snapshot.position)) {
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
            SetPlaybackTimer(false);
            LogApp(LogLevel::Debug, L"paused native runtime seek position=" + FormatTimecode(snapshot.position));
            return;
        }
    }

    if (forceDecoderRestart) {
        LogApp(LogLevel::Debug, L"paused native frame refresh requires decoder restart");
    }
    if (RuntimeBackendsRunning()) {
        QueuePausedNativeFrameRefresh(forceDecoderRestart);
        StopRuntimeAsync(false);
        return;
    }
    nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
    pendingPausedFrameRefresh_ = true;
    heldNativeFrameNeedsPresent_ = false;
    lastNativeStatsLog_ = {};
    MarkLayoutDirty();
    EnsureLayout();
    EnsureVideoHost();

    const auto settings = controller_.Settings();
    if (d3dRenderer_) {
        d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
        d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
        d3dRenderer_->ResetRenderStats();
    }
    const bool preferDolbyVisionHdrOutput = snapshot.media->dolbyVisionDetected;
    const bool preferHardwareDecode = snapshot.media->selectedDecodePath == L"ffmpeg_d3d11va";
    const bool enableDolbyVisionEnhancementDecode =
        MediaHasDolbyVisionEnhancementStream(snapshot.media) &&
        settings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off &&
        Cmv4ApproxActiveForPlayback(snapshot, settings.video);
    const bool started = videoHostReady_ &&
                         d3dRenderer_ &&
                         nativeVideoDecoder_->Start(snapshot.media->path,
                                                    snapshot.position,
                                                    hwnd_,
                                                    kNativeVideoFrameReadyMessage,
                                                    0,
                                                    {},
                                                    preferHardwareDecode,
                                                    d3dRenderer_->Device(),
                                                    settings.video.selectedTrackIndex,
                                                    settings.subtitles.preferredLanguage,
                                                    settings.subtitles.selectedTrackIndex,
                                                    std::chrono::milliseconds{settings.subtitles.subtitleDelayMs},
                                                    settings.subtitles.externalSubtitleAutoLoad,
                                                    settings.subtitles.externalSubtitlePath,
                                                    true,
                                                    preferDolbyVisionHdrOutput,
                                                    enableDolbyVisionEnhancementDecode);
    if (started) {
        nativeFrameHoldVisible_ = true;
        MarkLayoutDirty();
        EnsureLayout();
        UpdateVideoHost();
        LogApp(LogLevel::Debug, L"paused native frame refresh start position=" + FormatTimecode(snapshot.position));
    } else {
        pendingPausedFrameRefresh_ = false;
        LogApp(LogLevel::Warning, L"paused native frame refresh failed position=" + FormatTimecode(snapshot.position));
    }
}

void MainWindow::OpenPath(const std::filesystem::path& path, const bool autoplay) {
    LogApp(LogLevel::Info, L"open path=" + path.wstring());
    ClearDeferredRuntimeStart();
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    if (RuntimeStopInProgress()) {
        controller_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
    } else {
        SetTemporaryPlaybackRate(1.0);
    }
    if (RuntimeBackendsRunning() || RuntimeStopInProgress()) {
        StopRuntimeAsync(true);
    } else {
        FinishRuntimeStopVisuals(true, true);
    }
    SetPlaybackTimer(false);
    RefreshCapabilityCache();
    const bool opened = controller_.OpenMedia(path, false);
    LogApp(opened ? LogLevel::Info : LogLevel::Error,
           std::wstring(L"open result=") + (opened ? L"true" : L"false") + L" autoplay=" + (autoplay ? L"true" : L"false"));
    if (opened) {
        auto settings = controller_.Settings();
        bool settingsChanged = false;
        if (!settings.subtitles.externalSubtitlePath.empty()) {
            settings.subtitles.externalSubtitlePath.clear();
            settingsChanged = true;
        }
        if (!settings.danmaku.externalDanmakuPath.empty()) {
            settings.danmaku.externalDanmakuPath.clear();
            settings.danmaku.enabled = false;
            settingsChanged = true;
        }
        if (settingsChanged) {
            controller_.ApplySettings(settings);
        }
        UpdateInspectorMediaLists(path);
        ApplyDefaultHdrControlsForCurrentMedia();
        MarkLayoutDirty();
        EnsureLayout();
    }
    if (opened && autoplay) {
        StartPlayback();
        return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::LoadRecentMedia() {
    recentMedia_.clear();

    std::ifstream input(RecentMediaPath(), std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (recentMedia_.size() < kMaxRecentMedia && std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto decoded = Utf8ToWide(line.c_str());
        if (decoded.empty()) {
            continue;
        }
        const auto normalizedPath = NormalizeListPath(decoded);
        std::error_code error;
        if (normalizedPath.empty() ||
            !std::filesystem::exists(normalizedPath, error) ||
            !IsMediaFilePath(normalizedPath) ||
            std::find(recentMedia_.begin(), recentMedia_.end(), normalizedPath) != recentMedia_.end()) {
            continue;
        }
        recentMedia_.push_back(normalizedPath);
    }

    if (!recentMedia_.empty()) {
        LogApp(LogLevel::Debug, L"recent media loaded count=" + std::to_wstring(recentMedia_.size()));
    }
}

void MainWindow::SaveRecentMedia() const {
    const auto path = RecentMediaPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        LogApp(LogLevel::Warning, L"recent media directory failed path=" + path.parent_path().wstring());
        return;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        LogApp(LogLevel::Warning, L"recent media save failed path=" + path.wstring());
        return;
    }

    for (const auto& item : recentMedia_) {
        output << WideToUtf8(item.wstring()) << '\n';
    }
}

void MainWindow::UpdateInspectorMediaLists(const std::filesystem::path& path) {
    if (IsNetworkMediaPath(path)) {
        currentFolderEntries_.clear();
        PostWebUiState();
        return;
    }

    const auto normalizedPath = NormalizeListPath(path);
    if (normalizedPath.empty()) {
        return;
    }

    recentMedia_.erase(std::remove(recentMedia_.begin(), recentMedia_.end(), normalizedPath), recentMedia_.end());
    recentMedia_.insert(recentMedia_.begin(), normalizedPath);
    if (recentMedia_.size() > kMaxRecentMedia) {
        recentMedia_.resize(kMaxRecentMedia);
    }
    SaveRecentMedia();

    currentFolderEntries_ = MediaFilesInFolder(normalizedPath);
    if (std::find(currentFolderEntries_.begin(), currentFolderEntries_.end(), normalizedPath) == currentFolderEntries_.end()) {
        currentFolderEntries_.insert(currentFolderEntries_.begin(), normalizedPath);
    }
}

void MainWindow::StartPlayback() {
    nativeSeekPrerollHoldingAudio_ = false;
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    const auto before = controller_.Snapshot();

    controller_.Play();
    const auto snapshot = controller_.Snapshot();
    LogApp(LogLevel::Info, L"start playback state=" + ToDisplayString(snapshot.state) + L" hasMedia=" + (snapshot.media.has_value() ? L"true" : L"false"));
    if (snapshot.state == PlaybackState::Playing &&
        snapshot.media.has_value() &&
        (snapshot.media->hasVideo || snapshot.media->hasAudio)) {
        const auto settings = controller_.Settings();
        const bool enhancedPlaybackNeedsRestart = NativeCmv4ToggleNeedsDecoderRefresh(snapshot, settings.video);
        const bool resumedNativeRuntime =
            before.state == PlaybackState::Paused &&
            backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
            !RuntimeStopInProgress() &&
            nativeVideoDecoder_ &&
            nativeVideoDecoder_->IsRunning() &&
            !nativeVideoDecoder_->OneShotFrame() &&
            !enhancedPlaybackNeedsRestart;
        if (resumedNativeRuntime) {
            if (d3dRenderer_) {
                d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
                d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
                d3dRenderer_->ResetRenderStats();
            }
            nativeVideoDecoder_->SetPaused(false, snapshot.position);
            pendingPausedFrameRefresh_ = false;
            nativeFrameHoldVisible_ = false;
            heldNativeFrameNeedsPresent_ = false;
            if (snapshot.media->hasAudio &&
                settings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff) {
                audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
                bool audioStarted = audioPlayer_.IsRunning() && audioPlayer_.ResumePacketStream();
                if (!audioStarted) {
                    audioStarted = audioPlayer_.Start(snapshot.media->path,
                                                      snapshot.position,
                                                      snapshot.volume,
                                                      settings.audio.selectedTrackIndex);
                }
                LogApp(audioStarted ? LogLevel::Debug : LogLevel::Warning,
                       L"native paused runtime resume audio=" + std::wstring(audioStarted ? L"true" : L"false"));
                if (!audioStarted) {
                    FailPlaybackRuntime(L"Native audio failed to resume");
                    return;
                }
            } else {
                audioPlayer_.Stop();
            }
        } else {
            if (before.state == PlaybackState::Paused &&
                backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                nativeVideoDecoder_ &&
                nativeVideoDecoder_->IsRunning()) {
                LogApp(LogLevel::Debug,
                       L"native paused runtime resume requires restart one_shot=" +
                           std::wstring(nativeVideoDecoder_->OneShotFrame() ? L"true" : L"false") +
                           L" enhanced_decoder=" +
                           std::wstring(nativeVideoDecoder_->DolbyVisionEnhancementDecodeEnabled() ? L"true" : L"false"));
            }
            RequestRuntimeStart(false, false);
        }
        SetPlaybackTimer(true);
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::PausePlayback() {
    nativeSeekPrerollHoldingAudio_ = false;
    ClearDeferredRuntimeStart();
    controller_.Pause();
    if (RuntimeStopInProgress()) {
        controller_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
    } else {
        SetTemporaryPlaybackRate(1.0);
    }
    const auto snapshot = controller_.Snapshot();
    if (RuntimeStopInProgress()) {
        SetPlaybackTimer(false);
    } else if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        nativeVideoDecoder_ &&
        nativeVideoDecoder_->IsRunning() &&
        d3dRenderer_) {
        const bool hadHeldFrame = heldNativeFrame_.has_value() && heldNativeFrame_->HasContent();
        nativeVideoDecoder_->SetPaused(true, snapshot.position);
        NativeVideoFrame frame;
        if (nativeVideoDecoder_->LatestFrame(frame)) {
            heldNativeFrame_ = frame;
            nativeFrameHoldVisible_ = true;
            heldNativeFrameNeedsPresent_ = false;
            if (!hadHeldFrame) {
                d3dRenderer_->Render(frame);
            }
            LogApp(LogLevel::Debug,
                   L"captured native pause freeze frame pixels=" +
                       std::wstring(frame.HasPixels() ? L"true" : L"false") +
                       L" rendered=" +
                       std::wstring(!hadHeldFrame ? L"true" : L"false"));
        }
        audioPlayer_.HoldPacketStream(snapshot.position);
    } else {
        StopRuntimeAsync(false);
    }
    SetPlaybackTimer(false);
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::TogglePlayback() {
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state == PlaybackState::Playing) {
        PausePlayback();
    } else {
        StartPlayback();
    }
}

void MainWindow::StopPlayback() {
    ClearDeferredRuntimeStart();
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    controller_.Stop();
    if (RuntimeStopInProgress()) {
        controller_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
    } else {
        SetTemporaryPlaybackRate(1.0);
    }
    StopRuntimeAsync(true);
    SetPlaybackTimer(false);
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::RestartPlaybackIfPlaying(const bool waitForPreroll) {
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state == PlaybackState::Playing &&
        snapshot.media.has_value() &&
        (snapshot.media->hasVideo || snapshot.media->hasAudio)) {
        RequestRuntimeStart(true, waitForPreroll);
    }
}

void MainWindow::SeekRelative(const std::chrono::milliseconds delta) {
    const auto before = controller_.Snapshot();
    controller_.SeekRelative(delta);
    const auto after = controller_.Snapshot();
    if (after.state == PlaybackState::Paused &&
        after.media.has_value() &&
        after.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(after);
    } else if (after.state == PlaybackState::Playing &&
               !(before.state == PlaybackState::Playing &&
                 !RuntimeStopInProgress() &&
                 SeekNativeRuntime(after))) {
        RestartPlaybackIfPlaying(false);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::SeekToPosition(const std::chrono::milliseconds position) {
    const auto before = controller_.Snapshot();
    controller_.Seek(position);
    const auto after = controller_.Snapshot();
    if (after.state == PlaybackState::Paused &&
        after.media.has_value() &&
        after.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(after);
    } else if (after.state == PlaybackState::Playing &&
               !(before.state == PlaybackState::Playing &&
                 !RuntimeStopInProgress() &&
                 SeekNativeRuntime(after))) {
        RestartPlaybackIfPlaying(false);
    }
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::SeekFromProgress(const int x) {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value() || RectWidth(progress_) <= 0) {
        return;
    }
    const double ratio = std::clamp(
        static_cast<double>(x - progress_.left) / static_cast<double>(RectWidth(progress_)),
        0.0,
        1.0);
    const auto duration = snapshot.media->duration;
    SeekToPosition(std::chrono::milliseconds(static_cast<long long>(duration.count() * ratio)));
}

void MainWindow::ToggleFullscreen() {
    if (!fullscreen_) {
        previousStyle_ = GetWindowLongW(hwnd_, GWL_STYLE);
        previousExStyle_ = GetWindowLongW(hwnd_, GWL_EXSTYLE);
        previousPlacement_.length = sizeof(previousPlacement_);
        GetWindowPlacement(hwnd_, &previousPlacement_);

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitorInfo);
        SetWindowLongW(hwnd_, GWL_STYLE, previousStyle_ & ~WS_OVERLAPPEDWINDOW);
        SetWindowLongW(hwnd_, GWL_EXSTYLE, previousExStyle_ & ~WS_EX_WINDOWEDGE);
        SetWindowPos(hwnd_,
                     HWND_TOP,
                     monitorInfo.rcMonitor.left,
                     monitorInfo.rcMonitor.top,
                     RectWidth(monitorInfo.rcMonitor),
                     RectHeight(monitorInfo.rcMonitor),
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = true;
        fullscreenTransportVisible_ = false;
        fullscreenTransportAmount_ = 0.0;
        fullscreenTransportStartAmount_ = fullscreenTransportAmount_;
        fullscreenTransportTarget_ = fullscreenTransportAmount_;
        fullscreenTransportLastShownAt_ = {};
        POINT cursor{};
        hasLastFullscreenCursorClient_ = GetCursorPos(&cursor) && ScreenToClient(hwnd_, &cursor);
        if (hasLastFullscreenCursorClient_) {
            lastFullscreenCursorClient_ = cursor;
        }
        SetTimer(hwnd_, kFullscreenChromeHideTimer, 120, nullptr);
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, previousStyle_);
        SetWindowLongW(hwnd_, GWL_EXSTYLE, previousExStyle_);
        SetWindowPlacement(hwnd_, &previousPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
        fullscreenTransportVisible_ = false;
        fullscreenTransportAmount_ = 0.0;
        fullscreenTransportStartAmount_ = 0.0;
        fullscreenTransportTarget_ = 0.0;
        hasLastFullscreenCursorClient_ = false;
        KillTimer(hwnd_, kFullscreenChromeHideTimer);
        if (fullscreenOverlay_) {
            ShowWindow(fullscreenOverlay_, SW_HIDE);
        }
    }
    ApplyWindowChrome();
    MarkLayoutDirty();
    EnsureLayout();
    if (fullscreen_ &&
        hasLastFullscreenCursorClient_ &&
        IsFullscreenTransportActivationPoint(lastFullscreenCursorClient_)) {
        ShowFullscreenTransport(L"enter_fullscreen_activation");
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

}  // namespace anvil::app
