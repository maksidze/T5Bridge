#pragma once
#include "exportpreview.hpp"
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace TES5Viewer
{
    // Versioned derived data only; no VM or mutable gameplay state is stored.
    std::string runtimeSourceKey(const std::filesystem::path& data,
        const std::filesystem::path& resources, const std::vector<std::filesystem::path>& plugins);
    std::string runtimeCacheName(std::string_view key);
    bool readRuntimeCache(const std::filesystem::path& directory, std::string_view key, PreviewBundle& bundle) noexcept;
    void writeRuntimeCache(const std::filesystem::path& directory, std::string_view key,
        const PreviewBundle& bundle) noexcept;
}
