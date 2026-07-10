#include "AnvilPlayer/App/webui_root.h"

#include <system_error>

namespace anvil::app {

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

}  // namespace anvil::app
