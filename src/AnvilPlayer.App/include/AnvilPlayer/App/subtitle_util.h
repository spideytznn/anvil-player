#pragma once

// Subtitle text parsing and external-file discovery helpers.
// Extracted from ffmpeg_video_decoder.cpp's anonymous namespace: these are
// pure functions (no decoder state) for finding, scoring, and parsing
// external subtitle files and converting decoded AVSubtitle rects into the
// app's NativeSubtitleBitmap / plain-text forms.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Forward-declare FFmpeg types in the global namespace (they are C types).
struct AVSubtitle;
struct AVPacket;
struct AVRational;

namespace anvil::app {

struct NativeSubtitleBitmap;

// --- Codec / extension detection ---

bool IsSupportedExternalSubtitleExtension(const std::filesystem::path& path);
bool IsAssSubtitleExtension(const std::filesystem::path& path);
bool IsAssSubtitleCodec(unsigned int codecId);

// --- Language preference helpers (used by subtitle scoring & stream selection) ---

bool IsAutoLanguage(const std::wstring& value);
bool LanguageMatches(const std::wstring& desired, const std::wstring& actual);

// --- External subtitle file discovery ---

// Finds the best-scoring external subtitle file next to the media, honouring
// the preferred language tag. Returns nullopt if none found.
std::optional<std::filesystem::path> FindExternalSubtitleFile(
    const std::filesystem::path& mediaPath,
    const std::wstring& preferredLanguage);

// --- Subtitle text parsing ---

std::wstring NormalizeSubtitleText(std::wstring text);
std::wstring PlainSubtitleText(const char* utf8, bool ass);
std::wstring SubtitleTextFromDecoded(const AVSubtitle& subtitle);

// --- Subtitle bitmap extraction ---

bool ValidSubtitleCanvas(int width, int height);
std::optional<std::pair<int, int>> PgsCanvasFromPacket(const AVPacket* packet);
std::vector<NativeSubtitleBitmap> SubtitleBitmapsFromDecoded(
    const AVSubtitle& subtitle,
    int canvasWidth,
    int canvasHeight,
    std::uint64_t& serial);

// PTS of a decoded subtitle, in milliseconds.
std::chrono::milliseconds SubtitlePacketBasePts(
    const AVSubtitle& subtitle,
    const AVPacket* packet,
    AVRational streamTimeBase);

}  // namespace anvil::app
