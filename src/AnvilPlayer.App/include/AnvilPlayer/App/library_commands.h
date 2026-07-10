#pragma once

// Library-source command helpers shared between LibraryWindow and any other
// window that needs to enumerate/scan media sources (local folders, SMB,
// WebDAV). Extracted verbatim from main_window.cpp's anonymous namespace so a
// standalone library window can reuse the same scanning, normalization, and
// JSON-building pipeline without duplication.

#include "AnvilPlayer/App/app_messages.h"  // kLocalFolderScanResultMessage

#include <windows.h>
#include <winhttp.h>  // INTERNET_PORT, INTERNET_DEFAULT_HTTP_PORT

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil::app {

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

// --- String / path normalization ---

std::wstring LowerCopy(std::wstring value);
bool IsNetworkMediaPath(const std::filesystem::path& path);
std::filesystem::path NormalizeListPath(const std::filesystem::path& path);
std::wstring TrimWhitespace(std::wstring value);
bool StartsWithInsensitive(const std::wstring& value, const std::wstring& prefix);
bool ContainsInsensitive(const std::wstring& value, const std::wstring& needle);

// --- SMB ---

std::wstring NormalizeSmbPathText(std::wstring value);
std::vector<std::wstring> SmbUncParts(const std::wstring& path);
std::wstring SmbServerRootFromHost(const std::wstring& host);
std::wstring SmbShareRootFromPath(const std::filesystem::path& path);
std::wstring WindowsErrorMessage(DWORD code);
std::optional<std::wstring> ConnectSmbRemote(std::wstring remoteName, const SmbCredentials& credentials);
std::optional<std::wstring> ConnectSmbPathForAccess(const std::filesystem::path& path, const SmbCredentials& credentials);
std::vector<SmbDirectoryEntry> ListSmbShares(const std::wstring& host, const SmbCredentials& credentials, std::wstring& errorMessage);
std::vector<SmbDirectoryEntry> ListSmbChildDirectories(const std::filesystem::path& path, const SmbCredentials& credentials, std::wstring& errorMessage);

// --- Local folder scanning ---

bool IsMediaFilePath(const std::filesystem::path& path);
std::vector<std::filesystem::path> MediaFilesInFolder(const std::filesystem::path& mediaPath);
std::int64_t LastWriteTimeUnixMilliseconds(const std::filesystem::path& path);
std::vector<LocalFolderScanItem> ScanMediaFilesInLocalFolder(const std::filesystem::path& root, bool& truncated);

// --- WebDAV ---

struct WebDavUrlParts {
    std::wstring scheme;
    std::wstring host;
    std::wstring pathAndQuery;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
    bool secure = false;
};

std::optional<WebDavUrlParts> CrackWebDavUrl(const std::wstring& url);
std::wstring WebDavOrigin(const std::wstring& url);
std::wstring NormalizeWebDavUrlText(std::wstring url, bool ensureTrailingSlash = true);
std::wstring AbsoluteWebDavHref(const std::wstring& baseUrl, std::wstring href);
std::wstring WebDavChildUrlFromHref(const std::wstring& baseUrl, const std::wstring& href, bool isDirectory);
bool SameWebDavResource(const std::wstring& left, const std::wstring& right);
std::wstring WebDavNameFromUrl(std::wstring url, const std::wstring& fallback);
std::wstring WebDavExtension(const std::wstring& url);
std::wstring CredentialedWebDavUrl(const std::wstring& url, const std::wstring& username, const std::wstring& password);
std::vector<WebDavEntry> ParseWebDavEntries(const std::wstring& baseUrl, const std::string& xmlUtf8);
std::vector<WebDavEntry> WebDavPropFind(const std::wstring& url, const std::wstring& username, const std::wstring& password, std::wstring& errorMessage);
std::vector<SmbDirectoryEntry> ListWebDavDirectories(const std::wstring& url, const std::wstring& username, const std::wstring& password, std::wstring& errorMessage);
std::vector<LocalFolderScanItem> ScanWebDavMediaFiles(const std::wstring& rootUrl, const std::wstring& username, const std::wstring& password, bool& truncated, std::wstring& errorMessage);

// --- JSON builders (library result messages) ---

std::wstring MediaPathItemsJson(const std::vector<std::filesystem::path>& paths, std::size_t maxCount);
std::wstring LocalFolderScanItemsJson(const std::vector<LocalFolderScanItem>& items);
std::wstring LocalFolderJsonPrefix(const wchar_t* type, const std::filesystem::path& folder);
std::wstring LocalFolderPickedJson(const std::filesystem::path& folder, const std::vector<LocalFolderScanItem>& items, bool truncated, bool scanPending);
std::wstring LocalFolderScanCompletedJson(const std::filesystem::path& folder, const std::vector<LocalFolderScanItem>& items, bool truncated);
std::wstring LocalFolderScanFailedJson(const std::filesystem::path& folder, const std::wstring& message);
std::wstring SmbDirectoriesJson(const std::vector<SmbDirectoryEntry>& directories);
std::wstring SmbDirectoryListedJson(const std::wstring& requestId, const std::filesystem::path& path, const std::vector<SmbDirectoryEntry>& directories);
std::wstring SmbDirectoryFailedJson(const std::wstring& requestId, const std::filesystem::path& path, const std::wstring& message);
std::wstring WebDavDirectoryListedJson(const std::wstring& requestId, const std::wstring& path, const std::vector<SmbDirectoryEntry>& directories);
std::wstring WebDavDirectoryFailedJson(const std::wstring& requestId, const std::wstring& path, const std::wstring& message);

// --- Async scan workers (post results to targetWindow via kLocalFolderScanResultMessage) ---

void StartLocalFolderScanAsync(HWND targetWindow, std::filesystem::path folder, SmbCredentials credentials = {});
void StartSmbDirectoryListAsync(HWND targetWindow, std::wstring requestId, std::wstring host, std::wstring path, SmbCredentials credentials);
void StartWebDavDirectoryListAsync(HWND targetWindow, std::wstring requestId, std::wstring url, std::wstring username, std::wstring password);
void StartWebDavScanAsync(HWND targetWindow, std::wstring url, std::wstring username, std::wstring password);

}  // namespace anvil::app
