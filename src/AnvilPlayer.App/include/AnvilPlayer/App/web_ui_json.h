#pragma once

// Hand-rolled JSON helpers for the Web UI message bridge.
// main_window.cpp builds/emits state JSON and parses incoming command JSON
// without a JSON library; these primitives implement the escaping and the
// narrow field readers used across HandleWebUiMessage / BuildWebUiStateJson.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace anvil::app {

// Escapes a wide string for embedding inside a JSON string literal (without
// the surrounding quotes).
std::wstring JsonEscape(const std::wstring& value);

// Substring test used by the command dispatcher.
bool MessageContains(std::wstring_view message, const wchar_t* needle);

// Counts non-overlapping occurrences of needle in text.
std::size_t CountOccurrences(std::wstring_view text, std::wstring_view needle);

// Reads a numeric field "field":<number> from a JSON blob. Best-effort parse
// that stops at the first non-numeric character.
std::optional<double> ReadJsonNumber(std::wstring_view message, const wchar_t* field);

// Reads a string field "field":"..." from a JSON blob, honouring backslash
// escapes (\n \t \uXXXX ...). Returns nullopt if the field is absent or
// malformed.
std::optional<std::wstring> ReadJsonString(std::wstring_view message, const wchar_t* field);

// Extracts a balanced JSON object/array value for "field":{...} or "field":[...].
// Tracks brace/bracket depth and respects quoted strings so nested objects and
// strings containing delimiters do not confuse the scan. Returns nullopt when
// the field is absent or its value is not an object/array.
std::optional<std::wstring> ReadJsonObject(std::wstring_view message, const wchar_t* field);

}  // namespace anvil::app
