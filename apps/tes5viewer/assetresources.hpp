#ifndef OPENMW_APPS_TES5VIEWER_ASSETRESOURCES_H
#define OPENMW_APPS_TES5VIEWER_ASSETRESOURCES_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ToUTF8
{
    class StatelessUtf8Encoder;
}
namespace VFS
{
    class Manager;
}

namespace TES5Viewer
{
    struct AssetArchive
    {
        std::filesystem::path path;
        std::string kind;
        std::string plugin;
    };

    struct AssetPlan
    {
        std::vector<AssetArchive> archives;
        std::vector<std::filesystem::path> looseData;
        bool dataResourcesIncluded = true;
    };

    std::vector<std::string> coreRenderArchives();

    // Resolve sources without opening archives. Physical duplicates retain their last position.
    // Default includes all top-level Data BSAs and Data loose resources. Isolated mode limits
    // sources to core/active-plugin/explicit archives and explicitly supplied loose roots.
    AssetPlan buildAssetPlan(const std::filesystem::path& data, const std::vector<std::filesystem::path>& contents,
        const std::vector<std::string>& archives, const std::vector<std::filesystem::path>& looseData,
        bool isolatedAssets = false);

    std::unique_ptr<VFS::Manager> mountAssetResources(
        const AssetPlan& plan, const ToUTF8::StatelessUtf8Encoder* encoder = nullptr);
    std::shared_ptr<VFS::Manager> mountRuntimeAssets(const AssetPlan& plan);

    std::string modelAssetPath(std::string_view model);
}

#endif
