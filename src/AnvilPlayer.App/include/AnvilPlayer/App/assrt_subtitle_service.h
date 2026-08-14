#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace anvil::app {

struct AssrtSubtitleCandidate {
    int id = 0;
    std::wstring name;
    std::wstring videoName;
    std::wstring language;
    std::wstring format;
    std::wstring releaseSite;
    std::wstring uploadTime;
    double score = 0.0;
};

struct AssrtSubtitleSearchResult {
    std::vector<AssrtSubtitleCandidate> items;
    std::wstring error;
};

struct AssrtSubtitleDownloadResult {
    std::filesystem::path path;
    std::wstring fileName;
    std::wstring error;
};

// The ASSRT token is encrypted for the current Windows user with DPAPI before
// it is persisted. Passing an empty token clears the saved credential.
bool SaveAssrtToken(const std::wstring& token);
std::wstring LoadAssrtToken();

AssrtSubtitleSearchResult SearchAssrtSubtitles(
    const std::wstring& token,
    const std::wstring& query);

AssrtSubtitleDownloadResult DownloadAssrtSubtitle(
    const std::wstring& token,
    int subtitleId,
    const std::wstring& mediaName);

}  // namespace anvil::app
