#pragma once

#include <filesystem>

namespace anvil::app {

// True when `root` contains a built web UI (index.html + assets/).
bool HasBuiltWebUiRoot(const std::filesystem::path& root);

// Searches upward from `base` for the nearest built web UI directory.
// Returns an empty path when nothing is found.
std::filesystem::path FindBuiltWebUiRootNear(std::filesystem::path base);

}  // namespace anvil::app
